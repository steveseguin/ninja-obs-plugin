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

function parseNonNegativeInteger(value, fallback = 0) {
  if (value === undefined || value === null || value === "") {
    return fallback;
  }
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    return fallback;
  }
  return Math.max(0, Math.trunc(parsed));
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

function pickVideoSettings(settings) {
  const keys = ["baseWidth", "baseHeight", "outputWidth", "outputHeight", "fpsNumerator", "fpsDenominator"];
  const result = {};
  for (const key of keys) {
    if (typeof settings[key] === "number" && Number.isFinite(settings[key])) {
      result[key] = settings[key];
    }
  }
  return result;
}

function buildExtraSourceSettings(index, streamId, password, roomId) {
  return {
    stream_id: streamId,
    password,
    room_id: roomId,
    use_native_receiver: index % 2 === 0,
    enable_data_channel: index % 3 !== 0,
    auto_reconnect: true,
    width: 640 + index * 160,
    height: 360 + index * 90,
  };
}

async function createVdoInput(client, sceneName, inputName, settings) {
  const createResponse = await client.request("CreateInput", {
    sceneName,
    inputName,
    inputKind: "vdoninja_source",
    inputSettings: settings,
    sceneItemEnabled: true,
  });
  return {
    inputName,
    inputUuid: createResponse.inputUuid || createResponse.inputUUID || null,
    sceneItemId: createResponse.sceneItemId,
  };
}

async function createExtraSources(client, sceneName, baseInputName, count, streamId, password, roomId) {
  const extras = [];
  for (let index = 0; index < count; index += 1) {
    const extraInputName = `${baseInputName} extra ${index + 1}`;
    logStep(`creating extra input ${extraInputName}`);
    const extra = await createVdoInput(
      client,
      sceneName,
      extraInputName,
      buildExtraSourceSettings(index, streamId, password, roomId)
    );
    extras.push(extra);
    if (extra.sceneItemId !== undefined && extra.sceneItemId !== null) {
      await client.request("SetSceneItemTransform", {
        sceneName,
        sceneItemId: extra.sceneItemId,
        sceneItemTransform: {
          positionX: 40 + (index % 3) * 220,
          positionY: 420 + Math.floor(index / 3) * 160,
          scaleX: 0.35,
          scaleY: 0.35,
        },
      });
    }
    await sleep(250);
  }
  return extras;
}

async function runVideoSettingsChurn(client, originalVideoSettings) {
  const profile = String(process.env.VDONINJA_EDGE_VIDEO_CHURN_PROFILE || "standard").trim().toLowerCase();
  const cases = [
    { name: "qhd-60", settings: { baseWidth: 2560, baseHeight: 1440, outputWidth: 2560, outputHeight: 1440, fpsNumerator: 60, fpsDenominator: 1 } },
    { name: "ntsc-720p", settings: { baseWidth: 1280, baseHeight: 720, outputWidth: 1280, outputHeight: 720, fpsNumerator: 30000, fpsDenominator: 1001 } },
    { name: "uhd-30", settings: { baseWidth: 3840, baseHeight: 2160, outputWidth: 3840, outputHeight: 2160, fpsNumerator: 30, fpsDenominator: 1 } },
    { name: "small-30", settings: { baseWidth: 640, baseHeight: 360, outputWidth: 640, outputHeight: 360, fpsNumerator: 30, fpsDenominator: 1 } },
  ];
  if (profile === "aggressive") {
    cases.push(
      { name: "uhd-60", settings: { baseWidth: 3840, baseHeight: 2160, outputWidth: 3840, outputHeight: 2160, fpsNumerator: 60, fpsDenominator: 1 } },
      { name: "portrait-1080p-60", settings: { baseWidth: 1080, baseHeight: 1920, outputWidth: 1080, outputHeight: 1920, fpsNumerator: 60, fpsDenominator: 1 } },
      { name: "ultrawide-120", settings: { baseWidth: 3440, baseHeight: 1440, outputWidth: 3440, outputHeight: 1440, fpsNumerator: 120, fpsDenominator: 1 } },
      { name: "odd-59-94", settings: { baseWidth: 853, baseHeight: 481, outputWidth: 853, outputHeight: 481, fpsNumerator: 60000, fpsDenominator: 1001 } },
      { name: "tiny-15", settings: { baseWidth: 160, baseHeight: 90, outputWidth: 160, outputHeight: 90, fpsNumerator: 15, fpsDenominator: 1 } }
    );
  }
  const results = [];

  for (const edgeCase of cases) {
    logStep(`video settings ${edgeCase.name}`);
    const beforeStats = copyStats(await client.request("GetStats").catch(() => ({})));
    try {
      await client.request("SetVideoSettings", edgeCase.settings);
      await sleep(1500);
      const observedSettings = pickVideoSettings(await client.request("GetVideoSettings").catch(() => ({})));
      const afterStats = copyStats(await client.request("GetStats").catch(() => ({})));
      results.push({
        name: edgeCase.name,
        requestedSettings: edgeCase.settings,
        observedSettings,
        statsBefore: beforeStats,
        statsAfter: afterStats,
      });
    } catch (error) {
      results.push({
        name: edgeCase.name,
        requestedSettings: edgeCase.settings,
        statsBefore: beforeStats,
        error: String(error && error.message ? error.message : error),
      });
    }
  }

  logStep("restoring video settings");
  await client.request("SetVideoSettings", originalVideoSettings);
  await sleep(1500);

  return {
    profile,
    originalSettings: originalVideoSettings,
    results,
    restoredSettings: pickVideoSettings(await client.request("GetVideoSettings").catch(() => ({}))),
  };
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

async function runRapidMutationLoop(
  client,
  inputName,
  inputUuid,
  sceneName,
  sceneItemId,
  streamId,
  password,
  roomId,
  extraInputs = [],
  stableSettings = false
) {
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
      inputSettings: stableSettings
        ? {
            stream_id: streamId,
            password,
            room_id: roomId,
            use_native_receiver: true,
            enable_data_channel: true,
            auto_reconnect: true,
            width,
            height,
          }
        : {
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

    for (let extraIndex = 0; extraIndex < extraInputs.length; extraIndex += 1) {
      const extra = extraInputs[extraIndex];
      if (extra.sceneItemId === undefined || extra.sceneItemId === null) {
        continue;
      }
      await client.request("SetSceneItemEnabled", {
        sceneName,
        sceneItemId: extra.sceneItemId,
        sceneItemEnabled: (index + extraIndex) % 5 !== 0,
      });
      await client.request("SetSceneItemTransform", {
        sceneName,
        sceneItemId: extra.sceneItemId,
        sceneItemTransform: {
          positionX: 40 + ((index * 23 + extraIndex * 170) % 720),
          positionY: 400 + ((index * 17 + extraIndex * 70) % 260),
          scaleX: index % 3 === 0 ? 0.25 : 0.45,
          scaleY: index % 4 === 0 ? 0.25 : 0.45,
          rotation: (index * 11 + extraIndex * 19) % 360,
        },
      });
      if (index % 8 === 0) {
        await client.request("SetInputSettings", {
          ...buildInputReference(extra.inputName, extra.inputUuid),
          inputSettings: stableSettings
            ? {
                stream_id: streamId,
                password,
                room_id: roomId,
                use_native_receiver: extraIndex % 2 === 0,
                enable_data_channel: extraIndex % 3 !== 0,
                auto_reconnect: true,
                width: dimensions[(index + extraIndex) % dimensions.length][0],
                height: dimensions[(index + extraIndex) % dimensions.length][1],
              }
            : {
                use_native_receiver: (index + extraIndex) % 2 === 0,
                enable_data_channel: (index + extraIndex) % 3 !== 0,
                width: dimensions[(index + extraIndex) % dimensions.length][0],
                height: dimensions[(index + extraIndex) % dimensions.length][1],
              },
          overlay: true,
        });
      }
    }

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
    stableSettings,
    durationMs: Date.now() - startedAt,
  };
}

async function main() {
  const websocketUrl = process.env.OBS_WEBSOCKET_URL || "ws://127.0.0.1:4455";
  const streamId = process.env.VDONINJA_STREAM_ID || process.argv[2] || "codexEdgeStress";
  const password = process.env.VDONINJA_PASSWORD || process.argv[3] || "false";
  const roomId = process.env.VDONINJA_ROOM_ID || process.argv[4] || "";
  const keepScene = parseBoolean(process.env.VDONINJA_KEEP_SCENE, false);
  const extraSourceCount = parseNonNegativeInteger(process.env.VDONINJA_EDGE_EXTRA_SOURCES, 0);
  const videoChurn = parseBoolean(process.env.VDONINJA_EDGE_VIDEO_CHURN, false);
  const skipEdgeCases = parseBoolean(process.env.VDONINJA_EDGE_SKIP_CASES, false);
  const stableRapidSettings = parseBoolean(process.env.VDONINJA_EDGE_STABLE_RAPID_SETTINGS, false);
  const stamp = Date.now();
  const sceneName = `Codex Edge Stress ${stamp}`;
  const inputName = `Codex Edge VDO Source ${stamp}`;
  const reportPath = path.resolve(process.cwd(), "artifacts", `obs-edge-stress-${stamp}.json`);
  const client = new ObsWebSocketClient(websocketUrl);

  let previousSceneName = null;
  let createdScene = false;
  let sceneItemId = null;
  let inputUuid = null;
  const createdInputs = [];
  let extraInputs = [];
  let originalVideoSettings = null;

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
    const primaryInput = await createVdoInput(
      client,
      sceneName,
      inputName,
      {
        stream_id: streamId,
        password,
        room_id: roomId,
        use_native_receiver: true,
        enable_data_channel: true,
        auto_reconnect: true,
        width: 1280,
        height: 720,
      }
    );
    sceneItemId = primaryInput.sceneItemId;
    inputUuid = primaryInput.inputUuid;
    createdInputs.push(primaryInput);
    logStep(`created sceneItemId=${sceneItemId} inputUuid=${inputUuid || "(none)"}`);
    await sleep(500);

    if (sceneItemId === undefined || sceneItemId === null) {
      throw new Error("OBS did not return sceneItemId for edge stress input");
    }

    if (extraSourceCount > 0) {
      extraInputs = await createExtraSources(client, sceneName, inputName, extraSourceCount, streamId, password, roomId);
      createdInputs.push(...extraInputs);
    }

    const caseResults = [];
    if (!skipEdgeCases) {
      for (const edgeCase of buildCases(streamId, password, roomId)) {
        caseResults.push(await applyCase(client, inputName, inputUuid, sceneName, sceneItemId, edgeCase));
      }
    } else {
      logStep("skipping edge-case source settings");
    }

    let videoSettingsChurn = null;
    if (videoChurn) {
      originalVideoSettings = pickVideoSettings(await client.request("GetVideoSettings"));
      videoSettingsChurn = await runVideoSettingsChurn(client, originalVideoSettings);
      originalVideoSettings = null;
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
      roomId,
      extraInputs,
      stableRapidSettings
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
      skipEdgeCases,
      stableRapidSettings,
      extraSources: extraInputs.map((extra) => ({
        inputName: extra.inputName,
        inputUuid: extra.inputUuid,
        sceneItemId: extra.sceneItemId,
      })),
      caseResults,
      videoSettingsChurn,
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
        if (originalVideoSettings) {
          await client.request("SetVideoSettings", originalVideoSettings).catch(() => {});
        }
        if (!keepScene) {
          for (const input of [...createdInputs].reverse()) {
            await client.request("RemoveInput", buildInputReference(input.inputName, input.inputUuid)).catch(() => {});
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
