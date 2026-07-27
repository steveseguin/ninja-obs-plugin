const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const childProcess = require("child_process");
const { chromium } = require("playwright");
const {
  analyzePcm16Le,
  createPcm16Wav,
} = require("../tests/tools/audio-continuity-analysis.cjs");

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function createToneWav(sampleRate, toneHz, durationSeconds, amplitude = 0.08) {
  const sampleCount = Math.ceil(sampleRate * durationSeconds);
  const pcm = Buffer.allocUnsafe(sampleCount * 2);
  for (let index = 0; index < sampleCount; index += 1) {
    const sample = Math.sin((2 * Math.PI * toneHz * index) / sampleRate);
    pcm.writeInt16LE(Math.round(sample * amplitude * 32767), index * 2);
  }
  return createPcm16Wav(pcm, sampleRate);
}

class ObsWebSocketClient {
  constructor(url, options = {}) {
    this.url = url;
    this.eventSubscriptions = options.eventSubscriptions || 0;
    this.onEvent =
      typeof options.onEvent === "function" ? options.onEvent : null;
    this.socket = null;
    this.requestId = 0;
    this.pending = new Map();
    this.identified = false;
    this.requestTimeoutMs = Number(
      process.env.OBS_WEBSOCKET_REQUEST_TIMEOUT_MS || 20000,
    );
  }

  async connect() {
    const deadline =
      Date.now() +
      Number(process.env.OBS_WEBSOCKET_CONNECT_TIMEOUT_MS || 60000);
    let lastError = null;

    while (Date.now() < deadline) {
      try {
        await this.connectOnce();
        return;
      } catch (error) {
        lastError = error;
        if (this.socket) {
          try {
            this.socket.close();
          } catch (_) {}
          this.socket = null;
        }
        await sleep(1000);
      }
    }

    throw new Error(
      `Timed out connecting to obs-websocket at ${this.url}: ${lastError || "unknown error"}`,
    );
  }

  async connectOnce() {
    await new Promise((resolve, reject) => {
      const socket = new WebSocket(this.url, "obswebsocket.json");
      this.socket = socket;

      socket.addEventListener("open", () => resolve());
      socket.addEventListener("error", (error) =>
        reject(new Error(error.message || "WebSocket error")),
      );
      socket.addEventListener("message", (event) => {
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
              }),
            );
            return;
          }

          if (message.op === 2) {
            this.identified = true;
            return;
          }

          if (message.op === 5 && this.onEvent) {
            this.onEvent(message.d || {});
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
            (message.d.requestStatus && message.d.requestStatus.comment) ||
            "OBS request failed";
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
        reject(
          new Error(
            `${requestType}: Timed out after ${this.requestTimeoutMs}ms`,
          ),
        );
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
      }),
    );
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

const EVENT_SUBSCRIPTION_OUTPUTS = 1 << 6;
const EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS = 1 << 16;

function ensureQuery(url, key, value) {
  const u = new URL(url);
  if (!u.searchParams.has(key)) {
    u.searchParams.set(key, value);
  }
  return u.toString();
}

function logStep(message) {
  console.error(`[obs-publish-check] ${message}`);
}

function appendBoundedOutput(current, chunk, maximumLength = 1000000) {
  const combined = current + String(chunk);
  return combined.length > maximumLength
    ? combined.slice(combined.length - maximumLength)
    : combined;
}

function startBrowserStackViewerCheck(viewUrl, outputDir, stamp) {
  const profile = String(
    process.env.VDONINJA_BROWSERSTACK_PROFILE || "",
  ).trim();
  if (!profile) {
    return null;
  }

  const outputPath = path.join(
    outputDir,
    `browserstack-viewer-${profile}-${stamp}.json`,
  );
  const scriptPath = path.resolve(
    process.cwd(),
    "scripts",
    "browserstack-vdoninja-viewer-check.cjs",
  );
  const args = [
    scriptPath,
    "--url",
    viewUrl,
    "--profile",
    profile,
    "--output",
    outputPath,
    "--quiet=1",
  ];
  const secretFile = String(
    process.env.VDONINJA_BROWSERSTACK_SECRET_FILE ||
      process.env.BROWSERSTACK_SECRET_FILE ||
      "",
  ).trim();
  if (secretFile) {
    args.push("--secret-file", secretFile);
  }
  const phases = String(process.env.VDONINJA_BROWSERSTACK_PHASES || "").trim();
  if (phases) {
    args.push("--phases", phases);
  }
  const buildName = String(
    process.env.VDONINJA_BROWSERSTACK_BUILD || "",
  ).trim();
  if (buildName) {
    args.push("--build", buildName);
  }
  const viewParams = String(
    process.env.VDONINJA_BROWSERSTACK_VIEW_PARAMS || "",
  ).trim();
  if (viewParams) {
    args.push("--view-params", viewParams);
  }
  const requiredCandidateType = String(
    process.env.VDONINJA_BROWSERSTACK_REQUIRE_CANDIDATE_TYPE || "",
  ).trim();
  if (requiredCandidateType) {
    args.push("--require-candidate-type", requiredCandidateType);
  }
  const connectTimeoutMs = String(
    process.env.VDONINJA_BROWSERSTACK_CONNECT_TIMEOUT_MS || "",
  ).trim();
  if (connectTimeoutMs) {
    args.push("--connect-timeout-ms", connectTimeoutMs);
  }
  const expectedMediaKbps = String(
    process.env.VDONINJA_BROWSERSTACK_EXPECTED_MEDIA_KBPS || "",
  ).trim();
  if (expectedMediaKbps) {
    args.push("--expected-media-kbps", expectedMediaKbps);
  }
  if (process.env.VDONINJA_BROWSERSTACK_REQUIRE_NETWORK_EFFECT === "1") {
    args.push("--require-network-effect=1");
  }

  fs.mkdirSync(outputDir, { recursive: true });
  logStep(`starting BrowserStack viewer ${profile}`);
  const processHandle = childProcess.spawn(process.execPath, args, {
    cwd: process.cwd(),
    env: process.env,
    stdio: ["ignore", "pipe", "pipe"],
    windowsHide: true,
  });
  const state = {
    process: processHandle,
    outputPath,
    stdout: "",
    stderr: "",
    finished: false,
    completion: null,
  };
  processHandle.stdout.on("data", (chunk) => {
    state.stdout = appendBoundedOutput(state.stdout, chunk);
  });
  processHandle.stderr.on("data", (chunk) => {
    state.stderr = appendBoundedOutput(state.stderr, chunk);
  });
  state.completion = new Promise((resolve) => {
    let settled = false;
    const finish = (result) => {
      if (settled) {
        return;
      }
      settled = true;
      state.finished = true;
      resolve(result);
    };
    processHandle.once("error", (error) => {
      finish({ code: null, error: String(error) });
    });
    processHandle.once("close", (code, signal) => {
      finish({ code, signal });
    });
  });
  return state;
}

async function finishBrowserStackViewerCheck(state) {
  if (!state) {
    return null;
  }
  const result = await state.completion;
  let report = null;
  if (fs.existsSync(state.outputPath)) {
    report = JSON.parse(fs.readFileSync(state.outputPath, "utf8"));
  }
  if (result.code !== 0 || !report || !report.ok) {
    throw new Error(
      `BrowserStack viewer check failed: ` +
        `${JSON.stringify({
          process: result,
          outputPath: state.outputPath,
          error: report ? report.error || null : null,
          stdout: state.stdout.slice(-4000),
          stderr: state.stderr.slice(-4000),
        })}`,
    );
  }
  logStep(
    `BrowserStack viewer ${report.profile} passed ${report.phaseResults.length} network phase(s)`,
  );
  return report;
}

function compactConsoleMessages(messages) {
  return messages.slice(-20).map((message) => ({
    type: message.type,
    text:
      message.text.length > 500
        ? `${message.text.slice(0, 500)}...(truncated)`
        : message.text,
  }));
}

function selectColorSourceKind(inputKinds) {
  for (const candidate of ["color_source_v3", "color_source"]) {
    if (inputKinds.includes(candidate)) {
      return candidate;
    }
  }
  return null;
}

async function collectViewerSnapshot(page) {
  return page.evaluate(async () => {
    const videos = Array.from(document.querySelectorAll("video")).map(
      (v, index) => {
        const stream = v.srcObject;
        const audioTracks =
          stream && stream.getAudioTracks ? stream.getAudioTracks().length : 0;
        const videoTracks =
          stream && stream.getVideoTracks ? stream.getVideoTracks().length : 0;
        return {
          index,
          readyState: v.readyState,
          paused: v.paused,
          currentTime: v.currentTime,
          videoWidth: v.videoWidth,
          videoHeight: v.videoHeight,
          audioTracks,
          videoTracks,
        };
      },
    );

    const pcStats = [];
    if (Array.isArray(window.__pcList)) {
      for (const pc of window.__pcList) {
        try {
          const stats = await pc.getStats();
          let inboundVideoBytes = 0;
          let inboundAudioBytes = 0;
          let framesDecoded = 0;
          let framesReceived = 0;
          let framesDropped = 0;
          let freezeCount = 0;
          let totalFreezesDuration = 0;
          let packetsLost = 0;
          let nackCount = 0;
          let pliCount = 0;
          let keyFramesDecoded = 0;
          let concealedSamples = 0;
          let silentConcealedSamples = 0;
          let concealmentEvents = 0;
          let insertedSamplesForDeceleration = 0;
          let removedSamplesForAcceleration = 0;
          let totalSamplesReceived = 0;
          let jitterBufferDelay = 0;
          let jitterBufferEmittedCount = 0;
          let packetsDiscarded = 0;
          stats.forEach((s) => {
            if (s.type === "inbound-rtp" && !s.isRemote) {
              packetsLost += s.packetsLost || 0;
              if (s.kind === "video") {
                inboundVideoBytes += s.bytesReceived || 0;
                framesDecoded += s.framesDecoded || 0;
                framesReceived += s.framesReceived || 0;
                framesDropped += s.framesDropped || 0;
                freezeCount += s.freezeCount || 0;
                totalFreezesDuration += s.totalFreezesDuration || 0;
                nackCount += s.nackCount || 0;
                pliCount += s.pliCount || 0;
                keyFramesDecoded += s.keyFramesDecoded || 0;
              }
              if (s.kind === "audio") {
                inboundAudioBytes += s.bytesReceived || 0;
                concealedSamples += s.concealedSamples || 0;
                silentConcealedSamples += s.silentConcealedSamples || 0;
                concealmentEvents += s.concealmentEvents || 0;
                insertedSamplesForDeceleration +=
                  s.insertedSamplesForDeceleration || 0;
                removedSamplesForAcceleration +=
                  s.removedSamplesForAcceleration || 0;
                totalSamplesReceived += s.totalSamplesReceived || 0;
                jitterBufferDelay += s.jitterBufferDelay || 0;
                jitterBufferEmittedCount += s.jitterBufferEmittedCount || 0;
                packetsDiscarded += s.packetsDiscarded || 0;
              }
            }
          });
          pcStats.push({
            state: pc.connectionState,
            inboundVideoBytes,
            inboundAudioBytes,
            framesDecoded,
            framesReceived,
            framesDropped,
            freezeCount,
            totalFreezesDuration,
            packetsLost,
            nackCount,
            pliCount,
            keyFramesDecoded,
            concealedSamples,
            silentConcealedSamples,
            concealmentEvents,
            insertedSamplesForDeceleration,
            removedSamplesForAcceleration,
            totalSamplesReceived,
            jitterBufferDelay,
            jitterBufferEmittedCount,
            packetsDiscarded,
          });
        } catch (error) {
          pcStats.push({ error: String(error) });
        }
      }
    }

    return {
      url: location.href,
      title: document.title || "",
      textSample: (document.body ? document.body.innerText || "" : "").slice(
        0,
        300,
      ),
      videos,
      pcStats,
      timestamp: Date.now(),
    };
  });
}

function hasPlayableMedia(snapshot) {
  return snapshot.videos.some(
    (video) =>
      video.videoTracks > 0 && video.videoWidth > 0 && video.videoHeight > 0,
  );
}

function totalInboundBytes(snapshot) {
  return snapshot.pcStats.reduce(
    (total, stat) =>
      total + (stat.inboundVideoBytes || 0) + (stat.inboundAudioBytes || 0),
    0,
  );
}

function playbackAdvanced(before, after) {
  return after.videos.some((v2) => {
    const v1 = before.videos.find((candidate) => candidate.index === v2.index);
    return v1 && v2.currentTime > v1.currentTime + 0.4;
  });
}

function totalPcMetric(snapshot, key) {
  return snapshot.pcStats.reduce(
    (total, stat) => total + (Number(stat[key]) || 0),
    0,
  );
}

function saveObsScreenshot(imageData, filePath) {
  const match = /^data:image\/[^;]+;base64,(.+)$/s.exec(imageData || "");
  if (!match) {
    throw new Error("OBS returned an invalid source screenshot");
  }
  const bytes = Buffer.from(match[1], "base64");
  fs.writeFileSync(filePath, bytes);
  return {
    path: filePath,
    bytes: bytes.length,
    sha256: crypto.createHash("sha256").update(bytes).digest("hex"),
  };
}

function inspectObsScreenshot(imageData) {
  const match = /^data:image\/[^;]+;base64,(.+)$/s.exec(imageData || "");
  if (!match) {
    throw new Error("OBS returned an invalid source screenshot");
  }
  const bytes = Buffer.from(match[1], "base64");
  return {
    bytes: bytes.length,
    sha256: crypto.createHash("sha256").update(bytes).digest("hex"),
  };
}

async function startDecodedAudioCapture(page) {
  return page.evaluate(async () => {
    const mediaElement = Array.from(document.querySelectorAll("video")).find(
      (candidate) => {
        const stream = candidate.srcObject;
        return (
          stream && stream.getAudioTracks && stream.getAudioTracks().length > 0
        );
      },
    );
    if (!mediaElement) {
      throw new Error(
        "No viewer media element with an audio track is available for PCM capture",
      );
    }

    const AudioContext = window.AudioContext || window.webkitAudioContext;
    if (!AudioContext) {
      throw new Error("The viewer runtime does not expose Web Audio");
    }

    const audioTrack = mediaElement.srcObject.getAudioTracks()[0];
    const audioContext = new AudioContext({ sampleRate: 48000 });
    const sourceStream = new MediaStream([audioTrack]);
    const source = audioContext.createMediaStreamSource(sourceStream);
    const processor = audioContext.createScriptProcessor(4096, 1, 1);
    const silentOutput = audioContext.createGain();
    silentOutput.gain.value = 0;

    const capture = {
      audioContext,
      source,
      processor,
      silentOutput,
      chunks: [],
      sampleCount: 0,
      rawTrack: null,
    };
    processor.onaudioprocess = (event) => {
      const samples = event.inputBuffer.getChannelData(0);
      capture.chunks.push(new Float32Array(samples));
      capture.sampleCount += samples.length;
    };
    source.connect(processor);
    processor.connect(silentOutput);
    silentOutput.connect(audioContext.destination);

    if (typeof MediaStreamTrackProcessor === "function") {
      const clonedTrack = audioTrack.clone();
      const trackProcessor = new MediaStreamTrackProcessor({
        track: clonedTrack,
      });
      const reader = trackProcessor.readable.getReader();
      const rawTrack = {
        clonedTrack,
        reader,
        chunks: [],
        sampleCount: 0,
        sampleRate: 0,
        firstTimestamp: null,
        lastTimestamp: null,
        lastDuration: null,
        maxTimestampStep: 0,
        nonForwardTimestamps: 0,
        timestampGaps: [],
        active: true,
        error: null,
        loop: null,
      };
      rawTrack.loop = (async () => {
        try {
          while (rawTrack.active) {
            const { value, done } = await reader.read();
            if (done || !value) {
              break;
            }
            try {
              const chunkStartSample = rawTrack.sampleCount;
              const samples = new Float32Array(value.numberOfFrames);
              value.copyTo(samples, { planeIndex: 0, format: "f32-planar" });
              rawTrack.chunks.push(samples);
              rawTrack.sampleCount += samples.length;
              rawTrack.sampleRate = value.sampleRate;
              if (rawTrack.firstTimestamp === null) {
                rawTrack.firstTimestamp = value.timestamp;
              } else {
                const timestampStep = value.timestamp - rawTrack.lastTimestamp;
                rawTrack.maxTimestampStep = Math.max(
                  rawTrack.maxTimestampStep,
                  timestampStep,
                );
                if (timestampStep <= 0) {
                  rawTrack.nonForwardTimestamps += 1;
                } else if (
                  timestampStep >
                    Math.max(
                      12000,
                      Number(rawTrack.lastDuration || value.duration || 0) +
                        2000,
                    ) &&
                  rawTrack.timestampGaps.length < 50
                ) {
                  rawTrack.timestampGaps.push({
                    chunkStartSample,
                    captureTimeSeconds:
                      rawTrack.sampleRate > 0
                        ? chunkStartSample / rawTrack.sampleRate
                        : null,
                    previousTimestamp: rawTrack.lastTimestamp,
                    timestamp: value.timestamp,
                    timestampStep,
                    previousDuration: rawTrack.lastDuration,
                    receiveWallTimeMs: Date.now(),
                  });
                }
              }
              rawTrack.lastTimestamp = value.timestamp;
              rawTrack.lastDuration = value.duration;
            } finally {
              value.close();
            }
          }
        } catch (error) {
          rawTrack.error = String(error && error.stack ? error.stack : error);
        }
      })();
      capture.rawTrack = rawTrack;
    }

    window.__vdoninjaDecodedAudioCapture = capture;
    await audioContext.resume();

    return {
      sampleRate: audioContext.sampleRate,
      contextState: audioContext.state,
      trackSettings: audioTrack.getSettings ? audioTrack.getSettings() : {},
      rawTrackCaptureAvailable: Boolean(capture.rawTrack),
    };
  });
}

async function stopDecodedAudioCapture(page) {
  return page.evaluate(async () => {
    const capture = window.__vdoninjaDecodedAudioCapture;
    if (!capture) {
      throw new Error("Decoded-audio capture was not started");
    }

    capture.processor.onaudioprocess = null;
    capture.source.disconnect();
    capture.processor.disconnect();
    capture.silentOutput.disconnect();

    if (capture.rawTrack) {
      capture.rawTrack.active = false;
      await capture.rawTrack.reader.cancel().catch(() => {});
      await capture.rawTrack.loop;
      capture.rawTrack.clonedTrack.stop();
    }

    function encodeChunks(chunks, sampleCount) {
      const pcm = new Int16Array(sampleCount);
      let destinationOffset = 0;
      for (const chunk of chunks) {
        for (let index = 0; index < chunk.length; index += 1) {
          const sample = Math.max(-1, Math.min(1, chunk[index]));
          pcm[destinationOffset + index] = Math.round(sample * 32767);
        }
        destinationOffset += chunk.length;
      }

      const bytes = new Uint8Array(pcm.buffer);
      let binary = "";
      const blockSize = 32768;
      for (let offset = 0; offset < bytes.length; offset += blockSize) {
        binary += String.fromCharCode(
          ...bytes.subarray(offset, Math.min(offset + blockSize, bytes.length)),
        );
      }
      return btoa(binary);
    }

    const result = {
      sampleRate: capture.audioContext.sampleRate,
      sampleCount: capture.sampleCount,
      pcmBase64: encodeChunks(capture.chunks, capture.sampleCount),
      rawTrack: capture.rawTrack
        ? {
            sampleRate: capture.rawTrack.sampleRate,
            sampleCount: capture.rawTrack.sampleCount,
            pcmBase64: encodeChunks(
              capture.rawTrack.chunks,
              capture.rawTrack.sampleCount,
            ),
            firstTimestamp: capture.rawTrack.firstTimestamp,
            lastTimestamp: capture.rawTrack.lastTimestamp,
            lastDuration: capture.rawTrack.lastDuration,
            maxTimestampStep: capture.rawTrack.maxTimestampStep,
            nonForwardTimestamps: capture.rawTrack.nonForwardTimestamps,
            timestampGaps: capture.rawTrack.timestampGaps,
            error: capture.rawTrack.error,
          }
        : null,
    };
    await capture.audioContext.close();
    delete window.__vdoninjaDecodedAudioCapture;
    return result;
  });
}

async function waitForStreamActive(client, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let lastStatus = null;
  while (Date.now() < deadline) {
    lastStatus = await client
      .request("GetStreamStatus")
      .catch((error) => ({ error: String(error) }));
    if (lastStatus && lastStatus.outputActive) {
      return lastStatus;
    }
    await sleep(1000);
  }
  throw new Error(
    `OBS stream did not become active; last status=${JSON.stringify(lastStatus)}`,
  );
}

async function main() {
  const streamId =
    process.env.VDONINJA_STREAM_ID ||
    process.argv[2] ||
    `codexObs${Date.now()}`;
  const password = process.env.VDONINJA_PASSWORD || process.argv[3] || "false";
  const roomId = process.env.VDONINJA_ROOM_ID || process.argv[4] || "";
  const websocketUrl = process.env.OBS_WEBSOCKET_URL || "ws://127.0.0.1:4455";
  const waitMs = Number(process.env.VDONINJA_WAIT_MS || 90000);
  const soakMs = Number(process.env.VDONINJA_SOAK_MS || 7000);
  const sourceMode = String(
    process.env.VDONINJA_SOURCE_MODE || "static",
  ).toLowerCase();
  const useMotionSource = sourceMode === "motion";
  const useAudioContinuitySource = sourceMode === "audio-continuity";
  const useGeneratedBrowserSource = useMotionSource || useAudioContinuitySource;
  const requireAudioContinuity =
    useAudioContinuitySource ||
    process.env.VDONINJA_REQUIRE_AUDIO_CONTINUITY === "1";
  const captureDecodedAudio =
    requireAudioContinuity ||
    process.env.VDONINJA_CAPTURE_DECODED_AUDIO === "1";
  const recordLocalOutput = process.env.VDONINJA_RECORD_LOCAL_OUTPUT === "1";
  const useObsBrowserViewer = process.env.VDONINJA_OBS_BROWSER_VIEWER === "1";
  const useNativeViewer = process.env.VDONINJA_NATIVE_VIEWER === "1";
  const nativeViewerWidth = Math.max(
    320,
    Number(process.env.VDONINJA_NATIVE_VIEWER_WIDTH || 640),
  );
  const nativeViewerHeight = Math.max(
    240,
    Number(process.env.VDONINJA_NATIVE_VIEWER_HEIGHT || 360),
  );
  const requestedObsBrowserViewerCount = Number(
    process.env.VDONINJA_OBS_BROWSER_VIEWER_COUNT || 1,
  );
  const obsBrowserViewerCount = useObsBrowserViewer
    ? Math.max(
        1,
        Math.min(
          10,
          Number.isFinite(requestedObsBrowserViewerCount)
            ? Math.floor(requestedObsBrowserViewerCount)
            : 1,
        ),
      )
    : 0;
  const obsBrowserViewerBitratesKbps = String(
    process.env.VDONINJA_OBS_BROWSER_VIEWER_BITRATES_KBPS || "",
  )
    .split(",")
    .map((value) => {
      const parsed = Number(value.trim());
      return Number.isFinite(parsed) && parsed > 0 ? parsed : null;
    });
  const skipChromiumViewer = process.env.VDONINJA_SKIP_CHROMIUM_VIEWER === "1";
  const requireZeroFreezes = process.env.VDONINJA_REQUIRE_ZERO_FREEZES === "1";
  const obsBrowserSampleMs = Math.max(
    100,
    Number(process.env.VDONINJA_OBS_BROWSER_SAMPLE_MS || 1000),
  );
  const requestedObsBrowserScreenshotFormat = String(
    process.env.VDONINJA_OBS_BROWSER_SCREENSHOT_FORMAT || "png",
  ).toLowerCase();
  const obsBrowserScreenshotFormat = ["jpg", "jpeg", "png"].includes(
    requestedObsBrowserScreenshotFormat,
  )
    ? requestedObsBrowserScreenshotFormat
    : "png";
  const obsBrowserScreenshotQuality = Math.max(
    1,
    Math.min(
      100,
      Number(process.env.VDONINJA_OBS_BROWSER_SCREENSHOT_QUALITY || 80),
    ),
  );
  const obsBrowserMinimumScreenshotBytes = Math.max(
    1000,
    Number(
      process.env.VDONINJA_OBS_BROWSER_MIN_SCREENSHOT_BYTES ||
        (obsBrowserScreenshotFormat === "png" ? 100000 : 20000),
    ),
  );
  const viewBufferMs = Math.max(
    0,
    Number(process.env.VDONINJA_VIEW_BUFFER_MS || 0),
  );
  const videoProtectionMode = Math.max(
    0,
    Math.min(3, Number(process.env.VDONINJA_VIDEO_PROTECTION_MODE || 0)),
  );
  const audioRed = process.env.VDONINJA_AUDIO_RED === "1";
  const requestAudioRed =
    audioRed && process.env.VDONINJA_VIEWER_AUDIO_RED !== "0";
  const adaptiveBitrate = process.env.VDONINJA_ADAPTIVE_BITRATE === "1";
  const adaptiveBitrateMinimumKbps = Math.max(
    100,
    Number(process.env.VDONINJA_ADAPTIVE_BITRATE_MIN_KBPS || 500),
  );
  const requestedVideoWidth = Number(process.env.VDONINJA_VIDEO_WIDTH || 0);
  const requestedVideoHeight = Number(process.env.VDONINJA_VIDEO_HEIGHT || 0);
  const requestedFpsNumerator = Number(
    process.env.VDONINJA_VIDEO_FPS_NUMERATOR || 0,
  );
  const requestedFpsDenominator = Number(
    process.env.VDONINJA_VIDEO_FPS_DENOMINATOR || 0,
  );
  const requestedVideoBitrateKbps = Number(
    process.env.VDONINJA_VIDEO_BITRATE_KBPS || 0,
  );
  if (skipChromiumViewer && !useObsBrowserViewer) {
    throw new Error(
      "VDONINJA_SKIP_CHROMIUM_VIEWER requires VDONINJA_OBS_BROWSER_VIEWER=1",
    );
  }
  if (
    requestedVideoWidth > 0 !== requestedVideoHeight > 0 ||
    requestedFpsNumerator > 0 !== requestedFpsDenominator > 0
  ) {
    throw new Error(
      "Video width/height and FPS numerator/denominator must be supplied in pairs",
    );
  }
  if (
    requestedVideoWidth < 0 ||
    requestedVideoHeight < 0 ||
    requestedFpsNumerator < 0 ||
    requestedFpsDenominator < 0 ||
    requestedVideoBitrateKbps < 0
  ) {
    throw new Error("Requested video settings must not be negative");
  }
  const outputDir = path.resolve(process.cwd(), "artifacts");
  const stamp = Date.now();
  const sceneName = `Codex OBS Publish ${stamp}`;
  const sourceLabel = useAudioContinuitySource
    ? "Audio Continuity"
    : useMotionSource
      ? "Motion"
      : "Color";
  const inputName = `Codex ${sourceLabel} Program ${stamp}`;
  const audioInputName = `Codex Audio Continuity Tone ${stamp}`;
  const obsBrowserViewerName = `Codex OBS Browser Viewer ${stamp}`;
  const nativeViewerName = `Codex Native Viewer ${stamp}`;
  const motionSourcePath = path.resolve(
    process.cwd(),
    "tests",
    "tools",
    "publish-motion-source.html",
  );
  const audioContinuitySourcePath = path.resolve(
    process.cwd(),
    "tests",
    "tools",
    "publish-audio-continuity-source.html",
  );
  const generatedSourcePath = useAudioContinuitySource
    ? audioContinuitySourcePath
    : motionSourcePath;
  const sourceToneDurationSeconds = useAudioContinuitySource
    ? Math.ceil((soakMs + waitMs + 30000) / 1000)
    : 0;
  const sourceTonePath = useAudioContinuitySource
    ? path.join(outputDir, `obs-publish-source-tone-${stamp}.wav`)
    : null;
  const viewParams = new URLSearchParams();
  viewParams.set("view", streamId);
  if (roomId) {
    viewParams.set("room", roomId);
    viewParams.set("solo", "");
  }
  if (password) {
    viewParams.set("password", password);
  }
  if (viewBufferMs > 0) {
    viewParams.set("buffer", String(viewBufferMs));
  }
  if (requestAudioRed) {
    viewParams.set("audiocodec", "red");
  } else if (audioRed) {
    viewParams.set("audiocodec", "opus");
  }
  viewParams.set("debug", "");
  const viewUrl = ensureQuery(
    `https://vdo.ninja/?${viewParams.toString()}`,
    "cleanoutput",
    "1",
  );
  const consoleMessages = [];
  const pageErrors = [];
  const streamEvents = [];
  let previousSceneName = null;
  let createdScene = false;
  let createdObsBrowserViewer = false;
  let obsBrowserViewerUuid = null;
  const obsBrowserViewers = [];
  let nativeViewer = null;
  let browser = null;
  let recordingStarted = false;
  let localRecording = null;
  let originalVideoSettings = null;
  let appliedVideoSettings = null;
  let originalVideoBitrateParameter = null;
  let appliedVideoBitrateParameter = null;
  let browserStackViewerCheck = null;
  let browserStackViewerReport = null;
  const obsBrowserAudioMeter = {
    events: 0,
    nonSilentEvents: 0,
    maxMagnitude: 0,
    maxPeak: 0,
  };

  const client = new ObsWebSocketClient(websocketUrl, {
    eventSubscriptions:
      EVENT_SUBSCRIPTION_OUTPUTS |
      (useObsBrowserViewer && useAudioContinuitySource
        ? EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS
        : 0),
    onEvent(event) {
      if (event.eventType && /Stream|Output/i.test(event.eventType)) {
        streamEvents.push(event);
      }
      if (
        event.eventType === "InputVolumeMeters" &&
        Array.isArray(event.eventData?.inputs)
      ) {
        for (const input of event.eventData.inputs) {
          if (
            input.inputName !== obsBrowserViewerName &&
            !String(input.inputName || "").startsWith(
              `${obsBrowserViewerName} `,
            )
          ) {
            continue;
          }
          obsBrowserAudioMeter.events += 1;
          let eventMagnitude = 0;
          let eventPeak = 0;
          for (const channel of input.inputLevelsMul || []) {
            eventMagnitude = Math.max(
              eventMagnitude,
              Number(channel?.[0]) || 0,
            );
            eventPeak = Math.max(eventPeak, Number(channel?.[1]) || 0);
          }
          obsBrowserAudioMeter.maxMagnitude = Math.max(
            obsBrowserAudioMeter.maxMagnitude,
            eventMagnitude,
          );
          obsBrowserAudioMeter.maxPeak = Math.max(
            obsBrowserAudioMeter.maxPeak,
            eventPeak,
          );
          if (eventMagnitude > 0.00001 || eventPeak > 0.00001) {
            obsBrowserAudioMeter.nonSilentEvents += 1;
          }
        }
      }
    },
  });

  try {
    logStep(`connecting to ${websocketUrl}`);
    await client.connect();

    logStep("querying OBS/plugin capabilities");
    const version = await client.request("GetVersion");
    const kinds = await client.request("GetInputKindList", {
      unversioned: false,
    });
    const inputKinds = Array.isArray(kinds.inputKinds) ? kinds.inputKinds : [];
    if (!inputKinds.includes("vdoninja_source")) {
      throw new Error(
        "OBS does not have the vdoninja_source input kind registered",
      );
    }
    const colorKind = selectColorSourceKind(inputKinds);
    if (!useGeneratedBrowserSource && !colorKind) {
      throw new Error("OBS does not expose a color source input kind");
    }
    if (
      (useGeneratedBrowserSource || useObsBrowserViewer) &&
      !inputKinds.includes("browser_source")
    ) {
      throw new Error("OBS does not expose the browser_source input kind");
    }
    if (useGeneratedBrowserSource && !fs.existsSync(generatedSourcePath)) {
      throw new Error(
        `Generated browser source file is missing: ${generatedSourcePath}`,
      );
    }
    if (useAudioContinuitySource && !inputKinds.includes("ffmpeg_source")) {
      throw new Error("OBS does not expose the Media Source input kind");
    }
    if (useAudioContinuitySource) {
      fs.mkdirSync(outputDir, { recursive: true });
      fs.writeFileSync(
        sourceTonePath,
        createToneWav(48000, 997, sourceToneDurationSeconds),
      );
    }

    if (requestedVideoWidth > 0 || requestedFpsNumerator > 0) {
      originalVideoSettings = await client.request("GetVideoSettings");
      const requestedSettings = {};
      if (requestedVideoWidth > 0) {
        requestedSettings.baseWidth = requestedVideoWidth;
        requestedSettings.baseHeight = requestedVideoHeight;
        requestedSettings.outputWidth = requestedVideoWidth;
        requestedSettings.outputHeight = requestedVideoHeight;
      }
      if (requestedFpsNumerator > 0) {
        requestedSettings.fpsNumerator = requestedFpsNumerator;
        requestedSettings.fpsDenominator = requestedFpsDenominator;
      }
      logStep(
        `temporarily applying OBS video settings ${JSON.stringify(requestedSettings)}`,
      );
      await client.request("SetVideoSettings", requestedSettings);
      appliedVideoSettings = await client.request("GetVideoSettings");
      for (const [key, value] of Object.entries(requestedSettings)) {
        if (Number(appliedVideoSettings[key]) !== Number(value)) {
          throw new Error(
            `OBS did not apply ${key}=${value}; observed ${appliedVideoSettings[key]}`,
          );
        }
      }
    }

    if (requestedVideoBitrateKbps > 0) {
      originalVideoBitrateParameter = await client.request(
        "GetProfileParameter",
        {
          parameterCategory: "SimpleOutput",
          parameterName: "VBitrate",
        },
      );
      logStep(
        `temporarily applying OBS video bitrate ${requestedVideoBitrateKbps} kbps`,
      );
      await client.request("SetProfileParameter", {
        parameterCategory: "SimpleOutput",
        parameterName: "VBitrate",
        parameterValue: String(requestedVideoBitrateKbps),
      });
      appliedVideoBitrateParameter = await client.request(
        "GetProfileParameter",
        {
          parameterCategory: "SimpleOutput",
          parameterName: "VBitrate",
        },
      );
      if (
        Number(appliedVideoBitrateParameter.parameterValue) !==
        requestedVideoBitrateKbps
      ) {
        throw new Error(
          `OBS did not apply VBitrate=${requestedVideoBitrateKbps}; observed ` +
            `${appliedVideoBitrateParameter.parameterValue}`,
        );
      }
    }

    const currentProgram = await client
      .request("GetCurrentProgramScene")
      .catch(() => ({}));
    previousSceneName = currentProgram.currentProgramSceneName || null;

    logStep(`creating scene ${sceneName}`);
    await client.request("CreateScene", { sceneName });
    createdScene = true;
    if (useGeneratedBrowserSource) {
      await client.request("CreateInput", {
        sceneName,
        inputName,
        inputKind: "browser_source",
        inputSettings: {
          is_local_file: true,
          local_file: generatedSourcePath,
          width: 1920,
          height: 1080,
          fps: 60,
          shutdown: false,
          restart_when_active: false,
        },
        sceneItemEnabled: true,
      });
    } else {
      await client.request("CreateInput", {
        sceneName,
        inputName,
        inputKind: colorKind,
        inputSettings: {
          color: 4278233600,
          width: 1280,
          height: 720,
        },
        sceneItemEnabled: true,
      });
    }
    if (useAudioContinuitySource) {
      await client.request("CreateInput", {
        sceneName,
        inputName: audioInputName,
        inputKind: "ffmpeg_source",
        inputSettings: {
          is_local_file: true,
          local_file: sourceTonePath,
          looping: false,
          restart_on_activate: false,
          close_when_inactive: false,
          clear_on_media_end: false,
        },
        sceneItemEnabled: true,
      });
    }
    await client.request("SetCurrentProgramScene", { sceneName });
    if (recordLocalOutput) {
      logStep("starting a simultaneous local OBS recording");
      await client.request("StartRecord");
      recordingStarted = true;
    }

    logStep(`configuring VDO.Ninja stream service for ${streamId}`);
    await client.request("SetStreamServiceSettings", {
      streamServiceType: "vdoninja_service",
      streamServiceSettings: {
        stream_id: streamId,
        room_id: roomId,
        password,
        wss_host: "",
        salt: "",
        max_viewers: 10,
        video_codec: 0,
        enable_data_channel: true,
        auto_reconnect: true,
        video_protection_mode: videoProtectionMode,
        audio_red: audioRed,
        adaptive_bitrate: adaptiveBitrate,
        adaptive_bitrate_min: adaptiveBitrateMinimumKbps,
      },
    });

    logStep("starting OBS stream");
    await client.request("StartStream");
    const activeStatus = await waitForStreamActive(client, 30000);
    browserStackViewerCheck = startBrowserStackViewerCheck(
      viewUrl,
      outputDir,
      stamp,
    );
    if (useNativeViewer) {
      logStep(
        `creating native VDO.Ninja viewer ${nativeViewerName} at ` +
          `${nativeViewerWidth}x${nativeViewerHeight}`,
      );
      const createdNativeViewer = await client.request("CreateInput", {
        sceneName,
        inputName: nativeViewerName,
        inputKind: "vdoninja_source",
        inputSettings: {
          use_native_receiver: true,
          stream_id: streamId,
          room_id: roomId,
          password,
          wss_host: "",
          salt: "",
          width: nativeViewerWidth,
          height: nativeViewerHeight,
          enable_data_channel: true,
          auto_reconnect: true,
          force_turn: false,
        },
        sceneItemEnabled: true,
      });
      nativeViewer = {
        name: nativeViewerName,
        uuid: createdNativeViewer.inputUuid || null,
        sceneItemId: createdNativeViewer.sceneItemId ?? null,
        width: nativeViewerWidth,
        height: nativeViewerHeight,
      };
      await client.request("SetInputMute", {
        ...(nativeViewer.uuid
          ? { inputUuid: nativeViewer.uuid }
          : { inputName: nativeViewer.name }),
        inputMuted: true,
      });
      if (nativeViewer.sceneItemId !== null) {
        await client.request("SetSceneItemTransform", {
          sceneName,
          sceneItemId: nativeViewer.sceneItemId,
          sceneItemTransform: {
            positionX: 8192,
            positionY: 0,
          },
        });
      }
    }
    if (useObsBrowserViewer) {
      for (
        let viewerIndex = 0;
        viewerIndex < obsBrowserViewerCount;
        viewerIndex += 1
      ) {
        const viewerName =
          viewerIndex === 0
            ? obsBrowserViewerName
            : `${obsBrowserViewerName} ${viewerIndex + 1}`;
        const viewerUrl = new URL(viewUrl);
        viewerUrl.searchParams.set("autostart", "1");
        const viewerBitrateKbps =
          obsBrowserViewerBitratesKbps[viewerIndex] || null;
        if (viewerBitrateKbps) {
          viewerUrl.searchParams.set("bitrate", String(viewerBitrateKbps));
        }
        logStep(
          `creating actual OBS Browser Source viewer ${viewerName}` +
            (viewerBitrateKbps
              ? ` with ${viewerBitrateKbps} kbps receive target`
              : ""),
        );
        const createdViewer = await client.request("CreateInput", {
          sceneName,
          inputName: viewerName,
          inputKind: "browser_source",
          inputSettings: {
            is_local_file: false,
            url: viewerUrl.toString(),
            width: 640,
            height: 360,
            fps: 60,
            shutdown: false,
            restart_when_active: false,
            reroute_audio: true,
          },
          sceneItemEnabled: true,
        });
        const viewer = {
          name: viewerName,
          uuid: createdViewer.inputUuid || null,
          sceneItemId: createdViewer.sceneItemId ?? null,
          bitrateKbps: viewerBitrateKbps,
        };
        obsBrowserViewers.push(viewer);
        await client.request("SetInputMute", {
          ...(viewer.uuid
            ? { inputUuid: viewer.uuid }
            : { inputName: viewer.name }),
          inputMuted: true,
        });
        if (viewer.sceneItemId !== null) {
          // Keep each real OBS Browser Source active and renderable without
          // publishing a recursive copy of either its video or audio.
          await client.request("SetSceneItemTransform", {
            sceneName,
            sceneItemId: viewer.sceneItemId,
            sceneItemTransform: {
              positionX: 4096 + viewerIndex * 640,
              positionY: 0,
            },
          });
        }
      }
      createdObsBrowserViewer = obsBrowserViewers.length > 0;
      obsBrowserViewerUuid = obsBrowserViewers[0]?.uuid || null;
    }

    if (skipChromiumViewer) {
      fs.mkdirSync(outputDir, { recursive: true });
      const screenshotRequest = {
        ...(obsBrowserViewerUuid
          ? { sourceUuid: obsBrowserViewerUuid }
          : { sourceName: obsBrowserViewerName }),
        imageFormat: obsBrowserScreenshotFormat,
        imageWidth: 640,
        imageHeight: 360,
        ...(obsBrowserScreenshotFormat === "png"
          ? {}
          : { imageCompressionQuality: obsBrowserScreenshotQuality }),
      };
      const playableDeadline = Date.now() + waitMs;
      let previousProbe = null;
      let playableImageData = null;
      while (Date.now() < playableDeadline) {
        const response = await client.request(
          "GetSourceScreenshot",
          screenshotRequest,
        );
        const probe = inspectObsScreenshot(response.imageData);
        if (
          probe.bytes >= obsBrowserMinimumScreenshotBytes &&
          previousProbe &&
          previousProbe.sha256 !== probe.sha256
        ) {
          playableImageData = response.imageData;
          break;
        }
        previousProbe = probe;
        await sleep(1000);
      }
      if (!playableImageData) {
        throw new Error(
          `OBS Browser Source did not render advancing high-detail media; ` +
            `latest=${JSON.stringify(previousProbe)}`,
        );
      }

      const firstObsBrowserScreenshot = saveObsScreenshot(
        playableImageData,
        path.join(
          outputDir,
          `obs-browser-viewer-first-${stamp}.${obsBrowserScreenshotFormat}`,
        ),
      );
      const browserSamples = [
        {
          timestamp: Date.now(),
          ...inspectObsScreenshot(playableImageData),
        },
      ];
      let latestImageData = playableImageData;
      let repeatedSamples = 0;
      const soakDeadline = Date.now() + soakMs;
      while (Date.now() < soakDeadline) {
        await sleep(
          Math.min(obsBrowserSampleMs, Math.max(1, soakDeadline - Date.now())),
        );
        const response = await client.request(
          "GetSourceScreenshot",
          screenshotRequest,
        );
        latestImageData = response.imageData;
        const sample = {
          timestamp: Date.now(),
          ...inspectObsScreenshot(latestImageData),
        };
        const previousSample = browserSamples[browserSamples.length - 1];
        const sampleIntervalMs = sample.timestamp - previousSample.timestamp;
        sample.sampleIntervalMs = sampleIntervalMs;
        if (
          previousSample.sha256 === sample.sha256 &&
          sampleIntervalMs >= obsBrowserSampleMs * 0.8
        ) {
          repeatedSamples += 1;
          logStep(
            `OBS Browser Source repeated image sample ${repeatedSamples} at ${sample.timestamp} ` +
              `after ${sampleIntervalMs} ms`,
          );
        }
        browserSamples.push(sample);
      }
      const secondObsBrowserScreenshot = saveObsScreenshot(
        latestImageData,
        path.join(
          outputDir,
          `obs-browser-viewer-final-${stamp}.${obsBrowserScreenshotFormat}`,
        ),
      );
      let obsBrowserFailure = null;
      if (
        firstObsBrowserScreenshot.sha256 === secondObsBrowserScreenshot.sha256
      ) {
        obsBrowserFailure =
          "The actual OBS Browser Source viewer did not render an advancing image";
      } else if (requireZeroFreezes && repeatedSamples !== 0) {
        obsBrowserFailure = `OBS Browser Source repeated ${repeatedSamples} one-second image sample(s) during the soak`;
      }
      // Browser Source volume metering depends on the local OBS audio-routing
      // configuration. Record it for diagnostics, but only make it a gate when
      // the caller explicitly knows that rerouted browser audio is metered.
      if (
        !obsBrowserFailure &&
        process.env.VDONINJA_REQUIRE_OBS_BROWSER_AUDIO_METER === "1" &&
        (obsBrowserAudioMeter.events === 0 ||
          obsBrowserAudioMeter.nonSilentEvents === 0)
      ) {
        obsBrowserFailure =
          `OBS Browser Source did not expose decoded non-silent audio; ` +
          `meter=${JSON.stringify(obsBrowserAudioMeter)}`;
      }
      if (recordingStarted) {
        logStep("stopping the simultaneous local OBS recording");
        localRecording = await client.request("StopRecord");
        recordingStarted = false;
        await sleep(1000);
      }
      browserStackViewerReport = await finishBrowserStackViewerCheck(
        browserStackViewerCheck,
      );

      const reportPath = path.join(
        outputDir,
        `obs-publish-report-${stamp}.json`,
      );
      const report = {
        ok: !obsBrowserFailure,
        failure: obsBrowserFailure,
        viewerRuntime: "OBS Browser Source",
        streamId,
        password,
        roomId,
        viewUrl,
        sourceMode,
        soakMs,
        obsBrowserSampleMs,
        obsBrowserScreenshotFormat,
        obsBrowserScreenshotQuality,
        obsBrowserMinimumScreenshotBytes,
        obsBrowserViewerCount: obsBrowserViewers.length,
        obsBrowserViewerBitratesKbps,
        nativeViewer,
        videoProtectionMode,
        audioRed,
        requestAudioRed,
        adaptiveBitrate,
        adaptiveBitrateMinimumKbps,
        activeStatus,
        appliedVideoSettings,
        appliedVideoBitrateParameter,
        browserSamples,
        repeatedSamples,
        obsBrowserAudioMeter,
        firstObsBrowserScreenshot,
        secondObsBrowserScreenshot,
        localRecording,
        browserStackViewerReport,
        streamEvents,
        reportPath,
      };
      fs.writeFileSync(
        reportPath,
        `${JSON.stringify(report, null, 2)}\n`,
        "utf8",
      );
      if (obsBrowserFailure) {
        throw new Error(`${obsBrowserFailure}; report=${reportPath}`);
      }
      console.log(
        JSON.stringify({
          ok: true,
          viewerRuntime: report.viewerRuntime,
          streamId,
          repeatedSamples,
          firstObsBrowserScreenshot,
          secondObsBrowserScreenshot,
          reportPath,
        }),
      );
      return;
    }

    browser = await chromium.launch({
      headless: process.env.HEADLESS === "0" ? false : true,
      args: ["--autoplay-policy=no-user-gesture-required"],
    });
    const context = await browser.newContext();
    await context.addInitScript(() => {
      window.__pcList = [];
      const NativePC = window.RTCPeerConnection;
      if (!NativePC) {
        return;
      }
      window.RTCPeerConnection = function (...args) {
        const pc = new NativePC(...args);
        window.__pcList.push(pc);
        return pc;
      };
      window.RTCPeerConnection.prototype = NativePC.prototype;
    });

    const page = await context.newPage();
    page.on("console", (message) => {
      const text = message.text();
      consoleMessages.push({ type: message.type(), text });
    });
    page.on("pageerror", (error) => {
      pageErrors.push(String(error && error.stack ? error.stack : error));
    });

    logStep(`opening browser viewer ${viewUrl}`);
    await page.goto(viewUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
    const viewport = page.viewportSize() || { width: 1280, height: 720 };
    await page.mouse.click(
      Math.floor(viewport.width / 2),
      Math.floor(viewport.height / 2),
    );

    let firstPlayable = null;
    let latestSnapshot = null;
    const deadline = Date.now() + waitMs;
    while (Date.now() < deadline) {
      latestSnapshot = await collectViewerSnapshot(page);
      const invalidSdp = consoleMessages.find((message) =>
        /Invalid SCTP max message size/i.test(message.text),
      );
      if (invalidSdp) {
        throw new Error(`Browser rejected plugin SDP: ${invalidSdp.text}`);
      }
      if (
        hasPlayableMedia(latestSnapshot) &&
        totalInboundBytes(latestSnapshot) > 5000
      ) {
        firstPlayable = latestSnapshot;
        break;
      }
      await sleep(2000);
    }

    if (!firstPlayable) {
      throw new Error(
        `Viewer did not receive playable media; latest=${JSON.stringify(latestSnapshot)}`,
      );
    }

    fs.mkdirSync(outputDir, { recursive: true });
    let decodedAudioCaptureStart = null;
    if (captureDecodedAudio) {
      logStep("capturing decoded viewer audio as PCM");
      decodedAudioCaptureStart = await startDecodedAudioCapture(page);
    }
    let firstObsBrowserScreenshot = null;
    if (createdObsBrowserViewer) {
      await sleep(3000);
      const screenshot = await client.request("GetSourceScreenshot", {
        ...(obsBrowserViewerUuid
          ? { sourceUuid: obsBrowserViewerUuid }
          : { sourceName: obsBrowserViewerName }),
        imageFormat: "png",
        imageWidth: 640,
        imageHeight: 360,
      });
      firstObsBrowserScreenshot = saveObsScreenshot(
        screenshot.imageData,
        path.join(outputDir, `obs-browser-viewer-first-${stamp}.png`),
      );
      // The screenshot and second browser initialization can briefly stall
      // rendering. Establish the continuity baseline after that deliberate
      // setup work so the soak measures steady-state delivery.
      await sleep(1000);
    }

    const continuityBaseline = await collectViewerSnapshot(page);
    const samples = [continuityBaseline];
    const soakDeadline = Date.now() + soakMs;
    while (Date.now() < soakDeadline) {
      await sleep(Math.min(1000, Math.max(1, soakDeadline - Date.now())));
      samples.push(await collectViewerSnapshot(page));
    }
    browserStackViewerReport = await finishBrowserStackViewerCheck(
      browserStackViewerCheck,
    );
    if (browserStackViewerReport) {
      samples.push(await collectViewerSnapshot(page));
    }
    const secondPlayable = samples[samples.length - 1];
    let decodedAudioCapture = null;
    if (captureDecodedAudio) {
      const captured = await stopDecodedAudioCapture(page);
      const pcm = Buffer.from(captured.pcmBase64, "base64");
      const wavPath = path.join(
        outputDir,
        `obs-publish-decoded-audio-${stamp}.wav`,
      );
      fs.writeFileSync(wavPath, createPcm16Wav(pcm, captured.sampleRate));
      let rawTrack = null;
      if (captured.rawTrack && captured.rawTrack.sampleCount > 0) {
        const rawPcm = Buffer.from(captured.rawTrack.pcmBase64, "base64");
        const rawWavPath = path.join(
          outputDir,
          `obs-publish-raw-track-audio-${stamp}.wav`,
        );
        fs.writeFileSync(
          rawWavPath,
          createPcm16Wav(rawPcm, captured.rawTrack.sampleRate),
        );
        rawTrack = {
          sampleRate: captured.rawTrack.sampleRate,
          sampleCount: captured.rawTrack.sampleCount,
          durationSeconds:
            captured.rawTrack.sampleCount / captured.rawTrack.sampleRate,
          firstTimestampUs: captured.rawTrack.firstTimestamp,
          lastTimestampUs: captured.rawTrack.lastTimestamp,
          lastDurationUs: captured.rawTrack.lastDuration,
          maxTimestampStepUs: captured.rawTrack.maxTimestampStep,
          nonForwardTimestamps: captured.rawTrack.nonForwardTimestamps,
          timestampGaps: captured.rawTrack.timestampGaps,
          error: captured.rawTrack.error,
          wavPath: rawWavPath,
          analysis: analyzePcm16Le(rawPcm, {
            sampleRate: captured.rawTrack.sampleRate,
            toneHz: 997,
          }),
        };
      }
      decodedAudioCapture = {
        ...decodedAudioCaptureStart,
        sampleRate: captured.sampleRate,
        sampleCount: captured.sampleCount,
        durationSeconds: captured.sampleCount / captured.sampleRate,
        wavPath,
        continuityBasis: rawTrack
          ? "raw decoded MediaStreamTrack"
          : "Web Audio fallback",
        rawTrack,
        webAudioPlayoutAnalysis: analyzePcm16Le(pcm, {
          sampleRate: captured.sampleRate,
          toneHz: 997,
        }),
      };
    }
    if (recordingStarted) {
      logStep("stopping the simultaneous local OBS recording");
      localRecording = await client.request("StopRecord");
      recordingStarted = false;
      await sleep(1000);
    }
    if (!playbackAdvanced(continuityBaseline, secondPlayable)) {
      throw new Error("Viewer media did not advance after initial playback");
    }
    const newFreezes =
      totalPcMetric(secondPlayable, "freezeCount") -
      totalPcMetric(continuityBaseline, "freezeCount");
    let videoContinuityFailure = null;
    if (requireZeroFreezes && newFreezes !== 0) {
      videoContinuityFailure = `Chrome recorded ${newFreezes} new video freeze(s) during the ${soakMs} ms soak`;
    }
    const newConcealedSamples =
      totalPcMetric(secondPlayable, "concealedSamples") -
      totalPcMetric(continuityBaseline, "concealedSamples");
    const newConcealmentEvents =
      totalPcMetric(secondPlayable, "concealmentEvents") -
      totalPcMetric(continuityBaseline, "concealmentEvents");
    const newSilentConcealedSamples =
      totalPcMetric(secondPlayable, "silentConcealedSamples") -
      totalPcMetric(continuityBaseline, "silentConcealedSamples");
    const newInsertedSamplesForDeceleration =
      totalPcMetric(secondPlayable, "insertedSamplesForDeceleration") -
      totalPcMetric(continuityBaseline, "insertedSamplesForDeceleration");
    const newRemovedSamplesForAcceleration =
      totalPcMetric(secondPlayable, "removedSamplesForAcceleration") -
      totalPcMetric(continuityBaseline, "removedSamplesForAcceleration");
    const newPacketsDiscarded =
      totalPcMetric(secondPlayable, "packetsDiscarded") -
      totalPcMetric(continuityBaseline, "packetsDiscarded");
    const newPacketsLost =
      totalPcMetric(secondPlayable, "packetsLost") -
      totalPcMetric(continuityBaseline, "packetsLost");
    let audioContinuityFailure = null;
    if (requireAudioContinuity) {
      if (newPacketsLost > 0 || newPacketsDiscarded > 0) {
        audioContinuityFailure =
          `Chrome recorded ${newPacketsLost} newly lost packet(s) and ` +
          `${newPacketsDiscarded} newly discarded packet(s)`;
      } else if (newConcealedSamples > 0 || newConcealmentEvents > 0) {
        audioContinuityFailure =
          `Chrome recorded ${newConcealedSamples} newly concealed audio samples in ` +
          `${newConcealmentEvents} event(s)`;
      } else if (!decodedAudioCapture) {
        audioContinuityFailure = "decoded PCM capture did not produce a result";
      } else if (
        decodedAudioCapture.rawTrack &&
        decodedAudioCapture.rawTrack.error
      ) {
        audioContinuityFailure = `decoded-track capture failed: ${decodedAudioCapture.rawTrack.error}`;
      } else {
        const authoritativeAnalysis = decodedAudioCapture.rawTrack
          ? decodedAudioCapture.rawTrack.analysis
          : decodedAudioCapture.webAudioPlayoutAnalysis;
        if (!authoritativeAnalysis.ok) {
          audioContinuityFailure =
            `decoded PCM failed continuity analysis: ` +
            `${JSON.stringify(authoritativeAnalysis)}`;
        } else if (
          decodedAudioCapture.rawTrack &&
          decodedAudioCapture.rawTrack.nonForwardTimestamps !== 0
        ) {
          audioContinuityFailure =
            `decoded track recorded ${decodedAudioCapture.rawTrack.nonForwardTimestamps} ` +
            `non-forward timestamp(s)`;
        }
      }
    }

    let secondObsBrowserScreenshot = null;
    if (createdObsBrowserViewer) {
      const screenshot = await client.request("GetSourceScreenshot", {
        ...(obsBrowserViewerUuid
          ? { sourceUuid: obsBrowserViewerUuid }
          : { sourceName: obsBrowserViewerName }),
        imageFormat: "png",
        imageWidth: 640,
        imageHeight: 360,
      });
      secondObsBrowserScreenshot = saveObsScreenshot(
        screenshot.imageData,
        path.join(outputDir, `obs-browser-viewer-final-${stamp}.png`),
      );
      if (
        firstObsBrowserScreenshot.sha256 === secondObsBrowserScreenshot.sha256
      ) {
        throw new Error(
          "The actual OBS Browser Source viewer did not render an advancing image",
        );
      }
    }

    const streamStatusAfterViewer = await client
      .request("GetStreamStatus")
      .catch((error) => ({ error: String(error) }));
    const screenshotPath = path.join(
      outputDir,
      `obs-publish-viewer-${stamp}.png`,
    );
    const reportPath = path.join(outputDir, `obs-publish-report-${stamp}.json`);
    await page.screenshot({ path: screenshotPath, fullPage: true });

    const report = {
      ok: !audioContinuityFailure && !videoContinuityFailure,
      streamId,
      password,
      roomId,
      viewUrl,
      sourceMode,
      soakMs,
      obsBrowserViewerCount: obsBrowserViewers.length,
      obsBrowserViewerBitratesKbps,
      nativeViewer,
      videoProtectionMode,
      audioRed,
      requestAudioRed,
      adaptiveBitrate,
      adaptiveBitrateMinimumKbps,
      appliedVideoSettings,
      appliedVideoBitrateParameter,
      sourceTonePath,
      sourceToneDurationSeconds,
      obsVersion: version.obsVersion,
      obsWebSocketVersion: version.obsWebSocketVersion,
      activeStatus,
      streamStatusAfterViewer,
      firstPlayable,
      continuityBaseline,
      secondPlayable,
      samples,
      newFreezes,
      videoContinuityFailure,
      newConcealedSamples,
      newSilentConcealedSamples,
      newConcealmentEvents,
      newInsertedSamplesForDeceleration,
      newRemovedSamplesForAcceleration,
      newPacketsDiscarded,
      newPacketsLost,
      decodedAudioCapture,
      localRecording,
      browserStackViewerReport,
      audioContinuityFailure,
      firstObsBrowserScreenshot,
      secondObsBrowserScreenshot,
      consoleMessages: compactConsoleMessages(consoleMessages),
      pageErrors: pageErrors
        .slice(-10)
        .map((error) =>
          error.length > 500 ? `${error.slice(0, 500)}...(truncated)` : error,
        ),
      streamEvents,
      screenshotPath,
      reportPath,
    };
    fs.writeFileSync(
      reportPath,
      `${JSON.stringify(report, null, 2)}\n`,
      "utf8",
    );
    if (videoContinuityFailure) {
      throw new Error(videoContinuityFailure);
    }
    if (audioContinuityFailure) {
      throw new Error(audioContinuityFailure);
    }
    console.log(
      JSON.stringify({
        ok: true,
        streamId,
        password,
        roomId,
        viewUrl,
        inboundBytes: totalInboundBytes(secondPlayable),
        framesReceived: totalPcMetric(secondPlayable, "framesReceived"),
        framesDropped: totalPcMetric(secondPlayable, "framesDropped"),
        freezeCount: totalPcMetric(secondPlayable, "freezeCount"),
        totalFreezesDuration: totalPcMetric(
          secondPlayable,
          "totalFreezesDuration",
        ),
        packetsLost: totalPcMetric(secondPlayable, "packetsLost"),
        nackCount: totalPcMetric(secondPlayable, "nackCount"),
        pliCount: totalPcMetric(secondPlayable, "pliCount"),
        keyFramesDecoded: totalPcMetric(secondPlayable, "keyFramesDecoded"),
        concealedSamples: totalPcMetric(secondPlayable, "concealedSamples"),
        silentConcealedSamples: totalPcMetric(
          secondPlayable,
          "silentConcealedSamples",
        ),
        concealmentEvents: totalPcMetric(secondPlayable, "concealmentEvents"),
        newConcealedSamples,
        newSilentConcealedSamples,
        newConcealmentEvents,
        newInsertedSamplesForDeceleration,
        newRemovedSamplesForAcceleration,
        newPacketsDiscarded,
        newPacketsLost,
        decodedAudioCapture,
        localRecording,
        currentTime: secondPlayable.videos[0]
          ? secondPlayable.videos[0].currentTime
          : null,
        screenshotPath,
        reportPath,
        sourceTonePath,
      }),
    );

    await context.close();
  } finally {
    if (
      browserStackViewerCheck &&
      !browserStackViewerCheck.finished &&
      browserStackViewerCheck.process
    ) {
      browserStackViewerCheck.process.kill();
    }
    if (browser) {
      await browser.close().catch(() => {});
    }
    if (client.socket && client.socket.readyState === WebSocket.OPEN) {
      try {
        const status = await client
          .request("GetStreamStatus")
          .catch(() => null);
        if (status && status.outputActive) {
          logStep("stopping OBS stream");
          await client.request("StopStream").catch(() => {});
          await sleep(3000);
        }
        if (recordingStarted) {
          logStep("stopping local OBS recording");
          await client.request("StopRecord").catch(() => {});
          recordingStarted = false;
        }
        if (createdObsBrowserViewer) {
          for (const viewer of [...obsBrowserViewers].reverse()) {
            logStep(`removing input ${viewer.name}`);
            await client
              .request(
                "RemoveInput",
                viewer.uuid
                  ? { inputUuid: viewer.uuid }
                  : { inputName: viewer.name },
              )
              .catch(() => {});
          }
        }
        if (nativeViewer) {
          logStep(`removing input ${nativeViewer.name}`);
          await client
            .request(
              "RemoveInput",
              nativeViewer.uuid
                ? { inputUuid: nativeViewer.uuid }
                : { inputName: nativeViewer.name },
            )
            .catch(() => {});
        }
        if (useAudioContinuitySource) {
          logStep(`removing input ${audioInputName}`);
          await client
            .request("RemoveInput", { inputName: audioInputName })
            .catch(() => {});
        }
        logStep(`removing input ${inputName}`);
        await client.request("RemoveInput", { inputName }).catch(() => {});
        if (createdScene && previousSceneName) {
          await client
            .request("SetCurrentProgramScene", { sceneName: previousSceneName })
            .catch(() => {});
        }
        if (createdScene) {
          logStep(`removing scene ${sceneName}`);
          await client.request("RemoveScene", { sceneName }).catch(() => {});
        }
        if (originalVideoSettings) {
          logStep("restoring OBS video settings");
          await client
            .request("SetVideoSettings", originalVideoSettings)
            .catch(() => {});
          originalVideoSettings = null;
        }
        if (originalVideoBitrateParameter) {
          logStep("restoring OBS video bitrate");
          await client
            .request("SetProfileParameter", {
              parameterCategory: "SimpleOutput",
              parameterName: "VBitrate",
              parameterValue:
                originalVideoBitrateParameter.parameterValue === undefined
                  ? null
                  : originalVideoBitrateParameter.parameterValue,
            })
            .catch(() => {});
          originalVideoBitrateParameter = null;
        }
      } finally {
        await client.close();
      }
    }
  }
}

main().catch((error) => {
  console.error(error.stack || String(error));
  process.exit(1);
});
