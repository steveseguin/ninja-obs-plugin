const fs = require("fs");
const path = require("path");

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

class ObsWebSocketClient {
  constructor(url) {
    this.url = url;
    this.socket = null;
    this.requestId = 0;
    this.pending = new Map();
    this.identified = false;
    this.requestTimeoutMs = Number(process.env.OBS_WEBSOCKET_REQUEST_TIMEOUT_MS || 20000);
  }

  async connect() {
    await new Promise((resolve, reject) => {
      const socket = new WebSocket(this.url, "obswebsocket.json");
      this.socket = socket;

      socket.addEventListener("open", () => resolve());
      socket.addEventListener("error", (error) => reject(error));
      socket.addEventListener("close", () => {
        for (const pending of this.pending.values()) {
          pending.reject(new Error("obs-websocket connection closed"));
        }
        this.pending.clear();
      });
      socket.addEventListener("message", (event) => {
        try {
          const message = JSON.parse(event.data.toString());
          if (message.op === 0) {
            socket.send(
              JSON.stringify({
                op: 1,
                d: {
                  rpcVersion: 1,
                  eventSubscriptions: 0,
                },
              })
            );
            return;
          }

          if (message.op === 2) {
            this.identified = true;
            return;
          }

          if (message.op !== 7) {
            return;
          }

          const requestId = message.d && message.d.requestId;
          const pending = requestId ? this.pending.get(requestId) : null;
          if (!pending) {
            return;
          }

          this.pending.delete(requestId);
          if (message.d.requestStatus && message.d.requestStatus.result) {
            pending.resolve(message.d.responseData || {});
            return;
          }

          const comment =
            (message.d.requestStatus && message.d.requestStatus.comment) || "OBS request failed";
          pending.reject(new Error(`${message.d.requestType}: ${comment}`));
        } catch (error) {
          reject(error);
        }
      });
    });

    for (let i = 0; i < 50 && !this.identified; i += 1) {
      await sleep(100);
    }

    if (!this.identified) {
      throw new Error("Timed out waiting for obs-websocket identify handshake");
    }
  }

  async request(requestType, requestData = {}) {
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
      throw new Error("obs-websocket is not connected");
    }

    const requestId = `req-${++this.requestId}`;
    const response = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pending.delete(requestId);
        reject(new Error(`${requestType}: Timed out after ${this.requestTimeoutMs}ms`));
      }, this.requestTimeoutMs);

      this.pending.set(requestId, {
        resolve: (value) => {
          clearTimeout(timeout);
          resolve(value);
        },
        reject: (error) => {
          clearTimeout(timeout);
          reject(error);
        },
      });
    });

    this.socket.send(
      JSON.stringify({
        op: 6,
        d: {
          requestType,
          requestId,
          requestData,
        },
      })
    );
    return response;
  }

  async close() {
    if (!this.socket) {
      return;
    }
    this.socket.close();
    this.socket = null;
  }
}

function parseBoolean(value, fallback = false) {
  if (value === undefined) {
    return fallback;
  }
  const normalized = String(value).trim().toLowerCase();
  return normalized === "1" || normalized === "true" || normalized === "yes" || normalized === "on";
}

function logStep(message) {
  console.error(`[obs-edge-stress] ${message}`);
}

function copyStats(stats) {
  const keys = [
    "cpuUsage",
    "memoryUsage",
    "activeFps",
    "averageFrameRenderTime",
    "renderSkippedFrames",
    "renderTotalFrames",
    "outputSkippedFrames",
    "outputTotalFrames",
  ];
  const result = {};
  for (const key of keys) {
    if (typeof stats[key] === "number" && Number.isFinite(stats[key])) {
      result[key] = stats[key];
    }
  }
  return result;
}

function buildCases(streamId, password, roomId) {
  const longToken = "edge_" + "x".repeat(180);
  const valid = {
    stream_id: streamId,
    password,
    room_id: roomId,
    use_native_receiver: true,
    enable_data_channel: true,
    auto_reconnect: true,
    force_turn: false,
    custom_ice_servers: "",
    salt: "",
    wss_host: "",
    width: 1280,
    height: 720,
  };

  return [
    { name: "native-baseline", settings: valid, waitMs: 3000, skipUpdate: true },
    { name: "negative-dimensions", settings: { width: -1, height: -999 }, waitMs: 1000 },
    { name: "zero-dimensions", settings: { width: 0, height: 0 }, waitMs: 1000 },
    { name: "huge-dimensions", settings: { width: 999999999, height: 999999999 }, waitMs: 1000 },
    { name: "tiny-dimensions", settings: { width: 1, height: 1 }, waitMs: 1000 },
    { name: "browser-wrapper-uhd", settings: { use_native_receiver: false, width: 4096, height: 2160 }, waitMs: 2500 },
    { name: "native-wrapper-restore", settings: { use_native_receiver: true, width: 1280, height: 720 }, waitMs: 2500 },
    { name: "blank-stream-id", settings: { stream_id: "", password: "", room_id: "" }, waitMs: 1000 },
    {
      name: "unicode-password-room",
      settings: {
        stream_id: longToken,
        password: "edge $/#?&= snowman",
        room_id: "room with spaces",
        salt: "custom salt/#",
      },
      waitMs: 1000,
    },
    {
      name: "bad-ice-force-turn",
      settings: {
        stream_id: streamId,
        password,
        room_id: roomId,
        custom_ice_servers: "https://not-ice.example\nturn:turn.example.invalid:3478|user|credential\n???",
        force_turn: true,
      },
      waitMs: 1500,
    },
    { name: "data-channel-off", settings: { enable_data_channel: false, force_turn: false }, waitMs: 1000 },
    { name: "data-channel-on", settings: { enable_data_channel: true, auto_reconnect: true }, waitMs: 1000 },
    { name: "valid-restore", settings: valid, waitMs: 3000 },
  ];
}

function buildInputReference(inputName, inputUuid) {
  return inputUuid ? { inputUuid } : { inputName };
}

async function applyCase(client, inputName, inputUuid, sceneName, sceneItemId, edgeCase) {
  logStep(`case ${edgeCase.name}`);
  const beforeStats = copyStats(await client.request("GetStats").catch(() => ({})));

  if (!edgeCase.skipUpdate) {
    await client.request("SetInputSettings", {
      ...buildInputReference(inputName, inputUuid),
      inputSettings: edgeCase.settings,
      overlay: true,
    });
  }

  if (edgeCase.transform) {
    await client.request("SetSceneItemTransform", {
      sceneName,
      sceneItemId,
      sceneItemTransform: edgeCase.transform,
    });
  }

  await sleep(edgeCase.waitMs || 500);

  const inputSettings = await client.request("GetInputSettings", buildInputReference(inputName, inputUuid)).catch(() => null);
  const afterStats = copyStats(await client.request("GetStats").catch(() => ({})));

  return {
    name: edgeCase.name,
    waitMs: edgeCase.waitMs || 500,
    requestedSettings: edgeCase.settings,
    observedSettings: inputSettings && inputSettings.inputSettings ? inputSettings.inputSettings : null,
    statsBefore: beforeStats,
    statsAfter: afterStats,
  };
}

async function runRapidMutationLoop(client, inputName, inputUuid, sceneName, sceneItemId, streamId, password, roomId) {
  const iterations = Number(process.env.VDONINJA_EDGE_RAPID_ITERATIONS || 30);
  const waitMs = Number(process.env.VDONINJA_EDGE_RAPID_WAIT_MS || 150);
  const dimensions = [
    [1280, 720],
    [-1, -1],
    [0, 0],
    [1, 1],
    [4096, 2160],
    [999999999, 999999999],
    [320, 240],
  ];
  const startedAt = Date.now();
  let toggle = false;

  for (let index = 0; index < iterations; index += 1) {
    const [width, height] = dimensions[index % dimensions.length];
    toggle = !toggle;
    await client.request("SetInputSettings", {
      ...buildInputReference(inputName, inputUuid),
      inputSettings: {
        stream_id: index % 9 === 0 ? "" : streamId,
        password: index % 7 === 0 ? "edge rapid /#$" : password,
        room_id: index % 5 === 0 ? "rapid-room" : roomId,
        use_native_receiver: toggle,
        enable_data_channel: index % 4 !== 0,
        auto_reconnect: index % 3 !== 0,
        width,
        height,
      },
      overlay: true,
    });

    await client.request("SetSceneItemEnabled", {
      sceneName,
      sceneItemId,
      sceneItemEnabled: index % 6 !== 0,
    });

    await client.request("SetSceneItemTransform", {
      sceneName,
      sceneItemId,
      sceneItemTransform: {
        positionX: (index * 37) % 640,
        positionY: (index * 53) % 360,
        scaleX: index % 2 === 0 ? 0.5 : 1.0,
        scaleY: index % 3 === 0 ? 0.5 : 1.0,
        rotation: (index * 17) % 360,
      },
    });

    if (waitMs > 0) {
      await sleep(waitMs);
    }
  }

  await client.request("SetSceneItemEnabled", {
    sceneName,
    sceneItemId,
    sceneItemEnabled: true,
  });

  return {
    iterations,
    waitMs,
    durationMs: Date.now() - startedAt,
  };
}

async function main() {
  const websocketUrl = process.env.OBS_WEBSOCKET_URL || "ws://127.0.0.1:4455";
  const streamId = process.env.VDONINJA_STREAM_ID || process.argv[2] || "codexEdgeStress";
  const password = process.env.VDONINJA_PASSWORD || process.argv[3] || "false";
  const roomId = process.env.VDONINJA_ROOM_ID || process.argv[4] || "";
  const keepScene = parseBoolean(process.env.VDONINJA_KEEP_SCENE, false);
  const stamp = Date.now();
  const sceneName = `Codex Edge Stress ${stamp}`;
  const inputName = `Codex Edge VDO Source ${stamp}`;
  const reportPath = path.resolve(process.cwd(), "artifacts", `obs-edge-stress-${stamp}.json`);
  const client = new ObsWebSocketClient(websocketUrl);

  let previousSceneName = null;
  let createdScene = false;
  let createdInput = false;
  let sceneItemId = null;
  let inputUuid = null;

  try {
    logStep(`connecting to ${websocketUrl}`);
    await client.connect();

    const version = await client.request("GetVersion");
    const kinds = await client.request("GetInputKindList", { unversioned: false });
    if (!Array.isArray(kinds.inputKinds) || !kinds.inputKinds.includes("vdoninja_source")) {
      throw new Error("OBS does not have the vdoninja_source input kind registered");
    }

    const currentProgram = await client.request("GetCurrentProgramScene").catch(() => ({}));
    previousSceneName = currentProgram.currentProgramSceneName || null;

    logStep(`creating scene ${sceneName}`);
    await client.request("CreateScene", { sceneName });
    createdScene = true;
    await client.request("SetCurrentProgramScene", { sceneName });

    logStep(`creating input ${inputName}`);
    const createResponse = await client.request("CreateInput", {
      sceneName,
      inputName,
      inputKind: "vdoninja_source",
      inputSettings: {
        stream_id: streamId,
        password,
        room_id: roomId,
        use_native_receiver: true,
        enable_data_channel: true,
        auto_reconnect: true,
        width: 1280,
        height: 720,
      },
      sceneItemEnabled: true,
    });
    createdInput = true;
    sceneItemId = createResponse.sceneItemId;
    inputUuid = createResponse.inputUuid || createResponse.inputUUID || null;
    logStep(`created sceneItemId=${sceneItemId} inputUuid=${inputUuid || "(none)"}`);
    await sleep(500);

    if (sceneItemId === undefined || sceneItemId === null) {
      throw new Error("OBS did not return sceneItemId for edge stress input");
    }

    const caseResults = [];
    for (const edgeCase of buildCases(streamId, password, roomId)) {
      caseResults.push(await applyCase(client, inputName, inputUuid, sceneName, sceneItemId, edgeCase));
    }

    logStep("running rapid mutation loop");
    const rapidMutation = await runRapidMutationLoop(
      client,
      inputName,
      inputUuid,
      sceneName,
      sceneItemId,
      streamId,
      password,
      roomId
    );

    logStep("restoring final valid source settings");
    await client.request("SetInputSettings", {
      ...buildInputReference(inputName, inputUuid),
      inputSettings: {
        stream_id: streamId,
        password,
        room_id: roomId,
        use_native_receiver: true,
        enable_data_channel: true,
        auto_reconnect: true,
        force_turn: false,
        custom_ice_servers: "",
        salt: "",
        wss_host: "",
        width: 1280,
        height: 720,
      },
      overlay: true,
    });
    await sleep(Number(process.env.VDONINJA_EDGE_FINAL_WAIT_MS || 2500));

    const finalSettings = await client.request("GetInputSettings", buildInputReference(inputName, inputUuid)).catch(() => null);
    const finalStats = copyStats(await client.request("GetStats").catch(() => ({})));
    const report = {
      ok: true,
      websocketUrl,
      streamId,
      obsVersion: version.obsVersion,
      obsWebSocketVersion: version.obsWebSocketVersion,
      sceneName,
      inputName,
      inputUuid,
      caseResults,
      rapidMutation,
      finalSettings: finalSettings && finalSettings.inputSettings ? finalSettings.inputSettings : null,
      finalStats,
    };

    fs.mkdirSync(path.dirname(reportPath), { recursive: true });
    fs.writeFileSync(reportPath, JSON.stringify(report, null, 2));
    console.log(JSON.stringify({ ...report, reportPath }, null, 2));
  } finally {
    if (client.socket && client.socket.readyState === WebSocket.OPEN) {
      try {
        if (!keepScene) {
          if (createdInput) {
            await client.request("RemoveInput", buildInputReference(inputName, inputUuid)).catch(() => {});
          }
          if (createdScene && previousSceneName) {
            await client.request("SetCurrentProgramScene", { sceneName: previousSceneName }).catch(() => {});
          }
          if (createdScene) {
            await client.request("RemoveScene", { sceneName }).catch(() => {});
          }
        }
      } finally {
        await client.close();
      }
    }
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
