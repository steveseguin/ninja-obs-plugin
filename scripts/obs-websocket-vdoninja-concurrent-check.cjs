const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

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
    this.requestTimeoutMs = Number(process.env.OBS_WEBSOCKET_REQUEST_TIMEOUT_MS || 15000);
  }

  async connect() {
    await new Promise((resolve, reject) => {
      const socket = new WebSocket(this.url);
      this.socket = socket;

      socket.addEventListener("open", () => resolve());
      socket.addEventListener("error", (error) => reject(error));
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
    const payload = {
      op: 6,
      d: {
        requestType,
        requestId,
        requestData,
      },
    };

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

    this.socket.send(JSON.stringify(payload));
    return response;
  }

  async close() {
    if (!this.socket) {
      return;
    }

    for (const pending of this.pending.values()) {
      pending.reject(new Error("obs-websocket connection closed"));
    }
    this.pending.clear();

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
  console.error(`[obs-concurrent-check] ${message}`);
}

function parseSources() {
  const raw = process.env.VDONINJA_CONCURRENT_SOURCES_JSON || "";
  if (!raw) {
    throw new Error("VDONINJA_CONCURRENT_SOURCES_JSON is required");
  }

  const parsed = JSON.parse(raw);
  if (!Array.isArray(parsed) || parsed.length < 2) {
    throw new Error("VDONINJA_CONCURRENT_SOURCES_JSON must be a JSON array with at least two sources");
  }

  return parsed.map((entry, index) => {
    const streamId = entry && typeof entry.streamId === "string" ? entry.streamId.trim() : "";
    if (!streamId) {
      throw new Error(`Source ${index + 1} is missing streamId`);
    }

    return {
      label:
        entry && typeof entry.label === "string" && entry.label.trim()
          ? entry.label.trim()
          : `Source ${index + 1}`,
      streamId,
      password: entry && typeof entry.password === "string" ? entry.password : "",
      roomId: entry && typeof entry.roomId === "string" ? entry.roomId : "",
    };
  });
}

async function captureScreenshot(client, sourceName, outputPath) {
  const minScreenshotBytes = Number(process.env.VDONINJA_MIN_SCREENSHOT_BYTES || 10000);
  const response = await client.request("GetSourceScreenshot", {
    sourceName,
    imageFormat: "png",
    imageWidth: 1280,
    imageHeight: 720,
    imageCompressionQuality: 0,
  });

  const imageData = response.imageData || "";
  const prefix = "data:image/png;base64,";
  if (!imageData.startsWith(prefix)) {
    throw new Error(`OBS did not return a PNG screenshot for ${sourceName}`);
  }

  const buffer = Buffer.from(imageData.slice(prefix.length), "base64");
  if (buffer.length < minScreenshotBytes) {
    throw new Error(`Screenshot for ${sourceName} was unexpectedly small (${buffer.length} bytes)`);
  }

  fs.mkdirSync(path.dirname(outputPath), { recursive: true });
  fs.writeFileSync(outputPath, buffer);

  return {
    outputPath,
    byteLength: buffer.length,
    sha256: crypto.createHash("sha256").update(buffer).digest("hex"),
  };
}

async function setTileTransform(client, sceneName, sceneItemId, index, count) {
  const width = 1280;
  const height = 720;
  const columns = count >= 2 ? 2 : 1;
  const tileWidth = width / columns;
  const tileHeight = height / columns;
  const row = Math.floor(index / columns);
  const column = index % columns;

  await client.request("SetSceneItemTransform", {
    sceneName,
    sceneItemId,
    sceneItemTransform: {
      positionX: column * tileWidth,
      positionY: row * tileHeight,
      scaleX: tileWidth / width,
      scaleY: tileHeight / height,
    },
  });
}

async function main() {
  const mode = (process.env.VDONINJA_SOURCE_MODE || process.argv[2] || "native").trim().toLowerCase();
  const websocketUrl = process.env.OBS_WEBSOCKET_URL || "ws://127.0.0.1:4455";
  const waitMs = Number(process.env.VDONINJA_WAIT_MS || 30000);
  const keepScene = parseBoolean(process.env.VDONINJA_KEEP_SCENE, false);
  const skipSceneCapture = parseBoolean(process.env.VDONINJA_SKIP_CAPTURE, false);
  const useNativeReceiver = mode === "native";
  const sources = parseSources();

  const client = new ObsWebSocketClient(websocketUrl);
  const stamp = Date.now();
  const sceneName = `Codex Concurrent Smoke ${stamp}`;
  const sceneScreenshotPath = path.resolve(process.cwd(), "artifacts", `obs-concurrent-${mode}-${stamp}.png`);

  let previousSceneName = null;
  let createdScene = false;
  const createdInputs = [];

  try {
    logStep(`connecting to ${websocketUrl}`);
    await client.connect();

    logStep("requesting OBS version");
    const version = await client.request("GetVersion");

    logStep("requesting input kinds");
    const kinds = await client.request("GetInputKindList", { unversioned: false });
    if (!Array.isArray(kinds.inputKinds) || !kinds.inputKinds.includes("vdoninja_source")) {
      throw new Error("OBS does not have the vdoninja_source input kind registered");
    }

    logStep("reading current program scene");
    const currentProgram = await client.request("GetCurrentProgramScene").catch(() => ({}));
    previousSceneName = currentProgram.currentProgramSceneName || null;

    createdScene = true;
    logStep(`creating scene ${sceneName}`);
    await client.request("CreateScene", { sceneName });
    logStep(`switching to scene ${sceneName}`);
    await client.request("SetCurrentProgramScene", { sceneName });

    for (const [index, source] of sources.entries()) {
      const inputName = `Codex ${source.label} ${mode} ${stamp}`;
      logStep(`creating input ${inputName}`);
      const createResponse = await client.request("CreateInput", {
        sceneName,
        inputName,
        inputKind: "vdoninja_source",
        inputSettings: {
          stream_id: source.streamId,
          password: source.password,
          room_id: source.roomId,
          use_native_receiver: useNativeReceiver,
          enable_data_channel: true,
          auto_reconnect: true,
          width: 1280,
          height: 720,
        },
        sceneItemEnabled: true,
      });

      const created = {
        label: source.label,
        inputName,
        sceneItemId: createResponse && Number.isFinite(createResponse.sceneItemId) ? createResponse.sceneItemId : null,
      };
      createdInputs.push(created);

      if (created.sceneItemId !== null) {
        try {
          await setTileTransform(client, sceneName, created.sceneItemId, index, sources.length);
        } catch (error) {
          logStep(`scene item transform skipped for ${inputName}: ${error.message}`);
        }
      }
    }

    logStep(`waiting ${waitMs}ms for concurrent sources to render`);
    await sleep(waitMs);

    const sourceResults = [];
    for (const created of createdInputs) {
      logStep(`reading input settings for ${created.inputName}`);
      const inputSettings = await client.request("GetInputSettings", { inputName: created.inputName }).catch(() => null);
      const sceneItemTransform =
        created.sceneItemId !== null
          ? await client
              .request("GetSceneItemTransform", {
                sceneName,
                sceneItemId: created.sceneItemId,
              })
              .catch(() => null)
          : null;

      sourceResults.push({
        label: created.label,
        inputName: created.inputName,
        sceneItemId: created.sceneItemId,
        sourceSettings: inputSettings && inputSettings.inputSettings ? inputSettings.inputSettings : null,
        sceneItemTransform:
          sceneItemTransform && sceneItemTransform.sceneItemTransform ? sceneItemTransform.sceneItemTransform : null,
      });
    }

    let sceneScreenshot = null;
    if (!skipSceneCapture) {
      logStep(`capturing scene screenshot for ${sceneName}`);
      sceneScreenshot = await captureScreenshot(client, sceneName, sceneScreenshotPath);
    }

    console.log(
      JSON.stringify(
        {
          ok: true,
          mode,
          waitMs,
          websocketUrl,
          obsVersion: version.obsVersion,
          obsWebSocketVersion: version.obsWebSocketVersion,
          inputKindRegistered: true,
          sceneName,
          sceneScreenshot,
          sources: sourceResults,
        },
        null,
        2
      )
    );
  } finally {
    if (client.socket && client.socket.readyState === WebSocket.OPEN) {
      try {
        if (!keepScene) {
          for (const created of createdInputs.slice().reverse()) {
            logStep(`removing input ${created.inputName}`);
            await client.request("RemoveInput", { inputName: created.inputName }).catch(() => {});
          }
          if (createdScene && previousSceneName) {
            logStep(`restoring scene ${previousSceneName}`);
            await client.request("SetCurrentProgramScene", { sceneName: previousSceneName }).catch(() => {});
          }
          if (createdScene) {
            logStep(`removing scene ${sceneName}`);
            await client.request("RemoveScene", { sceneName }).catch(() => {});
          }
        }
      } finally {
        logStep("closing websocket");
        await client.close();
      }
    }
  }
}

main().catch((error) => {
  console.error(error.stack || String(error));
  process.exit(1);
});
