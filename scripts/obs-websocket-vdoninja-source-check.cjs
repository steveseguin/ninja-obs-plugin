const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const zlib = require("zlib");
const { spawn } = require("child_process");

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

class ObsWebSocketClient {
  constructor(url, options = {}) {
    this.url = url;
    this.eventSubscriptions = options.eventSubscriptions || 0;
    this.onEvent = typeof options.onEvent === "function" ? options.onEvent : null;
    this.socket = null;
    this.requestId = 0;
    this.pending = new Map();
    this.identified = false;
    this.requestTimeoutMs = Number(process.env.OBS_WEBSOCKET_REQUEST_TIMEOUT_MS || 15000);
  }

  async connect() {
    await new Promise((resolve, reject) => {
      const socket = new WebSocket(this.url, "obswebsocket.json");
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
                  eventSubscriptions: this.eventSubscriptions,
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
            if (message.op === 5 && this.onEvent) {
              this.onEvent(message.d || {});
            }
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

const EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS = 1 << 16;

function parseBoolean(value, fallback = false) {
  if (value === undefined) {
    return fallback;
  }
  const normalized = String(value).trim().toLowerCase();
  return normalized === "1" || normalized === "true" || normalized === "yes" || normalized === "on";
}

function parseNumber(value, fallback) {
  if (value === undefined || value === null || value === "") {
    return fallback;
  }
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function logStep(message) {
  console.error(`[obs-source-check] ${message}`);
}

function selectColorSourceKind(inputKinds) {
  for (const candidate of ["color_source_v3", "color_source"]) {
    if (inputKinds.includes(candidate)) {
      return candidate;
    }
  }
  return null;
}

function paethPredictor(left, up, upLeft) {
  const p = left + up - upLeft;
  const pa = Math.abs(p - left);
  const pb = Math.abs(p - up);
  const pc = Math.abs(p - upLeft);
  if (pa <= pb && pa <= pc) {
    return left;
  }
  return pb <= pc ? up : upLeft;
}

function decodePng(buffer) {
  const signature = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  if (buffer.length < signature.length || !buffer.subarray(0, signature.length).equals(signature)) {
    throw new Error("PNG signature not found");
  }

  let offset = signature.length;
  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  const idatChunks = [];

  while (offset + 12 <= buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString("ascii", offset + 4, offset + 8);
    const dataStart = offset + 8;
    const dataEnd = dataStart + length;
    if (dataEnd + 4 > buffer.length) {
      throw new Error(`PNG chunk ${type} exceeds buffer length`);
    }
    const data = buffer.subarray(dataStart, dataEnd);
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
    } else if (type === "IDAT") {
      idatChunks.push(data);
    } else if (type === "IEND") {
      break;
    }
    offset = dataEnd + 4;
  }

  if (!width || !height) {
    throw new Error("PNG IHDR was missing dimensions");
  }
  if (bitDepth !== 8 || (colorType !== 2 && colorType !== 6)) {
    throw new Error(`Unsupported PNG format bitDepth=${bitDepth} colorType=${colorType}`);
  }

  const channels = colorType === 6 ? 4 : 3;
  const raw = zlib.inflateSync(Buffer.concat(idatChunks));
  const rowBytes = width * channels;
  const expectedRawBytes = (rowBytes + 1) * height;
  if (raw.length < expectedRawBytes) {
    throw new Error(`PNG data was shorter than expected (${raw.length}/${expectedRawBytes})`);
  }

  const filtered = Buffer.alloc(rowBytes * height);
  let rawOffset = 0;
  for (let y = 0; y < height; y += 1) {
    const filter = raw[rawOffset++];
    const rowStart = y * rowBytes;
    const prevRowStart = rowStart - rowBytes;
    for (let x = 0; x < rowBytes; x += 1) {
      const current = raw[rawOffset++];
      const left = x >= channels ? filtered[rowStart + x - channels] : 0;
      const up = y > 0 ? filtered[prevRowStart + x] : 0;
      const upLeft = y > 0 && x >= channels ? filtered[prevRowStart + x - channels] : 0;
      let value = current;
      if (filter === 1) {
        value = current + left;
      } else if (filter === 2) {
        value = current + up;
      } else if (filter === 3) {
        value = current + Math.floor((left + up) / 2);
      } else if (filter === 4) {
        value = current + paethPredictor(left, up, upLeft);
      } else if (filter !== 0) {
        throw new Error(`Unsupported PNG filter ${filter}`);
      }
      filtered[rowStart + x] = value & 0xff;
    }
  }

  const rgba = Buffer.alloc(width * height * 4);
  for (let source = 0, dest = 0; source < filtered.length; source += channels, dest += 4) {
    rgba[dest] = filtered[source];
    rgba[dest + 1] = filtered[source + 1];
    rgba[dest + 2] = filtered[source + 2];
    rgba[dest + 3] = channels === 4 ? filtered[source + 3] : 255;
  }

  return { width, height, rgba };
}

function pixelAt(image, x, y) {
  const clampedX = Math.max(0, Math.min(image.width - 1, Math.round(x)));
  const clampedY = Math.max(0, Math.min(image.height - 1, Math.round(y)));
  const index = (clampedY * image.width + clampedX) * 4;
  return {
    r: image.rgba[index],
    g: image.rgba[index + 1],
    b: image.rgba[index + 2],
    a: image.rgba[index + 3],
  };
}

function colorDistance(a, b) {
  return Math.abs(a.r - b.r) + Math.abs(a.g - b.g) + Math.abs(a.b - b.b);
}

function analyzeAlphaComposite(backgroundPngPath, finalPngPath, options = {}) {
  const background = decodePng(fs.readFileSync(backgroundPngPath));
  const final = decodePng(fs.readFileSync(finalPngPath));
  if (background.width !== final.width || background.height !== final.height) {
    throw new Error(
      `Screenshot dimensions changed between background and final captures: ` +
        `${background.width}x${background.height} -> ${final.width}x${final.height}`
    );
  }

  const expected = pixelAt(background, background.width / 2, background.height / 2);
  const tolerance = parseNumber(options.tolerance, 36);
  const step = Math.max(1, Math.trunc(parseNumber(options.sampleStep, 2)));
  let total = 0;
  let backgroundLike = 0;
  let foregroundLike = 0;
  let darkFill = 0;
  let greenFill = 0;

  for (let y = 0; y < final.height; y += step) {
    for (let x = 0; x < final.width; x += step) {
      const px = pixelAt(final, x, y);
      total += 1;
      if (colorDistance(px, expected) <= tolerance) {
        backgroundLike += 1;
      } else {
        foregroundLike += 1;
      }
      if (px.r < 12 && px.g < 12 && px.b < 12) {
        darkFill += 1;
      }
      if (px.g > 150 && px.g > px.r + 40 && px.g > px.b + 40) {
        greenFill += 1;
      }
    }
  }

  const result = {
    ok: false,
    backgroundPngPath,
    finalPngPath,
    width: final.width,
    height: final.height,
    expectedBackground: expected,
    tolerance,
    sampleStep: step,
    totalSamples: total,
    backgroundLikeSamples: backgroundLike,
    foregroundLikeSamples: foregroundLike,
    darkFillSamples: darkFill,
    greenFillSamples: greenFill,
    backgroundLikeRatio: total > 0 ? backgroundLike / total : 0,
    foregroundLikeRatio: total > 0 ? foregroundLike / total : 0,
    darkFillRatio: total > 0 ? darkFill / total : 0,
    greenFillRatio: total > 0 ? greenFill / total : 0,
  };

  const minBackgroundRatio = parseNumber(options.minBackgroundRatio, 0.03);
  const minForegroundRatio = parseNumber(options.minForegroundRatio, 0.01);
  const maxGreenFillRatio = parseNumber(options.maxGreenFillRatio, 1);
  result.minBackgroundRatio = minBackgroundRatio;
  result.minForegroundRatio = minForegroundRatio;
  result.maxGreenFillRatio = maxGreenFillRatio;
  result.ok =
    result.backgroundLikeRatio >= minBackgroundRatio &&
    result.foregroundLikeRatio >= minForegroundRatio &&
    result.greenFillRatio <= maxGreenFillRatio;
  if (!result.ok) {
    let reason = "";
    if (result.backgroundLikeRatio < minBackgroundRatio) {
      reason = `transparent/background area too small (${result.backgroundLikeRatio.toFixed(4)} < ${minBackgroundRatio})`;
    } else if (result.foregroundLikeRatio < minForegroundRatio) {
      reason = `foreground area too small (${result.foregroundLikeRatio.toFixed(4)} < ${minForegroundRatio})`;
    } else {
      reason = `green fill too large (${result.greenFillRatio.toFixed(4)} > ${maxGreenFillRatio})`;
    }
    throw new Error(`Alpha composite pixel check failed: ${reason}`);
  }
  return result;
}

function startPerturbCommand(command) {
  if (!command) {
    return null;
  }
  logStep(`starting perturb command: ${command}`);
  const child = spawn(command, {
    cwd: process.cwd(),
    shell: true,
    windowsHide: true,
  });
  const chunks = { stdout: [], stderr: [] };
  child.stdout.on("data", (chunk) => chunks.stdout.push(Buffer.from(chunk)));
  child.stderr.on("data", (chunk) => chunks.stderr.push(Buffer.from(chunk)));
  const exitPromise = new Promise((resolve) => {
    child.on("exit", (code) => resolve(code));
  });
  return { child, command, chunks, startedAt: new Date().toISOString(), exitPromise };
}

async function waitForPerturbCommand(handle, timeoutMs) {
  if (!handle) {
    return null;
  }
  const { child, chunks } = handle;
  const result = {
    command: handle.command,
    startedAt: handle.startedAt,
    finishedAt: null,
    exitCode: null,
    timedOut: false,
    stdout: "",
    stderr: "",
  };

  await new Promise((resolve) => {
    const timeout = setTimeout(() => {
      result.timedOut = true;
      child.kill();
    }, timeoutMs);
    handle.exitPromise.then((code) => {
      clearTimeout(timeout);
      result.exitCode = code;
      result.finishedAt = new Date().toISOString();
      resolve();
    });
  });

  result.stdout = Buffer.concat(chunks.stdout).toString("utf8");
  result.stderr = Buffer.concat(chunks.stderr).toString("utf8");
  return result;
}

function createAudioMeterCollector(inputName) {
  const summary = {
    inputName,
    sampleCount: 0,
    nonSilentSampleCount: 0,
    highLevelSampleCount: 0,
    maxMagnitude: 0,
    maxLevel: 0,
    firstSeenAt: null,
    lastSeenAt: null,
  };

  function observeLevel(value) {
    if (!Number.isFinite(value)) {
      return;
    }

    const abs = Math.abs(value);
    if (abs > summary.maxMagnitude) {
      summary.maxMagnitude = abs;
    }
    if (abs > summary.maxLevel) {
      summary.maxLevel = abs;
    }
    if (abs > 0.0001) {
      summary.nonSilentSampleCount += 1;
    }
    if (abs >= 0.999) {
      summary.highLevelSampleCount += 1;
    }
  }

  return {
    summary,
    onEvent(event) {
      if (!event || event.eventType !== "InputVolumeMeters") {
        return;
      }

      const inputs = event.eventData && Array.isArray(event.eventData.inputs) ? event.eventData.inputs : [];
      const target = inputs.find((input) => input && input.inputName === inputName);
      if (!target || !Array.isArray(target.inputLevelsMul)) {
        return;
      }

      summary.sampleCount += 1;
      const now = new Date().toISOString();
      if (!summary.firstSeenAt) {
        summary.firstSeenAt = now;
      }
      summary.lastSeenAt = now;

      for (const channel of target.inputLevelsMul) {
        if (!Array.isArray(channel)) {
          continue;
        }
        for (const level of channel) {
          observeLevel(Number(level));
        }
      }
    },
  };
}

async function captureSourceScreenshot(client, sourceName, outputPath, options = {}) {
  const minScreenshotBytes = Number(
    options.minScreenshotBytes || process.env.VDONINJA_MIN_SCREENSHOT_BYTES || 10000
  );
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

async function stretchSceneItemToCanvas(client, sceneName, sceneItemId, label) {
  if (sceneItemId === undefined || sceneItemId === null) {
    return;
  }

  logStep(`stretching ${label} to 1280x720 canvas`);
  await client.request("SetSceneItemTransform", {
    sceneName,
    sceneItemId,
    sceneItemTransform: {
      positionX: 0,
      positionY: 0,
      rotation: 0,
      scaleX: 1,
      scaleY: 1,
      alignment: 5,
      boundsType: "OBS_BOUNDS_STRETCH",
      boundsAlignment: 5,
      boundsWidth: 1280,
      boundsHeight: 720,
      cropLeft: 0,
      cropRight: 0,
      cropTop: 0,
      cropBottom: 0,
    },
  });
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
  const skipCapture = parseBoolean(process.env.VDONINJA_SKIP_CAPTURE, mode === "browser");
  const alphaPixelCheckEnabled = parseBoolean(process.env.VDONINJA_ALPHA_PIXEL_CHECK, false);
  const alphaBackgroundColor = Number(process.env.VDONINJA_ALPHA_BACKGROUND_COLOR || 0xffff00ff);
  const resultJsonPath = process.env.VDONINJA_SOURCE_CHECK_RESULT_JSON || "";
  const perturbCommand = process.env.VDONINJA_DURING_WAIT_COMMAND || "";
  const requirePerturbCommand = parseBoolean(process.env.VDONINJA_REQUIRE_PERTURB_COMMAND, false);
  const perturbTimeoutMs = Number(process.env.VDONINJA_PERTURB_TIMEOUT_MS || Math.max(waitMs + 15000, 30000));
  const checkAudioMeter = parseBoolean(process.env.VDONINJA_CHECK_AUDIO_METER, false);
  const failOnAudioClip = parseBoolean(process.env.VDONINJA_FAIL_ON_AUDIO_CLIP, false);
  const minAudioMeterSamples = Number(process.env.VDONINJA_MIN_AUDIO_METER_SAMPLES || 3);

  if (!streamId) {
    throw new Error("Usage: node scripts/obs-websocket-vdoninja-source-check.cjs <browser|native> <streamId> [password] [roomId]");
  }

  const stamp = Date.now();
  const sceneName = `Codex Source Smoke ${stamp}`;
  const inputName = `Codex VDO Source ${mode} ${stamp}`;
  const audioMeterCollector = checkAudioMeter ? createAudioMeterCollector(inputName) : null;
  const client = new ObsWebSocketClient(websocketUrl, {
    eventSubscriptions: checkAudioMeter ? EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS : 0,
    onEvent: audioMeterCollector ? audioMeterCollector.onEvent : null,
  });
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
    const colorSourceKind = selectColorSourceKind(kinds.inputKinds);
    if (alphaPixelCheckEnabled && !colorSourceKind) {
      throw new Error("OBS does not expose a color source input kind for alpha pixel checks");
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

    let backgroundInputName = null;
    let backgroundScreenshot = null;
    if (alphaPixelCheckEnabled) {
      backgroundInputName = `Codex Alpha Background ${stamp}`;
      logStep(`creating alpha-check background ${backgroundInputName}`);
      const backgroundInput = await client.request("CreateInput", {
        sceneName: targetSceneName,
        inputName: backgroundInputName,
        inputKind: colorSourceKind,
        inputSettings: {
          width: 1280,
          height: 720,
          color: alphaBackgroundColor,
        },
        sceneItemEnabled: true,
      });
      await stretchSceneItemToCanvas(
        client,
        targetSceneName,
        backgroundInput.sceneItemId,
        backgroundInputName
      );
      backgroundScreenshot = await captureSourceScreenshot(
        client,
        targetSceneName,
        path.resolve(process.cwd(), "artifacts", `obs-source-${mode}-background-${stamp}.png`),
        { minScreenshotBytes: 1000 }
      );
    }

    logStep(`creating input ${inputName}`);
    const sourceInput = await client.request("CreateInput", {
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
    await stretchSceneItemToCanvas(client, targetSceneName, sourceInput.sceneItemId, inputName);

    const perturb = startPerturbCommand(perturbCommand);
    logStep(`waiting ${waitMs}ms for source to render`);
    await sleep(waitMs);
    const perturbResult = await waitForPerturbCommand(perturb, perturbTimeoutMs);
    if (
      perturbResult &&
      requirePerturbCommand &&
      (perturbResult.timedOut || perturbResult.exitCode !== 0)
    ) {
      throw new Error(
        `Perturb command failed exit=${perturbResult.exitCode} timedOut=${perturbResult.timedOut}: ` +
          `${perturbResult.stderr || perturbResult.stdout}`
      );
    }

    logStep(`reading input settings for ${inputName}`);
    const inputSettings = await client.request("GetInputSettings", { inputName }).catch(() => null);
    let screenshot = null;
    if (!skipCapture) {
      logStep(`capturing screenshot for ${targetSceneName}`);
      screenshot = await captureSourceScreenshot(client, targetSceneName, screenshotPath);
    }
    if (alphaPixelCheckEnabled && backgroundScreenshot && screenshot) {
      logStep("analyzing alpha composite pixels");
    }
    const alphaPixelCheck =
      alphaPixelCheckEnabled && backgroundScreenshot && screenshot
        ? analyzeAlphaComposite(backgroundScreenshot.outputPath, screenshot.outputPath, {
            tolerance: process.env.VDONINJA_ALPHA_TOLERANCE,
            minBackgroundRatio: process.env.VDONINJA_ALPHA_MIN_BACKGROUND_RATIO,
            minForegroundRatio: process.env.VDONINJA_ALPHA_MIN_FOREGROUND_RATIO,
            maxGreenFillRatio: process.env.VDONINJA_ALPHA_MAX_GREEN_RATIO,
            sampleStep: process.env.VDONINJA_ALPHA_SAMPLE_STEP,
          })
        : null;
    if (alphaPixelCheck) {
      logStep("alpha composite pixel check passed");
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
      backgroundInputName,
      sceneName: targetSceneName,
      sourceSettings: inputSettings && inputSettings.inputSettings ? inputSettings.inputSettings : null,
      useNativeReceiver:
        inputSettings && inputSettings.inputSettings
          ? parseBoolean(inputSettings.inputSettings.use_native_receiver, useNativeReceiver)
          : useNativeReceiver,
      audioMeter: audioMeterCollector ? audioMeterCollector.summary : null,
      alphaPixelCheck,
      perturbCommand: perturbResult,
    };

    if (checkAudioMeter) {
      const audioMeter = audioMeterCollector.summary;
      if (audioMeter.sampleCount < minAudioMeterSamples) {
        throw new Error(
          `OBS mixer did not report enough audio meter samples for ${inputName}: ` +
            `${audioMeter.sampleCount}/${minAudioMeterSamples}`
        );
      }
      if (audioMeter.nonSilentSampleCount === 0 || audioMeter.maxMagnitude <= 0.0001) {
        throw new Error(`OBS mixer audio for ${inputName} stayed silent`);
      }
      if (failOnAudioClip && (audioMeter.highLevelSampleCount > 0 || audioMeter.maxLevel >= 0.999)) {
        throw new Error(
          `OBS mixer audio for ${inputName} exceeded the configured high-level threshold: ` +
            `maxLevel=${audioMeter.maxLevel}, highLevelSamples=${audioMeter.highLevelSampleCount}`
        );
      }
    }

    const resultJson = JSON.stringify(result, null, 2);
    if (resultJsonPath) {
      fs.mkdirSync(path.dirname(path.resolve(resultJsonPath)), { recursive: true });
      fs.writeFileSync(resultJsonPath, `${resultJson}\n`);
    }
    logStep("source check result ready");
    console.log(resultJson);
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
