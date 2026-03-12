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
      socket.addEventListener("message", async (event) => {
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
  console.error(`[obs-source-check] ${message}`);
}

async function captureSourceScreenshot(client, sourceName, outputPath) {
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
    throw new Error("OBS did not return a PNG screenshot");
  }

  const buffer = Buffer.from(imageData.slice(prefix.length), "base64");
  if (buffer.length < minScreenshotBytes) {
    throw new Error(`Screenshot was unexpectedly small (${buffer.length} bytes)`);
  }

  fs.mkdirSync(path.dirname(outputPath), { recursive: true });
  fs.writeFileSync(outputPath, buffer);

  return {
    outputPath,
    byteLength: buffer.length,
    sha256: crypto.createHash("sha256").update(buffer).digest("hex"),
  };
}

async function main() {
  const mode = (process.env.VDONINJA_SOURCE_MODE || process.argv[2] || "browser").trim().toLowerCase();
  const streamId = process.env.VDONINJA_STREAM_ID || process.argv[3];
  const password = process.env.VDONINJA_PASSWORD || process.argv[4] || "";
  const roomId = process.env.VDONINJA_ROOM_ID || process.argv[5] || "";
  const waitMs = Number(process.env.VDONINJA_WAIT_MS || 30000);
  const websocketUrl = process.env.OBS_WEBSOCKET_URL || "ws://127.0.0.1:4455";
  const useNativeReceiver = mode === "native";
  const keepScene = parseBoolean(process.env.VDONINJA_KEEP_SCENE, false);
  const skipCapture = parseBoolean(process.env.VDONINJA_SKIP_CAPTURE, false);

  if (!streamId) {
    throw new Error("Usage: node scripts/obs-websocket-vdoninja-source-check.cjs <browser|native> <streamId> [password] [roomId]");
  }

  const client = new ObsWebSocketClient(websocketUrl);
  const stamp = Date.now();
  const sceneName = `Codex Source Smoke ${stamp}`;
  const inputName = `Codex VDO Source ${mode} ${stamp}`;
  const screenshotPath = path.resolve(
    process.cwd(),
    "artifacts",
    `obs-source-${mode}-${stamp}.png`
  );

  let previousSceneName = null;
  let targetSceneName = null;
  let createdScene = false;

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

    targetSceneName = sceneName;
    createdScene = true;
    logStep(`creating scene ${sceneName}`);
    await client.request("CreateScene", { sceneName });
    logStep(`switching to scene ${sceneName}`);
    await client.request("SetCurrentProgramScene", { sceneName });

    logStep(`creating input ${inputName}`);
    await client.request("CreateInput", {
      sceneName: targetSceneName,
      inputName,
      inputKind: "vdoninja_source",
      inputSettings: {
        stream_id: streamId,
        password,
        room_id: roomId,
        use_native_receiver: useNativeReceiver,
        enable_data_channel: true,
        auto_reconnect: true,
        width: 1280,
        height: 720,
      },
      sceneItemEnabled: true,
    });

    logStep(`waiting ${waitMs}ms for source to render`);
    await sleep(waitMs);

    logStep(`reading input settings for ${inputName}`);
    const inputSettings = await client.request("GetInputSettings", { inputName }).catch(() => null);
    let screenshot = null;
    if (!skipCapture) {
      logStep(`capturing screenshot for ${targetSceneName}`);
      screenshot = await captureSourceScreenshot(client, targetSceneName, screenshotPath);
    }

    const result = {
      ok: true,
      mode,
      waitMs,
      websocketUrl,
      obsVersion: version.obsVersion,
      obsWebSocketVersion: version.obsWebSocketVersion,
      inputKindRegistered: true,
      screenshot,
      inputName,
      sceneName: targetSceneName,
      sourceSettings: inputSettings && inputSettings.inputSettings ? inputSettings.inputSettings : null,
      useNativeReceiver:
        inputSettings && inputSettings.inputSettings
          ? parseBoolean(inputSettings.inputSettings.use_native_receiver, useNativeReceiver)
          : useNativeReceiver,
    };

    console.log(JSON.stringify(result, null, 2));
  } finally {
    if (client.socket && client.socket.readyState === WebSocket.OPEN) {
      try {
        if (!keepScene) {
          logStep(`removing input ${inputName}`);
          await client.request("RemoveInput", { inputName }).catch(() => {});
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
