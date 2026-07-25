const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const { chromium } = require("playwright");

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
    this.requestTimeoutMs = Number(process.env.OBS_WEBSOCKET_REQUEST_TIMEOUT_MS || 20000);
  }

  async connect() {
    const deadline = Date.now() + Number(process.env.OBS_WEBSOCKET_CONNECT_TIMEOUT_MS || 60000);
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

    throw new Error(`Timed out connecting to obs-websocket at ${this.url}: ${lastError || "unknown error"}`);
  }

  async connectOnce() {
    await new Promise((resolve, reject) => {
      const socket = new WebSocket(this.url, "obswebsocket.json");
      this.socket = socket;

      socket.addEventListener("open", () => resolve());
      socket.addEventListener("error", (error) => reject(new Error(error.message || "WebSocket error")));
      socket.addEventListener("message", (event) => {
        try {
          const message = JSON.parse(event.data.toString());
          if (message.op === 0) {
            socket.send(
              JSON.stringify({
                op: 1,
                d: {
                  rpcVersion: 1,
                  eventSubscriptions: this.eventSubscriptions
                }
              })
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

          const comment = (message.d.requestStatus && message.d.requestStatus.comment) || "OBS request failed";
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
        }
      });
    });

    this.socket.send(
      JSON.stringify({
        op: 6,
        d: {
          requestType,
          requestId,
          requestData
        }
      })
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

function compactConsoleMessages(messages) {
  return messages.slice(-20).map((message) => ({
    type: message.type,
    text: message.text.length > 500 ? `${message.text.slice(0, 500)}...(truncated)` : message.text
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
    const videos = Array.from(document.querySelectorAll("video")).map((v, index) => {
      const stream = v.srcObject;
      const audioTracks = stream && stream.getAudioTracks ? stream.getAudioTracks().length : 0;
      const videoTracks = stream && stream.getVideoTracks ? stream.getVideoTracks().length : 0;
      return {
        index,
        readyState: v.readyState,
        paused: v.paused,
        currentTime: v.currentTime,
        videoWidth: v.videoWidth,
        videoHeight: v.videoHeight,
        audioTracks,
        videoTracks
      };
    });

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
          let concealmentEvents = 0;
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
                concealmentEvents += s.concealmentEvents || 0;
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
            concealmentEvents
          });
        } catch (error) {
          pcStats.push({ error: String(error) });
        }
      }
    }

    return {
      url: location.href,
      title: document.title || "",
      textSample: (document.body ? document.body.innerText || "" : "").slice(0, 300),
      videos,
      pcStats,
      timestamp: Date.now()
    };
  });
}

function hasPlayableMedia(snapshot) {
  return snapshot.videos.some((video) => video.videoTracks > 0 && video.videoWidth > 0 && video.videoHeight > 0);
}

function totalInboundBytes(snapshot) {
  return snapshot.pcStats.reduce(
    (total, stat) => total + (stat.inboundVideoBytes || 0) + (stat.inboundAudioBytes || 0),
    0
  );
}

function playbackAdvanced(before, after) {
  return after.videos.some((v2) => {
    const v1 = before.videos.find((candidate) => candidate.index === v2.index);
    return v1 && v2.currentTime > v1.currentTime + 0.4;
  });
}

function totalPcMetric(snapshot, key) {
  return snapshot.pcStats.reduce((total, stat) => total + (Number(stat[key]) || 0), 0);
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
    sha256: crypto.createHash("sha256").update(bytes).digest("hex")
  };
}

async function waitForStreamActive(client, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let lastStatus = null;
  while (Date.now() < deadline) {
    lastStatus = await client.request("GetStreamStatus").catch((error) => ({ error: String(error) }));
    if (lastStatus && lastStatus.outputActive) {
      return lastStatus;
    }
    await sleep(1000);
  }
  throw new Error(`OBS stream did not become active; last status=${JSON.stringify(lastStatus)}`);
}

async function main() {
  const streamId = process.env.VDONINJA_STREAM_ID || process.argv[2] || `codexObs${Date.now()}`;
  const password = process.env.VDONINJA_PASSWORD || process.argv[3] || "false";
  const roomId = process.env.VDONINJA_ROOM_ID || process.argv[4] || "";
  const websocketUrl = process.env.OBS_WEBSOCKET_URL || "ws://127.0.0.1:4455";
  const waitMs = Number(process.env.VDONINJA_WAIT_MS || 90000);
  const soakMs = Number(process.env.VDONINJA_SOAK_MS || 7000);
  const sourceMode = String(process.env.VDONINJA_SOURCE_MODE || "static").toLowerCase();
  const useMotionSource = sourceMode === "motion";
  const useObsBrowserViewer = process.env.VDONINJA_OBS_BROWSER_VIEWER === "1";
  const requireZeroFreezes = process.env.VDONINJA_REQUIRE_ZERO_FREEZES === "1";
  const outputDir = path.resolve(process.cwd(), "artifacts");
  const stamp = Date.now();
  const sceneName = `Codex OBS Publish ${stamp}`;
  const inputName = `Codex ${useMotionSource ? "Motion" : "Color"} Program ${stamp}`;
  const obsBrowserViewerName = `Codex OBS Browser Viewer ${stamp}`;
  const motionSourcePath = path.resolve(process.cwd(), "tests", "tools", "publish-motion-source.html");
  const viewParams = new URLSearchParams();
  viewParams.set("view", streamId);
  if (roomId) {
    viewParams.set("room", roomId);
    viewParams.set("solo", "");
  }
  if (password) {
    viewParams.set("password", password);
  }
  viewParams.set("debug", "");
  const viewUrl = ensureQuery(`https://vdo.ninja/?${viewParams.toString()}`, "cleanoutput", "1");
  const consoleMessages = [];
  const pageErrors = [];
  const streamEvents = [];
  let previousSceneName = null;
  let createdScene = false;
  let createdObsBrowserViewer = false;
  let obsBrowserViewerUuid = null;
  let browser = null;

  const client = new ObsWebSocketClient(websocketUrl, {
    eventSubscriptions: EVENT_SUBSCRIPTION_OUTPUTS,
    onEvent(event) {
      if (event.eventType && /Stream|Output/i.test(event.eventType)) {
        streamEvents.push(event);
      }
    }
  });

  try {
    logStep(`connecting to ${websocketUrl}`);
    await client.connect();

    logStep("querying OBS/plugin capabilities");
    const version = await client.request("GetVersion");
    const kinds = await client.request("GetInputKindList", {
      unversioned: false
    });
    const inputKinds = Array.isArray(kinds.inputKinds) ? kinds.inputKinds : [];
    if (!inputKinds.includes("vdoninja_source")) {
      throw new Error("OBS does not have the vdoninja_source input kind registered");
    }
    const colorKind = selectColorSourceKind(inputKinds);
    if (!useMotionSource && !colorKind) {
      throw new Error("OBS does not expose a color source input kind");
    }
    if ((useMotionSource || useObsBrowserViewer) && !inputKinds.includes("browser_source")) {
      throw new Error("OBS does not expose the browser_source input kind");
    }
    if (useMotionSource && !fs.existsSync(motionSourcePath)) {
      throw new Error(`Motion source file is missing: ${motionSourcePath}`);
    }

    const currentProgram = await client.request("GetCurrentProgramScene").catch(() => ({}));
    previousSceneName = currentProgram.currentProgramSceneName || null;

    logStep(`creating scene ${sceneName}`);
    await client.request("CreateScene", { sceneName });
    createdScene = true;
    if (useMotionSource) {
      await client.request("CreateInput", {
        sceneName,
        inputName,
        inputKind: "browser_source",
        inputSettings: {
          is_local_file: true,
          local_file: motionSourcePath,
          width: 1920,
          height: 1080,
          fps: 60,
          reroute_audio: true,
          shutdown: false,
          restart_when_active: false
        },
        sceneItemEnabled: true
      });
    } else {
      await client.request("CreateInput", {
        sceneName,
        inputName,
        inputKind: colorKind,
        inputSettings: {
          color: 4278233600,
          width: 1280,
          height: 720
        },
        sceneItemEnabled: true
      });
    }
    await client.request("SetCurrentProgramScene", { sceneName });

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
        auto_reconnect: true
      }
    });

    logStep("starting OBS stream");
    await client.request("StartStream");
    const activeStatus = await waitForStreamActive(client, 30000);
    if (useObsBrowserViewer) {
      logStep(`creating actual OBS Browser Source viewer ${obsBrowserViewerName}`);
      const createdViewer = await client.request("CreateInput", {
        sceneName,
        inputName: obsBrowserViewerName,
        inputKind: "browser_source",
        inputSettings: {
          is_local_file: false,
          url: ensureQuery(viewUrl, "autostart", "1"),
          width: 640,
          height: 360,
          fps: 60,
          shutdown: false,
          restart_when_active: false
        },
        sceneItemEnabled: true
      });
      createdObsBrowserViewer = true;
      obsBrowserViewerUuid = createdViewer.inputUuid || null;
    }

    browser = await chromium.launch({
      headless: process.env.HEADLESS === "0" ? false : true,
      args: ["--autoplay-policy=no-user-gesture-required"]
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
    await page.mouse.click(Math.floor(viewport.width / 2), Math.floor(viewport.height / 2));

    let firstPlayable = null;
    let latestSnapshot = null;
    const deadline = Date.now() + waitMs;
    while (Date.now() < deadline) {
      latestSnapshot = await collectViewerSnapshot(page);
      const invalidSdp = consoleMessages.find((message) => /Invalid SCTP max message size/i.test(message.text));
      if (invalidSdp) {
        throw new Error(`Browser rejected plugin SDP: ${invalidSdp.text}`);
      }
      if (hasPlayableMedia(latestSnapshot) && totalInboundBytes(latestSnapshot) > 5000) {
        firstPlayable = latestSnapshot;
        break;
      }
      await sleep(2000);
    }

    if (!firstPlayable) {
      throw new Error(`Viewer did not receive playable media; latest=${JSON.stringify(latestSnapshot)}`);
    }

    fs.mkdirSync(outputDir, { recursive: true });
    let firstObsBrowserScreenshot = null;
    if (createdObsBrowserViewer) {
      await sleep(3000);
      const screenshot = await client.request("GetSourceScreenshot", {
        ...(obsBrowserViewerUuid ? { sourceUuid: obsBrowserViewerUuid } : { sourceName: obsBrowserViewerName }),
        imageFormat: "png",
        imageWidth: 640,
        imageHeight: 360
      });
      firstObsBrowserScreenshot = saveObsScreenshot(
        screenshot.imageData,
        path.join(outputDir, `obs-browser-viewer-first-${stamp}.png`)
      );
    }

    const samples = [firstPlayable];
    const soakDeadline = Date.now() + soakMs;
    while (Date.now() < soakDeadline) {
      await sleep(Math.min(1000, Math.max(1, soakDeadline - Date.now())));
      samples.push(await collectViewerSnapshot(page));
    }
    const secondPlayable = samples[samples.length - 1];
    if (!playbackAdvanced(firstPlayable, secondPlayable)) {
      throw new Error("Viewer media did not advance after initial playback");
    }
    const newFreezes = totalPcMetric(secondPlayable, "freezeCount") - totalPcMetric(firstPlayable, "freezeCount");
    if (requireZeroFreezes && newFreezes !== 0) {
      throw new Error(`Chrome recorded ${newFreezes} new video freeze(s) during the ${soakMs} ms soak`);
    }

    let secondObsBrowserScreenshot = null;
    if (createdObsBrowserViewer) {
      const screenshot = await client.request("GetSourceScreenshot", {
        ...(obsBrowserViewerUuid ? { sourceUuid: obsBrowserViewerUuid } : { sourceName: obsBrowserViewerName }),
        imageFormat: "png",
        imageWidth: 640,
        imageHeight: 360
      });
      secondObsBrowserScreenshot = saveObsScreenshot(
        screenshot.imageData,
        path.join(outputDir, `obs-browser-viewer-final-${stamp}.png`)
      );
      if (firstObsBrowserScreenshot.sha256 === secondObsBrowserScreenshot.sha256) {
        throw new Error("The actual OBS Browser Source viewer did not render an advancing image");
      }
    }

    const streamStatusAfterViewer = await client
      .request("GetStreamStatus")
      .catch((error) => ({ error: String(error) }));
    const screenshotPath = path.join(outputDir, `obs-publish-viewer-${stamp}.png`);
    const reportPath = path.join(outputDir, `obs-publish-report-${stamp}.json`);
    await page.screenshot({ path: screenshotPath, fullPage: true });

    const report = {
      ok: true,
      streamId,
      password,
      roomId,
      viewUrl,
      sourceMode,
      soakMs,
      obsVersion: version.obsVersion,
      obsWebSocketVersion: version.obsWebSocketVersion,
      activeStatus,
      streamStatusAfterViewer,
      firstPlayable,
      secondPlayable,
      samples,
      newFreezes,
      firstObsBrowserScreenshot,
      secondObsBrowserScreenshot,
      consoleMessages: compactConsoleMessages(consoleMessages),
      pageErrors: pageErrors
        .slice(-10)
        .map((error) => (error.length > 500 ? `${error.slice(0, 500)}...(truncated)` : error)),
      streamEvents,
      screenshotPath,
      reportPath
    };
    fs.writeFileSync(reportPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
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
        totalFreezesDuration: totalPcMetric(secondPlayable, "totalFreezesDuration"),
        packetsLost: totalPcMetric(secondPlayable, "packetsLost"),
        nackCount: totalPcMetric(secondPlayable, "nackCount"),
        pliCount: totalPcMetric(secondPlayable, "pliCount"),
        keyFramesDecoded: totalPcMetric(secondPlayable, "keyFramesDecoded"),
        currentTime: secondPlayable.videos[0] ? secondPlayable.videos[0].currentTime : null,
        screenshotPath,
        reportPath
      })
    );

    await context.close();
  } finally {
    if (browser) {
      await browser.close().catch(() => {});
    }
    if (client.socket && client.socket.readyState === WebSocket.OPEN) {
      try {
        const status = await client.request("GetStreamStatus").catch(() => null);
        if (status && status.outputActive) {
          logStep("stopping OBS stream");
          await client.request("StopStream").catch(() => {});
          await sleep(3000);
        }
        if (createdObsBrowserViewer) {
          logStep(`removing input ${obsBrowserViewerName}`);
          await client
            .request(
              "RemoveInput",
              obsBrowserViewerUuid ? { inputUuid: obsBrowserViewerUuid } : { inputName: obsBrowserViewerName }
            )
            .catch(() => {});
        }
        logStep(`removing input ${inputName}`);
        await client.request("RemoveInput", { inputName }).catch(() => {});
        if (createdScene && previousSceneName) {
          await client.request("SetCurrentProgramScene", { sceneName: previousSceneName }).catch(() => {});
        }
        if (createdScene) {
          logStep(`removing scene ${sceneName}`);
          await client.request("RemoveScene", { sceneName }).catch(() => {});
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
