const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, Math.max(0, ms)));
}

class ObsWebSocketClient {
  constructor(url) {
    this.url = url;
    this.socket = null;
    this.requestId = 0;
    this.pending = new Map();
    this.identified = false;
    this.requestTimeoutMs = Number(process.env.OBS_WEBSOCKET_REQUEST_TIMEOUT_MS || 30000);
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
  if (value === undefined || value === null || value === "") {
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
  console.error(`[vdoninja-chaos] ${message}`);
}

function inputRef(inputName, inputUuid) {
  return inputUuid ? { inputUuid } : { inputName };
}

function inputUuidFromRecord(input) {
  return input ? input.inputUuid || input.inputUUID || null : null;
}

async function getInputRecord(client, inputName) {
  const inputs = await client.request("GetInputList").catch(() => ({ inputs: [] }));
  if (!Array.isArray(inputs.inputs)) {
    return null;
  }
  return inputs.inputs.find((input) => input && input.inputName === inputName) || null;
}

async function setInputSettings(client, inputName, inputUuid, inputSettings) {
  const request = {
    ...inputRef(inputName, inputUuid),
    inputSettings,
    overlay: true,
  };
  try {
    await client.request("SetInputSettings", request);
  } catch (error) {
    if (!inputUuid) {
      throw error;
    }
    await client.request("SetInputSettings", {
      inputName,
      inputSettings,
      overlay: true,
    });
  }
}

async function getInputSettings(client, inputName, inputUuid) {
  try {
    return await client.request("GetInputSettings", inputRef(inputName, inputUuid));
  } catch (error) {
    if (!inputUuid) {
      throw error;
    }
    return client.request("GetInputSettings", { inputName });
  }
}

async function removeInput(client, inputName, inputUuid = null) {
  let resolvedUuid = inputUuid;
  if (!resolvedUuid) {
    resolvedUuid = inputUuidFromRecord(await getInputRecord(client, inputName));
  }
  try {
    await client.request("RemoveInput", inputRef(inputName, resolvedUuid));
  } catch (error) {
    if (!resolvedUuid) {
      throw error;
    }
    await client.request("RemoveInput", { inputName });
  }
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
    "webSocketSessionIncomingMessages",
    "webSocketSessionOutgoingMessages",
  ];
  const result = {};
  for (const key of keys) {
    if (typeof stats[key] === "number" && Number.isFinite(stats[key])) {
      result[key] = stats[key];
    }
  }
  return result;
}

function pngFromDataUrl(imageData) {
  const prefix = "data:image/png;base64,";
  if (!String(imageData || "").startsWith(prefix)) {
    throw new Error("OBS did not return a PNG screenshot");
  }
  return Buffer.from(imageData.slice(prefix.length), "base64");
}

async function screenshot(client, sourceName, outputPath, minBytes) {
  const response = await client.request("GetSourceScreenshot", {
    sourceName,
    imageFormat: "png",
    imageWidth: 1280,
    imageHeight: 720,
    imageCompressionQuality: 0,
  });
  const buffer = pngFromDataUrl(response.imageData || "");
  if (buffer.length < minBytes) {
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

async function motionPair(client, sceneName, reportDir, label, options) {
  const waitMs = Number(options.waitMs || 2500);
  const minBytes = Number(options.minBytes || 8000);
  const first = await screenshot(client, sceneName, path.join(reportDir, `${label}-a.png`), minBytes);
  await sleep(waitMs);
  const second = await screenshot(client, sceneName, path.join(reportDir, `${label}-b.png`), minBytes);
  const changed = first.sha256 !== second.sha256;
  if (options.requireMotion && !changed) {
    throw new Error(`Scene ${sceneName} did not visibly change between screenshots for ${label}`);
  }
  return {
    first,
    second,
    waitMs,
    changed,
  };
}

async function waitForMotionPair(client, sceneName, reportDir, label, options) {
  const timeoutMs = Number(options.motionTimeoutMs || 0);
  const retryDelayMs = Number(options.retryDelayMs || 1000);
  const startedAt = Date.now();
  let attempt = 0;
  let lastError = null;

  while (true) {
    const attemptLabel = attempt === 0 ? label : `${label}-retry-${attempt}`;
    try {
      const result = await motionPair(client, sceneName, reportDir, attemptLabel, options);
      return {
        ...result,
        attempts: attempt + 1,
      };
    } catch (error) {
      lastError = error;
      if (!options.requireMotion || timeoutMs <= 0 || Date.now() - startedAt >= timeoutMs) {
        throw error;
      }
      logStep(`motion check ${label} not ready yet: ${error.message}`);
      attempt += 1;
      await sleep(retryDelayMs);
    }
  }
}

async function getInputKinds(client) {
  const kinds = await client.request("GetInputKindList", { unversioned: false });
  return Array.isArray(kinds.inputKinds) ? kinds.inputKinds : [];
}

async function sceneExists(client, sceneName) {
  const scenes = await client.request("GetSceneList");
  return Array.isArray(scenes.scenes) && scenes.scenes.some((scene) => scene.sceneName === sceneName);
}

async function inputExists(client, inputName) {
  return !!(await getInputRecord(client, inputName));
}

async function findSceneItem(client, sceneName, sourceName) {
  const list = await client.request("GetSceneItemList", { sceneName }).catch(() => ({ sceneItems: [] }));
  const item = Array.isArray(list.sceneItems)
    ? list.sceneItems.find((entry) => entry && entry.sourceName === sourceName)
    : null;
  return item ? item.sceneItemId : null;
}

function defaultSceneItemTransform() {
  return {
    positionX: 0,
    positionY: 0,
    scaleX: 1,
    scaleY: 1,
    rotation: 0,
    alignment: 5,
    boundsType: "OBS_BOUNDS_NONE",
    boundsAlignment: 0,
    boundsWidth: 1,
    boundsHeight: 1,
    cropLeft: 0,
    cropRight: 0,
    cropTop: 0,
    cropBottom: 0,
  };
}

async function restoreSceneItem(client, sceneName, sceneItemId) {
  await client.request("SetCurrentProgramScene", { sceneName });
  await client.request("SetSceneItemEnabled", {
    sceneName,
    sceneItemId,
    sceneItemEnabled: true,
  });
  await client.request("SetSceneItemLocked", {
    sceneName,
    sceneItemId,
    sceneItemLocked: false,
  }).catch(() => {});
  await client.request("SetSceneItemBlendMode", {
    sceneName,
    sceneItemId,
    sceneItemBlendMode: "OBS_BLEND_NORMAL",
  }).catch(() => {});
  await client.request("SetSceneItemTransform", {
    sceneName,
    sceneItemId,
    sceneItemTransform: defaultSceneItemTransform(),
  });
}

function buildSourceSettings(streamId, password, roomId, overrides = {}) {
  return {
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
    ...overrides,
  };
}

async function createColorInput(client, sceneName, inputName, kinds, color) {
  const inputKind = kinds.includes("color_source_v3")
    ? "color_source_v3"
    : kinds.includes("color_source")
      ? "color_source"
      : null;
  if (!inputKind) {
    return null;
  }
  if (await inputExists(client, inputName)) {
    const input = await getInputRecord(client, inputName);
    return {
      inputName,
      inputKind,
      sceneItemId: await findSceneItem(client, sceneName, inputName),
      inputUuid: inputUuidFromRecord(input),
    };
  }
  const response = await client.request("CreateInput", {
    sceneName,
    inputName,
    inputKind,
    inputSettings: {
      width: 1280,
      height: 720,
      color,
    },
    sceneItemEnabled: true,
  });
  return {
    inputName,
    inputKind,
    sceneItemId: response.sceneItemId,
    inputUuid: response.inputUuid || response.inputUUID || null,
  };
}

async function cleanupKnown(client, names) {
  const inputs = await client.request("GetInputList").catch(() => ({ inputs: [] }));
  const inputNames = new Set(Array.isArray(inputs.inputs) ? inputs.inputs.map((input) => input.inputName) : []);
  for (const name of names.inputs) {
    if (inputNames.has(name)) {
      await removeInput(client, name).catch(() => {});
    }
  }

  const scenes = await client.request("GetSceneList").catch(() => ({ scenes: [] }));
  const sceneNames = Array.isArray(scenes.scenes) ? scenes.scenes.map((scene) => scene.sceneName) : [];
  const fallbackScene = sceneNames.find((name) => !names.scenes.includes(name));
  if (fallbackScene) {
    await client.request("SetCurrentProgramScene", { sceneName: fallbackScene }).catch(() => {});
  }
  for (const sceneName of names.scenes) {
    if (sceneNames.includes(sceneName)) {
      await client.request("RemoveScene", { sceneName }).catch(() => {});
    }
  }
}

async function ensureChaosScene(client, config, reset) {
  const kinds = await getInputKinds(client);
  if (!kinds.includes("vdoninja_source")) {
    throw new Error("OBS does not have the vdoninja_source input kind registered");
  }

  const knownNames = {
    scenes: [config.sceneName, config.altSceneName],
    inputs: [config.inputName, config.duplicateInputName, config.browserInputName, config.colorInputName],
  };

  if (reset) {
    await cleanupKnown(client, knownNames);
  }

  if (!(await sceneExists(client, config.sceneName))) {
    await client.request("CreateScene", { sceneName: config.sceneName });
  }
  if (!(await sceneExists(client, config.altSceneName))) {
    await client.request("CreateScene", { sceneName: config.altSceneName });
  }

  await createColorInput(client, config.altSceneName, config.colorInputName, kinds, 0xff224466);
  await client.request("SetCurrentProgramScene", { sceneName: config.sceneName });

  let input = await getInputRecord(client, config.inputName);
  let inputUuid = inputUuidFromRecord(input);
  let sceneItemId = await findSceneItem(client, config.sceneName, config.inputName);
  if (!input) {
    const response = await client.request("CreateInput", {
      sceneName: config.sceneName,
      inputName: config.inputName,
      inputKind: "vdoninja_source",
      inputSettings: buildSourceSettings(config.streamId, config.password, config.roomId),
      sceneItemEnabled: true,
    });
    sceneItemId = response.sceneItemId;
    inputUuid = response.inputUuid || response.inputUUID || null;
    if (!inputUuid) {
      inputUuid = inputUuidFromRecord(await getInputRecord(client, config.inputName));
    }
  } else {
    await setInputSettings(
      client,
      config.inputName,
      inputUuid,
      buildSourceSettings(config.streamId, config.password, config.roomId)
    );
    sceneItemId = await findSceneItem(client, config.sceneName, config.inputName);
  }

  if (sceneItemId === undefined || sceneItemId === null) {
    throw new Error(`Could not find scene item for ${config.inputName}`);
  }

  await restoreSceneItem(client, config.sceneName, sceneItemId);

  return {
    inputName: config.inputName,
    inputUuid,
    sceneItemId,
    kinds,
  };
}

async function runSourceMutationCases(client, config, primary) {
  const cases = [
    {
      name: "dimensions-negative",
      settings: { width: -1, height: -1 },
      waitMs: 700,
    },
    {
      name: "dimensions-zero",
      settings: { width: 0, height: 0 },
      waitMs: 700,
    },
    {
      name: "dimensions-huge",
      settings: { width: 999999999, height: 999999999 },
      waitMs: 700,
    },
    {
      name: "browser-mode",
      settings: { use_native_receiver: false, width: 4096, height: 2160 },
      waitMs: 1600,
    },
    {
      name: "native-mode-restore",
      settings: buildSourceSettings(config.streamId, config.password, config.roomId),
      waitMs: 1800,
    },
    {
      name: "blank-stream",
      settings: { stream_id: "", room_id: "", password: "" },
      waitMs: 900,
    },
    {
      name: "unicode-room-password",
      settings: {
        stream_id: `${config.streamId}_unicode`,
        room_id: "chaos room spaces",
        password: "chaos $/#?&= snowman",
        salt: "chaos salt/#",
      },
      waitMs: 900,
    },
    {
      name: "bad-ice-force-turn",
      settings: {
        stream_id: config.streamId,
        room_id: config.roomId,
        password: config.password,
        custom_ice_servers: "https://not-ice.example\nturn:turn.example.invalid:3478|user|credential\n???",
        force_turn: true,
      },
      waitMs: 1200,
    },
    {
      name: "data-channel-off",
      settings: { enable_data_channel: false, force_turn: false },
      waitMs: 900,
    },
    {
      name: "auto-reconnect-off",
      settings: { auto_reconnect: false },
      waitMs: 900,
    },
    {
      name: "valid-final-restore",
      settings: buildSourceSettings(config.streamId, config.password, config.roomId),
      waitMs: 2500,
    },
  ];

  const results = [];
  for (const item of cases) {
    logStep(`source mutation ${item.name}`);
    const statsBefore = copyStats(await client.request("GetStats").catch(() => ({})));
    await setInputSettings(client, primary.inputName, primary.inputUuid, item.settings);
    await sleep(item.waitMs);
    const settings = await getInputSettings(client, primary.inputName, primary.inputUuid).catch(() => null);
    const statsAfter = copyStats(await client.request("GetStats").catch(() => ({})));
    results.push({
      name: item.name,
      waitMs: item.waitMs,
      requestedSettings: item.settings,
      observedSettings: settings && settings.inputSettings ? settings.inputSettings : null,
      statsBefore,
      statsAfter,
    });
  }
  return results;
}

async function runSceneVisibilityChaos(client, config, primary) {
  const iterations = parseNonNegativeInteger(process.env.VDONINJA_CHAOS_VISIBILITY_ITERATIONS, 24);
  const waitMs = parseNonNegativeInteger(process.env.VDONINJA_CHAOS_VISIBILITY_WAIT_MS, 250);
  const results = [];

  for (let index = 0; index < iterations; index += 1) {
    const sceneName = index % 4 === 0 ? config.altSceneName : config.sceneName;
    await client.request("SetCurrentProgramScene", { sceneName });
    await sleep(Math.floor(waitMs / 2));
    await client.request("SetCurrentProgramScene", { sceneName: config.sceneName });

    const enabled = index % 5 !== 0;
    await client.request("SetSceneItemEnabled", {
      sceneName: config.sceneName,
      sceneItemId: primary.sceneItemId,
      sceneItemEnabled: enabled,
    });
    await client.request("SetSceneItemTransform", {
      sceneName: config.sceneName,
      sceneItemId: primary.sceneItemId,
      sceneItemTransform: {
        positionX: (index * 31) % 520,
        positionY: (index * 19) % 260,
        scaleX: index % 2 === 0 ? 0.6 : 1,
        scaleY: index % 3 === 0 ? 0.6 : 1,
        rotation: (index * 13) % 360,
      },
    });
    results.push({ iteration: index + 1, sceneName, enabled });
    await sleep(waitMs);
  }

  await restoreSceneItem(client, config.sceneName, primary.sceneItemId);

  return {
    iterations,
    waitMs,
    results,
  };
}

async function runCreateDestroyChaos(client, config) {
  const results = [];
  const duplicateSettings = buildSourceSettings(config.streamId, config.password, config.roomId, {
    width: 640,
    height: 360,
  });
  const browserSettings = buildSourceSettings(config.streamId, config.password, config.roomId, {
    use_native_receiver: false,
    width: 1280,
    height: 720,
  });

  for (const inputName of [config.duplicateInputName, config.browserInputName]) {
    if (await inputExists(client, inputName)) {
      await removeInput(client, inputName).catch(() => {});
    }
  }

  const duplicate = await client.request("CreateInput", {
    sceneName: config.sceneName,
    inputName: config.duplicateInputName,
    inputKind: "vdoninja_source",
    inputSettings: duplicateSettings,
    sceneItemEnabled: true,
  });
  results.push({ action: "create-duplicate-native", inputName: config.duplicateInputName });
  await sleep(1000);
  await removeInput(client, config.duplicateInputName, duplicate.inputUuid || duplicate.inputUUID || null);
  results.push({ action: "remove-duplicate-native", inputName: config.duplicateInputName });
  await sleep(500);

  const browser = await client.request("CreateInput", {
    sceneName: config.sceneName,
    inputName: config.browserInputName,
    inputKind: "vdoninja_source",
    inputSettings: browserSettings,
    sceneItemEnabled: true,
  });
  const browserItemId = browser.sceneItemId;
  if (browserItemId !== undefined && browserItemId !== null) {
    await client.request("SetSceneItemTransform", {
      sceneName: config.sceneName,
      sceneItemId: browserItemId,
      sceneItemTransform: {
        positionX: 640,
        positionY: 360,
        scaleX: 0.5,
        scaleY: 0.5,
      },
    }).catch(() => {});
  }
  results.push({ action: "create-browser-viewer", inputName: config.browserInputName });
  await sleep(1200);
  await removeInput(client, config.browserInputName, browser.inputUuid || browser.inputUUID || null);
  results.push({ action: "remove-browser-viewer", inputName: config.browserInputName });

  return results;
}

async function restoreValidSource(client, config, primary) {
  await setInputSettings(
    client,
    primary.inputName,
    primary.inputUuid,
    buildSourceSettings(config.streamId, config.password, config.roomId)
  );
  await restoreSceneItem(client, config.sceneName, primary.sceneItemId);
}

async function runPhase(client, config, phase) {
  const report = {
    ok: true,
    phase,
    streamId: config.streamId,
    sceneName: config.sceneName,
    inputName: config.inputName,
    startedAt: new Date().toISOString(),
    steps: [],
  };

  if (phase === "cleanup") {
    await cleanupKnown(client, {
      scenes: [config.sceneName, config.altSceneName],
      inputs: [config.inputName, config.duplicateInputName, config.browserInputName, config.colorInputName],
    });
    report.steps.push({ name: "cleanup", ok: true });
    return report;
  }

  const reset = phase === "setup" || phase === "full";
  const primary = await ensureChaosScene(client, config, reset);
  report.primary = primary;
  report.statsBefore = copyStats(await client.request("GetStats").catch(() => ({})));

  if (phase === "setup") {
    await sleep(config.initialWaitMs);
    report.motion = await waitForMotionPair(client, config.sceneName, config.reportDir, `${phase}-motion`, {
      waitMs: config.motionWaitMs,
      minBytes: config.minScreenshotBytes,
      requireMotion: config.requireMotion,
      motionTimeoutMs: config.motionTimeoutMs,
    });
    report.statsAfter = copyStats(await client.request("GetStats").catch(() => ({})));
    return report;
  }

  if (phase === "mutate" || phase === "full") {
    report.steps.push({ name: "source-mutations", results: await runSourceMutationCases(client, config, primary) });
    report.steps.push({ name: "scene-visibility", results: await runSceneVisibilityChaos(client, config, primary) });
    report.steps.push({ name: "create-destroy", results: await runCreateDestroyChaos(client, config) });
    await restoreValidSource(client, config, primary);
    await sleep(config.finalWaitMs);
    report.motion = await waitForMotionPair(client, config.sceneName, config.reportDir, `${phase}-final-motion`, {
      waitMs: config.motionWaitMs,
      minBytes: config.minScreenshotBytes,
      requireMotion: config.requireMotion,
      motionTimeoutMs: config.motionTimeoutMs,
    });
    report.statsAfter = copyStats(await client.request("GetStats").catch(() => ({})));
    return report;
  }

  if (phase === "verify" || phase === "offline-verify") {
    await restoreValidSource(client, config, primary);
    await sleep(config.finalWaitMs);
    if (phase === "verify" || config.requireMotionWhenOffline) {
      report.motion = await waitForMotionPair(client, config.sceneName, config.reportDir, `${phase}-motion`, {
        waitMs: config.motionWaitMs,
        minBytes: config.minScreenshotBytes,
        requireMotion: config.requireMotion,
        motionTimeoutMs: config.motionTimeoutMs,
      });
    } else {
      report.screenshot = await screenshot(
        client,
        config.sceneName,
        path.join(config.reportDir, `${phase}.png`),
        Math.max(1000, Math.floor(config.minScreenshotBytes / 4))
      );
    }
    report.statsAfter = copyStats(await client.request("GetStats").catch(() => ({})));
    return report;
  }

  throw new Error(`Unknown chaos phase: ${phase}`);
}

async function main() {
  const websocketUrl = process.env.OBS_WEBSOCKET_URL || "ws://127.0.0.1:4455";
  const phase = String(process.env.VDONINJA_CHAOS_PHASE || process.argv[2] || "full").trim().toLowerCase();
  const streamId = process.env.VDONINJA_STREAM_ID || process.argv[3] || "codexChaos";
  const password = process.env.VDONINJA_PASSWORD || process.argv[4] || "false";
  const roomId = process.env.VDONINJA_ROOM_ID || process.argv[5] || "";
  const safeStreamId = streamId.replace(/[^A-Za-z0-9_.-]/g, "_");
  const reportDir = path.resolve(process.env.VDONINJA_CHAOS_REPORT_DIR || path.join("artifacts", `vdoninja-chaos-${safeStreamId}`));
  const reportPath =
    process.env.VDONINJA_CHAOS_REPORT ||
    path.join(reportDir, `chaos-${phase}-${Date.now()}.json`);
  const config = {
    streamId,
    password,
    roomId,
    sceneName: process.env.VDONINJA_CHAOS_SCENE_NAME || `Codex Chaos ${safeStreamId}`,
    altSceneName: process.env.VDONINJA_CHAOS_ALT_SCENE_NAME || `Codex Chaos Alt ${safeStreamId}`,
    inputName: process.env.VDONINJA_CHAOS_INPUT_NAME || `Codex Chaos VDO ${safeStreamId}`,
    duplicateInputName:
      process.env.VDONINJA_CHAOS_DUP_INPUT_NAME || `Codex Chaos VDO Duplicate ${safeStreamId}`,
    browserInputName:
      process.env.VDONINJA_CHAOS_BROWSER_INPUT_NAME || `Codex Chaos VDO Browser ${safeStreamId}`,
    colorInputName: process.env.VDONINJA_CHAOS_COLOR_INPUT_NAME || `Codex Chaos Color ${safeStreamId}`,
    reportDir,
    initialWaitMs: parseNonNegativeInteger(process.env.VDONINJA_CHAOS_INITIAL_WAIT_MS, 12000),
    finalWaitMs: parseNonNegativeInteger(process.env.VDONINJA_CHAOS_FINAL_WAIT_MS, 9000),
    motionWaitMs: parseNonNegativeInteger(process.env.VDONINJA_CHAOS_MOTION_WAIT_MS, 2500),
    motionTimeoutMs: parseNonNegativeInteger(process.env.VDONINJA_CHAOS_MOTION_TIMEOUT_MS, 45000),
    minScreenshotBytes: parseNonNegativeInteger(process.env.VDONINJA_MIN_SCREENSHOT_BYTES, 1500),
    requireMotion: parseBoolean(process.env.VDONINJA_CHAOS_REQUIRE_MOTION, true),
    requireMotionWhenOffline: parseBoolean(process.env.VDONINJA_CHAOS_REQUIRE_MOTION_WHEN_OFFLINE, false),
  };

  fs.mkdirSync(reportDir, { recursive: true });
  const client = new ObsWebSocketClient(websocketUrl);
  try {
    logStep(`connecting to ${websocketUrl}`);
    await client.connect();
    const version = await client.request("GetVersion").catch(() => ({}));
    const report = await runPhase(client, config, phase);
    report.websocketUrl = websocketUrl;
    report.obsVersion = version.obsVersion;
    report.obsWebSocketVersion = version.obsWebSocketVersion;
    report.finishedAt = new Date().toISOString();
    report.reportPath = path.resolve(reportPath);
    fs.writeFileSync(reportPath, JSON.stringify(report, null, 2));
    console.log(JSON.stringify(report, null, 2));
  } finally {
    await client.close().catch(() => {});
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
