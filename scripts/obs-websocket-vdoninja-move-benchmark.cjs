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
  console.error(`[obs-move-benchmark] ${message}`);
}

function percentile(values, fraction) {
  if (!values.length) {
    return 0;
  }

  const sorted = [...values].sort((a, b) => a - b);
  const index = Math.min(sorted.length - 1, Math.max(0, Math.ceil(sorted.length * fraction) - 1));
  return sorted[index];
}

function summarizeLatencies(latencies) {
  if (!latencies.length) {
    return {
      count: 0,
      avgMs: 0,
      minMs: 0,
      maxMs: 0,
      p50Ms: 0,
      p95Ms: 0,
      over16ms: 0,
      over33ms: 0,
      over50ms: 0,
      over100ms: 0,
    };
  }

  const sum = latencies.reduce((acc, value) => acc + value, 0);
  return {
    count: latencies.length,
    avgMs: Number((sum / latencies.length).toFixed(3)),
    minMs: Number(Math.min(...latencies).toFixed(3)),
    maxMs: Number(Math.max(...latencies).toFixed(3)),
    p50Ms: Number(percentile(latencies, 0.5).toFixed(3)),
    p95Ms: Number(percentile(latencies, 0.95).toFixed(3)),
    over16ms: latencies.filter((value) => value > 16.0).length,
    over33ms: latencies.filter((value) => value > 33.0).length,
    over50ms: latencies.filter((value) => value > 50.0).length,
    over100ms: latencies.filter((value) => value > 100.0).length,
  };
}

function copyNumericStats(stats) {
  if (!stats || typeof stats !== "object") {
    return {};
  }

  const keys = [
    "cpuUsage",
    "memoryUsage",
    "availableDiskSpace",
    "activeFps",
    "averageFrameRenderTime",
    "renderSkippedFrames",
    "renderTotalFrames",
    "outputSkippedFrames",
    "outputTotalFrames",
    "webSocketSessionIncomingMessages",
    "webSocketSessionOutgoingMessages",
  ];
  const result = {};
  for (const key of keys) {
    const value = stats[key];
    if (typeof value === "number" && Number.isFinite(value)) {
      result[key] = value;
    }
  }
  return result;
}

function diffNumericStats(beforeStats, afterStats) {
  const result = {};
  const keys = new Set([...Object.keys(beforeStats || {}), ...Object.keys(afterStats || {})]);
  for (const key of keys) {
    if (typeof beforeStats[key] === "number" && typeof afterStats[key] === "number") {
      result[key] = Number((afterStats[key] - beforeStats[key]).toFixed(3));
    }
  }
  return result;
}

function parsePhases() {
  const raw = process.env.VDONINJA_MOVE_PHASES_JSON;
  if (!raw) {
    return [
      { name: "immediate", waitMs: 0 },
      { name: "loading", waitMs: 2500 },
      { name: "steady", waitMs: 12000 },
    ];
  }

  const parsed = JSON.parse(raw);
  if (!Array.isArray(parsed) || parsed.length === 0) {
    throw new Error("VDONINJA_MOVE_PHASES_JSON must be a non-empty JSON array");
  }

  return parsed.map((entry, index) => {
    const name =
      entry && typeof entry.name === "string" && entry.name.trim() ? entry.name.trim() : `phase-${index + 1}`;
    const waitMs = Number(entry && entry.waitMs !== undefined ? entry.waitMs : 0);
    if (!Number.isFinite(waitMs) || waitMs < 0) {
      throw new Error(`Phase ${name} has invalid waitMs`);
    }
    return { name, waitMs };
  });
}

function selectBaselineInputKind(kinds) {
  const preferredKinds = ["color_source_v3", "color_source", "image_source"];
  return preferredKinds.find((kind) => kinds.includes(kind)) || null;
}

function buildInputDefinition(mode, streamId, password, roomId, kinds) {
  if (mode === "baseline") {
    const inputKind = selectBaselineInputKind(kinds);
    if (!inputKind) {
      throw new Error("Could not find a lightweight baseline input kind");
    }

    const inputSettings =
      inputKind === "image_source"
        ? {}
        : {
            color: 4278255615,
            width: 1280,
            height: 720,
          };
    return {
      inputKind,
      inputSettings,
    };
  }

  if (!streamId) {
    throw new Error("streamId is required for browser/native movement benchmarks");
  }

  return {
    inputKind: "vdoninja_source",
    inputSettings: {
      stream_id: streamId,
      password,
      room_id: roomId,
      use_native_receiver: mode === "native",
      enable_data_channel: true,
      auto_reconnect: true,
      width: 1280,
      height: 720,
    },
  };
}

async function captureSceneScreenshot(client, sourceName, outputPath) {
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

async function runMovePhase(client, sceneName, sceneItemId, phaseName, iterations, intervalMs) {
  const beforeStats = copyNumericStats(await client.request("GetStats").catch(() => ({})));
  const latencies = [];
  const canvasWidth = 1280;
  const canvasHeight = 720;
  const itemWidth = 640;
  const itemHeight = 360;
  const positions = [
    [40, 40],
    [canvasWidth - itemWidth - 40, 40],
    [canvasWidth - itemWidth - 40, canvasHeight - itemHeight - 40],
    [40, canvasHeight - itemHeight - 40],
    [320, 180],
  ];

  for (let index = 0; index < iterations; index += 1) {
    const [positionX, positionY] = positions[index % positions.length];
    const startedAt = process.hrtime.bigint();
    await client.request("SetSceneItemTransform", {
      sceneName,
      sceneItemId,
      sceneItemTransform: {
        positionX,
        positionY,
        scaleX: itemWidth / canvasWidth,
        scaleY: itemHeight / canvasHeight,
        rotation: 0,
      },
    });
    const elapsedMs = Number(process.hrtime.bigint() - startedAt) / 1_000_000;
    latencies.push(elapsedMs);

    if (intervalMs > 0 && index + 1 < iterations) {
      await sleep(intervalMs);
    }
  }

  const afterStats = copyNumericStats(await client.request("GetStats").catch(() => ({})));

  return {
    phase: phaseName,
    ...summarizeLatencies(latencies),
    statsBefore: beforeStats,
    statsAfter: afterStats,
    statsDelta: diffNumericStats(beforeStats, afterStats),
  };
}

async function main() {
  const mode = (process.env.VDONINJA_SOURCE_MODE || process.argv[2] || "browser").trim().toLowerCase();
  const streamId = process.env.VDONINJA_STREAM_ID || process.argv[3] || "";
  const password = process.env.VDONINJA_PASSWORD || process.argv[4] || "";
  const roomId = process.env.VDONINJA_ROOM_ID || process.argv[5] || "";
  const websocketUrl = process.env.OBS_WEBSOCKET_URL || "ws://127.0.0.1:4455";
  const iterations = Number(process.env.VDONINJA_MOVE_ITERATIONS || 60);
  const intervalMs = Number(process.env.VDONINJA_MOVE_INTERVAL_MS || 15);
  const keepScene = parseBoolean(process.env.VDONINJA_KEEP_SCENE, false);
  const skipCapture = parseBoolean(process.env.VDONINJA_SKIP_CAPTURE, false);
  const phases = parsePhases();

  if (!["baseline", "browser", "native"].includes(mode)) {
    throw new Error("Mode must be baseline, browser, or native");
  }
  if (!Number.isFinite(iterations) || iterations < 1) {
    throw new Error("VDONINJA_MOVE_ITERATIONS must be at least 1");
  }
  if (!Number.isFinite(intervalMs) || intervalMs < 0) {
    throw new Error("VDONINJA_MOVE_INTERVAL_MS must be >= 0");
  }

  const client = new ObsWebSocketClient(websocketUrl);
  const stamp = Date.now();
  const sceneName = `Codex Move Benchmark ${stamp}`;
  const inputName = `Codex Move Source ${mode} ${stamp}`;
  const screenshotPath = path.resolve(process.cwd(), "artifacts", `obs-move-${mode}-${stamp}.png`);

  let previousSceneName = null;
  let createdScene = false;
  let createdInput = false;
  try {
    logStep(`connecting to ${websocketUrl}`);
    await client.connect();

    logStep("requesting OBS version");
    const version = await client.request("GetVersion");

    logStep("requesting input kinds");
    const kindsResponse = await client.request("GetInputKindList", { unversioned: false });
    const inputKinds = Array.isArray(kindsResponse.inputKinds) ? kindsResponse.inputKinds : [];
    if (mode !== "baseline" && !inputKinds.includes("vdoninja_source")) {
      throw new Error("OBS does not have the vdoninja_source input kind registered");
    }

    const { inputKind, inputSettings } = buildInputDefinition(mode, streamId, password, roomId, inputKinds);

    logStep("reading current program scene");
    const currentProgram = await client.request("GetCurrentProgramScene").catch(() => ({}));
    previousSceneName = currentProgram.currentProgramSceneName || null;

    createdScene = true;
    logStep(`creating scene ${sceneName}`);
    await client.request("CreateScene", { sceneName });
    logStep(`switching to scene ${sceneName}`);
    await client.request("SetCurrentProgramScene", { sceneName });

    logStep(`creating ${inputKind} input ${inputName}`);
    const createInputResponse = await client.request("CreateInput", {
      sceneName,
      inputName,
      inputKind,
      inputSettings,
      sceneItemEnabled: true,
    });
    createdInput = true;
    const sceneItemId = createInputResponse.sceneItemId;
    if (sceneItemId === undefined || sceneItemId === null) {
      throw new Error("OBS did not return a sceneItemId for the created input");
    }

    await client.request("SetSceneItemTransform", {
      sceneName,
      sceneItemId,
      sceneItemTransform: {
        positionX: 40,
        positionY: 40,
        scaleX: 0.5,
        scaleY: 0.5,
        rotation: 0,
      },
    });

    const phaseResults = [];
    let lastWaitMs = 0;
    for (const phase of phases) {
      const additionalWaitMs = Math.max(0, phase.waitMs - lastWaitMs);
      if (additionalWaitMs > 0) {
        logStep(`waiting ${additionalWaitMs}ms before phase ${phase.name}`);
        await sleep(additionalWaitMs);
      }
      lastWaitMs = phase.waitMs;

      logStep(`running move phase ${phase.name}`);
      phaseResults.push(await runMovePhase(client, sceneName, sceneItemId, phase.name, iterations, intervalMs));
    }

    let screenshot = null;
    if (!skipCapture) {
      logStep(`capturing screenshot for ${sceneName}`);
      screenshot = await captureSceneScreenshot(client, sceneName, screenshotPath);
    }

    const result = {
      ok: true,
      mode,
      streamId,
      websocketUrl,
      iterations,
      intervalMs,
      phases: phaseResults,
      screenshot,
      obsVersion: version.obsVersion,
      obsWebSocketVersion: version.obsWebSocketVersion,
    };

    process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  } finally {
    try {
      if (!keepScene && previousSceneName) {
        await client.request("SetCurrentProgramScene", { sceneName: previousSceneName }).catch(() => undefined);
      }
      if (!keepScene && createdInput) {
        await client.request("RemoveInput", { inputName }).catch(() => undefined);
      }
      if (!keepScene && createdScene) {
        await client.request("RemoveScene", { sceneName }).catch(() => undefined);
      }
    } finally {
      await client.close();
    }
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exitCode = 1;
});
