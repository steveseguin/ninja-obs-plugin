const fs = require("fs");
const path = require("path");
const { chromium, _android } = require("playwright");
const playwrightPackage = require("@playwright/test/package.json");
const {
  analyzePresentationContinuity,
  analyzeVisualSequence,
} = require("../tests/tools/presentation-continuity-analysis.cjs");

const PROFILES = {
  "win-chrome": {
    label: "Windows 11 Chrome latest",
    caps: {
      os: "Windows",
      os_version: "11",
      browser: "chrome",
      browser_version: "latest",
    },
  },
  "win-edge": {
    label: "Windows 11 Edge latest",
    caps: {
      os: "Windows",
      os_version: "11",
      browser: "edge",
      browser_version: "latest",
    },
  },
  "win-firefox": {
    label: "Windows 11 Playwright Firefox",
    caps: {
      os: "Windows",
      os_version: "11",
      browser: "playwright-firefox",
    },
  },
  "mac-chrome": {
    label: "macOS Chrome latest",
    caps: {
      os: "OS X",
      os_version: "Sequoia",
      browser: "chrome",
      browser_version: "latest",
    },
  },
  "mac-webkit": {
    label: "macOS Playwright WebKit",
    caps: {
      os: "OS X",
      os_version: "Sequoia",
      browser: "playwright-webkit",
    },
  },
  "android-pixel9-chrome": {
    label: "Google Pixel 9 Chrome",
    android: true,
    caps: {
      browser: "chrome",
      osVersion: "16.0",
      deviceName: "Google Pixel 9",
      realMobile: "true",
    },
  },
  "android-s25-chrome": {
    label: "Samsung Galaxy S25 Ultra Chrome",
    android: true,
    caps: {
      browser: "chrome",
      osVersion: "15.0",
      deviceName: "Samsung Galaxy S25 Ultra",
      realMobile: "true",
    },
  },
  "ios-iphone15-safari": {
    label: "iPhone 15 Pro Max Safari",
    ios: true,
    caps: {
      browser: "safari",
      osVersion: "17",
      deviceName: "iPhone 15 Pro Max",
      realMobile: "true",
    },
  },
};

function parseArgs(argv) {
  const args = {};
  for (let index = 0; index < argv.length; index += 1) {
    const value = argv[index];
    if (!value.startsWith("--")) {
      continue;
    }
    const equals = value.indexOf("=");
    if (equals !== -1) {
      args[value.slice(2, equals)] = value.slice(equals + 1);
      continue;
    }
    const name = value.slice(2);
    if (index + 1 < argv.length && !argv[index + 1].startsWith("--")) {
      args[name] = argv[index + 1];
      index += 1;
    } else {
      args[name] = "1";
    }
  }
  return args;
}

function asPositiveInteger(value, fallback) {
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed > 0 ? Math.floor(parsed) : fallback;
}

function asNonNegativeNumber(value, fallback = 0) {
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : fallback;
}

function isEnabled(value) {
  if (value === true || value === 1) {
    return true;
  }
  return ["1", "true", "yes", "on"].includes(String(value).toLowerCase());
}

function parseCustomNetwork(value) {
  const parts = String(value || "")
    .split(",")
    .map((part) => Number(part.trim()));
  if (parts.length !== 4 || parts.some((part) => !Number.isFinite(part))) {
    return null;
  }
  return {
    downloadKbps: parts[0],
    uploadKbps: parts[1],
    latencyMs: parts[2],
    packetLossPercent: parts[3],
  };
}

function redactViewerUrl(value) {
  const url = new URL(value);
  for (const name of ["password", "pass", "pw", "hash", "salt"]) {
    if (url.searchParams.has(name)) {
      url.searchParams.set(name, "<redacted>");
    }
  }
  return url.toString();
}

function sanitizeSessionDetails(details) {
  const allowed = [
    "name",
    "duration",
    "os",
    "os_version",
    "browser_version",
    "browser",
    "device",
    "status",
    "hashed_id",
    "reason",
    "build_name",
    "project_name",
    "build_hashed_id",
    "browserstack_status",
    "created_at",
  ];
  return Object.fromEntries(
    allowed
      .filter((name) => Object.prototype.hasOwnProperty.call(details, name))
      .map((name) => [name, details[name]]),
  );
}

function loadSecrets(secretFile) {
  if (
    process.env.BROWSERSTACK_USERNAME &&
    process.env.BROWSERSTACK_ACCESS_KEY
  ) {
    return;
  }
  if (!secretFile) {
    throw new Error(
      "Set BROWSERSTACK_USERNAME and BROWSERSTACK_ACCESS_KEY, or pass --secret-file",
    );
  }
  const lines = fs
    .readFileSync(path.resolve(secretFile), "utf8")
    .split(/\r?\n/);
  for (const line of lines) {
    if (!/^\s*[^#][^=]+=/.test(line)) {
      continue;
    }
    const equals = line.indexOf("=");
    const name = line.slice(0, equals).trim();
    if (
      name === "BROWSERSTACK_USERNAME" ||
      name === "BROWSERSTACK_ACCESS_KEY"
    ) {
      process.env[name] = line.slice(equals + 1).trim();
    }
  }
  if (
    !process.env.BROWSERSTACK_USERNAME ||
    !process.env.BROWSERSTACK_ACCESS_KEY
  ) {
    throw new Error("BrowserStack credentials were not found");
  }
}

function parsePhases(raw) {
  if (!raw) {
    return [
      {
        name: "baseline",
        customNetwork: "20000,10000,80,0",
        durationMs: 15000,
        expectAdvance: true,
      },
    ];
  }
  const parsed = JSON.parse(raw);
  if (!Array.isArray(parsed) || parsed.length === 0) {
    throw new Error("--phases must be a non-empty JSON array");
  }
  return parsed.map((phase, index) => {
    if (!phase || typeof phase !== "object") {
      throw new Error(`Network phase ${index + 1} must be an object`);
    }
    if (!phase.customNetwork && !phase.networkProfile) {
      throw new Error(
        `Network phase ${index + 1} needs customNetwork or networkProfile`,
      );
    }
    return {
      name: String(phase.name || `phase-${index + 1}`),
      ...(phase.customNetwork
        ? { customNetwork: String(phase.customNetwork) }
        : { networkProfile: String(phase.networkProfile) }),
      durationMs: asPositiveInteger(phase.durationMs, 15000),
      expectAdvance: phase.expectAdvance !== false,
      requireNetworkEffect: isEnabled(phase.requireNetworkEffect),
      expectedMediaKbps: asNonNegativeNumber(phase.expectedMediaKbps),
    };
  });
}

function browserStackWebSocket(caps) {
  return (
    "wss://cdp.browserstack.com/playwright?caps=" +
    encodeURIComponent(JSON.stringify(caps))
  );
}

async function webDriverRequest(method, pathname, body) {
  const response = await fetch(
    `https://hub-cloud.browserstack.com${pathname}`,
    {
      method,
      headers: {
        Authorization:
          "Basic " +
          Buffer.from(
            `${process.env.BROWSERSTACK_USERNAME}:${process.env.BROWSERSTACK_ACCESS_KEY}`,
          ).toString("base64"),
        "Content-Type": "application/json; charset=utf-8",
      },
      ...(body === undefined ? {} : { body: JSON.stringify(body) }),
    },
  );
  const text = await response.text();
  let parsed = {};
  try {
    parsed = text ? JSON.parse(text) : {};
  } catch (_) {
    throw new Error(
      `BrowserStack WebDriver returned invalid JSON (HTTP ${response.status})`,
    );
  }
  if (!response.ok || (parsed.value && parsed.value.error)) {
    const message =
      parsed.value && parsed.value.message ? parsed.value.message : text;
    throw new Error(
      `BrowserStack WebDriver HTTP ${response.status}: ${String(message).slice(
        0,
        1000,
      )}`,
    );
  }
  return parsed.value === undefined ? parsed : parsed.value;
}

class SafariWebDriverPage {
  constructor(sessionId, sessionDetails) {
    this.sessionId = sessionId;
    this.browserStackSessionDetails = sessionDetails;
    this.mouse = {
      click: async (x, y) => {
        await webDriverRequest(
          "POST",
          `/wd/hub/session/${this.sessionId}/actions`,
          {
            actions: [
              {
                type: "pointer",
                id: "finger",
                parameters: { pointerType: "touch" },
                actions: [
                  {
                    type: "pointerMove",
                    duration: 0,
                    x,
                    y,
                    origin: "viewport",
                  },
                  { type: "pointerDown", button: 0 },
                  { type: "pointerUp", button: 0 },
                ],
              },
            ],
          },
        );
      },
    };
  }

  async goto(url) {
    return webDriverRequest("POST", `/wd/hub/session/${this.sessionId}/url`, {
      url,
    });
  }

  async evaluate(callback, ...args) {
    const result = await webDriverRequest(
      "POST",
      `/wd/hub/session/${this.sessionId}/execute/async`,
      {
        script:
          "var done = arguments[arguments.length - 1];" +
          "var values = Array.prototype.slice.call(arguments, 0, -1);" +
          `Promise.resolve((${callback.toString()}).apply(null, values)).then(` +
          "function (value) { done({ok:true,value:value}); }," +
          "function (error) { done({ok:false,error:error && error.message ? error.message : String(error)}); });",
        args,
      },
    );
    if (!result || !result.ok) {
      throw new Error(
        result && result.error
          ? result.error
          : "BrowserStack asynchronous script failed",
      );
    }
    return result.value;
  }

  waitForTimeout(timeoutMs) {
    return new Promise((resolve) => setTimeout(resolve, timeoutMs));
  }

  viewportSize() {
    return { width: 430, height: 932 };
  }

  on() {}

  async screenshot(options) {
    const encoded = await webDriverRequest(
      "GET",
      `/wd/hub/session/${this.sessionId}/screenshot`,
    );
    fs.writeFileSync(options.path, Buffer.from(encoded, "base64"));
  }
}

async function connectSafariWebDriver(profile, caps) {
  const browserStackOptions = {
    userName: process.env.BROWSERSTACK_USERNAME,
    accessKey: process.env.BROWSERSTACK_ACCESS_KEY,
    osVersion: process.env.VDONINJA_SAFARI_OS_VERSION || "26",
    deviceName:
      process.env.VDONINJA_SAFARI_DEVICE_NAME || profile.caps.deviceName,
    realMobile: true,
    local: false,
    projectName: caps.project,
    buildName: caps.build,
    sessionName: caps.name,
    networkLogs: true,
    acceptInsecureCerts: true,
  };
  // BrowserStack's native iOS WebDriver endpoint rejects some otherwise valid
  // customNetwork tuples during session creation. Apply every requested phase
  // through the session update API after Safari has connected instead.
  const created = await webDriverRequest("POST", "/wd/hub/session", {
    capabilities: {
      alwaysMatch: {
        browserName: "safari",
        pageLoadStrategy: "eager",
        acceptInsecureCerts: true,
        "appium:nativeWebTap": true,
        "bstack:options": browserStackOptions,
      },
    },
  });
  const sessionId = created.sessionId || created.session_id;
  if (!sessionId) {
    throw new Error("BrowserStack WebDriver did not return a session id");
  }
  await webDriverRequest("POST", `/wd/hub/session/${sessionId}/timeouts`, {
    script: 30000,
  });
  const page = new SafariWebDriverPage(sessionId, {
    sessionId,
    browser: "safari",
    device: browserStackOptions.deviceName,
    os: "ios",
    os_version: browserStackOptions.osVersion,
    build_name: browserStackOptions.buildName,
    project_name: browserStackOptions.projectName,
  });
  const context = {
    addInitScript: async () => {},
    newPage: async () => page,
    close: async () => {},
  };
  return {
    context,
    close: async () => {
      await webDriverRequest("DELETE", `/wd/hub/session/${sessionId}`).catch(
        () => {},
      );
    },
  };
}

function buildCapabilities(profile, profileName, firstPhase, buildName) {
  const caps = {
    ...profile.caps,
    project: "OBS VDO.Ninja Plugin",
    build: buildName,
    name: `Remote viewer recovery ${profileName}`,
    "browserstack.username": process.env.BROWSERSTACK_USERNAME,
    "browserstack.accessKey": process.env.BROWSERSTACK_ACCESS_KEY,
    "browserstack.playwrightVersion":
      process.env.BROWSERSTACK_PLAYWRIGHT_VERSION || "1.latest",
    "client.playwrightVersion":
      process.env.CLIENT_PLAYWRIGHT_VERSION || playwrightPackage.version,
    "browserstack.console": "info",
    "browserstack.networkLogs": "true",
    "browserstack.video": "true",
    "browserstack.debug": process.env.BROWSERSTACK_DEBUG || "false",
  };
  if (firstPhase.customNetwork) {
    caps["browserstack.customNetwork"] = firstPhase.customNetwork;
  } else {
    caps["browserstack.networkProfile"] = firstPhase.networkProfile;
  }
  return caps;
}

async function connectBrowserStack(profile, caps) {
  if (profile.ios) {
    return connectSafariWebDriver(profile, caps);
  }
  const webSocket = browserStackWebSocket(caps);
  if (profile.android) {
    const device = await _android.connect(webSocket);
    const context = await device.launchBrowser();
    return {
      context,
      close: async () => {
        await context.close().catch(() => {});
        await device.close().catch(() => {});
      },
    };
  }

  const browser = await chromium.connect(webSocket);
  let context;
  try {
    context = await browser.newContext({
      permissions: ["camera", "microphone"],
      viewport: { width: 1280, height: 720 },
    });
  } catch (error) {
    if (!/permission/i.test(String(error && error.message))) {
      throw error;
    }
    context = await browser.newContext({
      viewport: { width: 1280, height: 720 },
    });
  }
  return {
    context,
    close: async () => {
      await context.close().catch(() => {});
      await browser.close().catch(() => {});
    },
  };
}

async function getSessionDetails(page) {
  if (page.browserStackSessionDetails) {
    return page.browserStackSessionDetails;
  }
  const payload = JSON.stringify({ action: "getSessionDetails" });
  const raw = await page.evaluate(
    function browserStackExecutor() {},
    `browserstack_executor: ${payload}`,
  );
  if (typeof raw === "string") {
    try {
      return JSON.parse(raw);
    } catch (_) {
      return { raw };
    }
  }
  return raw || {};
}

function findSessionId(details) {
  const candidates = [
    details.hashed_id,
    details.session_id,
    details.sessionId,
    details.automation_session,
    details.automationSession,
  ];
  return candidates.find((value) => typeof value === "string" && value) || "";
}

async function updateNetwork(sessionId, phase) {
  const response = await fetch(
    `https://api.browserstack.com/automate/sessions/${encodeURIComponent(
      sessionId,
    )}/update_network.json`,
    {
      method: "PUT",
      headers: {
        Authorization:
          "Basic " +
          Buffer.from(
            `${process.env.BROWSERSTACK_USERNAME}:${process.env.BROWSERSTACK_ACCESS_KEY}`,
          ).toString("base64"),
        "Content-Type": "application/json",
      },
      body: JSON.stringify(
        phase.customNetwork
          ? { customNetwork: phase.customNetwork }
          : { networkProfile: phase.networkProfile },
      ),
    },
  );
  const body = await response.text();
  if (!response.ok) {
    throw new Error(
      `BrowserStack network update failed (${response.status}): ${body.slice(
        0,
        500,
      )}`,
    );
  }
  return { status: response.status, body: body.slice(0, 500) };
}

async function installPeerCapture(context) {
  await context.addInitScript(() => {
    window.__obsPluginPeerConnections = [];
    const NativePeerConnection = window.RTCPeerConnection;
    if (!NativePeerConnection) {
      return;
    }
    window.RTCPeerConnection = function wrappedPeerConnection(...args) {
      const peerConnection = new NativePeerConnection(...args);
      window.__obsPluginPeerConnections.push(peerConnection);
      return peerConnection;
    };
    window.RTCPeerConnection.prototype = NativePeerConnection.prototype;
  });
}

async function startPresentationCapture(page, requireMarker, markerFormat) {
  return page.evaluate(
    ({ markerRequired, format }) => {
      const video = Array.from(document.querySelectorAll("video")).find(
        (candidate) =>
          candidate.srcObject &&
          candidate.videoWidth > 0 &&
          candidate.videoHeight > 0,
      );
      if (!video) {
        throw new Error(
          "No playable video element exists for presentation capture",
        );
      }
      if (typeof video.requestVideoFrameCallback !== "function") {
        throw new Error(
          "This browser does not support requestVideoFrameCallback",
        );
      }

      const capture = {
        active: true,
        records: [],
        maximumRecords: 120000,
        markerRequired,
        markerFormat: format,
        markerCanvas: document.createElement("canvas"),
        markerError: "",
      };
      capture.markerCanvas.width = 32;
      capture.markerCanvas.height = 1;
      capture.markerContext = capture.markerCanvas.getContext("2d", {
        alpha: false,
        willReadFrequently: true,
      });

      function crc8(value) {
        let crc = 0;
        for (const byte of [(value >>> 8) & 255, value & 255]) {
          crc ^= byte;
          for (let bit = 0; bit < 8; bit += 1) {
            crc = crc & 0x80 ? ((crc << 1) ^ 0x07) & 255 : (crc << 1) & 255;
          }
        }
        return crc;
      }

      function grayToBinary(gray) {
        let binary = gray;
        for (let shift = 1; shift < 16; shift <<= 1) {
          binary ^= binary >>> shift;
        }
        return binary & 0xffff;
      }

      function decodeMarker() {
        if (!capture.markerRequired) {
          return { markerFrame: null, markerError: "" };
        }
        try {
          capture.markerContext.drawImage(
            video,
            video.videoWidth * (192 / 1920),
            video.videoHeight * (920 / 1080),
            video.videoWidth * (1536 / 1920),
            video.videoHeight * (96 / 1080),
            0,
            0,
            32,
            1,
          );
          const pixels = capture.markerContext.getImageData(0, 0, 32, 1).data;
          const luma = [];
          for (let offset = 0; offset < pixels.length; offset += 4) {
            luma.push(
              pixels[offset] * 0.2126 +
                pixels[offset + 1] * 0.7152 +
                pixels[offset + 2] * 0.0722,
            );
          }
          const minimum = Math.min(...luma);
          const maximum = Math.max(...luma);
          if (maximum - minimum < 64) {
            return {
              markerFrame: null,
              markerError: "low-contrast",
              markerObserved: { minimum, maximum },
            };
          }
          const threshold = (minimum + maximum) / 2;
          let sync = 0;
          let gray = 0;
          let checksum = 0;
          for (let bit = 0; bit < 8; bit += 1) {
            sync = (sync << 1) | (luma[bit] > threshold ? 1 : 0);
          }
          for (let bit = 8; bit < 24; bit += 1) {
            gray = (gray << 1) | (luma[bit] > threshold ? 1 : 0);
          }
          for (let bit = 24; bit < 32; bit += 1) {
            checksum = (checksum << 1) | (luma[bit] > threshold ? 1 : 0);
          }
          if (sync !== 0xd3) {
            return {
              markerFrame: null,
              markerError: "sync",
              markerObserved: { minimum, maximum, sync, gray, checksum },
            };
          }
          const markerFrame =
            capture.markerFormat === "counter-complement"
              ? gray
              : grayToBinary(gray);
          const expectedChecksum =
            capture.markerFormat === "counter-complement"
              ? ~markerFrame & 255
              : crc8(markerFrame);
          if (expectedChecksum !== checksum) {
            return {
              markerFrame: null,
              markerError: "crc",
              markerObserved: { minimum, maximum, sync, gray, checksum },
            };
          }
          return {
            markerFrame,
            markerError: "",
            markerContrast: maximum - minimum,
          };
        } catch (error) {
          capture.markerError = String(error);
          return { markerFrame: null, markerError: String(error) };
        }
      }

      function onFrame(callbackTime, metadata) {
        if (!capture.active) {
          return;
        }
        if (capture.records.length < capture.maximumRecords) {
          capture.records.push({
            callbackTime,
            expectedDisplayTime: metadata.expectedDisplayTime,
            presentationTime: metadata.presentationTime,
            mediaTime: metadata.mediaTime,
            presentedFrames: metadata.presentedFrames,
            processingDuration: metadata.processingDuration,
            width: metadata.width,
            height: metadata.height,
            ...decodeMarker(),
          });
        }
        video.requestVideoFrameCallback(onFrame);
      }

      window.__obsPluginPresentationCapture = capture;
      video.requestVideoFrameCallback(onFrame);
      return {
        supported: true,
        requireMarker: markerRequired,
        videoWidth: video.videoWidth,
        videoHeight: video.videoHeight,
      };
    },
    { markerRequired: requireMarker ? 1 : 0, format: markerFormat },
  );
}

async function stopPresentationCapture(page) {
  return page.evaluate(() => {
    const capture = window.__obsPluginPresentationCapture;
    if (!capture) {
      return { records: [], error: "capture-not-started" };
    }
    capture.active = false;
    const markerDiagnostics = {};
    for (const record of capture.records) {
      const key = record.markerError || "valid";
      markerDiagnostics[key] = (markerDiagnostics[key] || 0) + 1;
    }
    const result = {
      records: capture.records,
      markerError: capture.markerError,
      markerDiagnostics,
      markerSamples: capture.records.slice(0, 10).map((record) => ({
        markerFrame: record.markerFrame,
        markerError: record.markerError,
        markerContrast: record.markerContrast,
        markerObserved: record.markerObserved,
        width: record.width,
        height: record.height,
      })),
      truncated: capture.records.length >= capture.maximumRecords,
    };
    delete window.__obsPluginPresentationCapture;
    return result;
  });
}

async function collectSnapshot(page) {
  return page.evaluate(async () => {
    const safeUrl = new URL(location.href);
    for (const name of ["password", "pass", "pw", "hash", "salt"]) {
      if (safeUrl.searchParams.has(name)) {
        safeUrl.searchParams.set(name, "<redacted>");
      }
    }
    const videos = Array.from(document.querySelectorAll("video")).map(
      (video, index) => {
        const stream = video.srcObject;
        return {
          index,
          currentTime: video.currentTime,
          readyState: video.readyState,
          paused: video.paused,
          videoWidth: video.videoWidth,
          videoHeight: video.videoHeight,
          audioTracks:
            stream && stream.getAudioTracks
              ? stream.getAudioTracks().length
              : 0,
          videoTracks:
            stream && stream.getVideoTracks
              ? stream.getVideoTracks().length
              : 0,
        };
      },
    );
    const peerConnections = new Set(window.__obsPluginPeerConnections || []);
    if (window.session) {
      for (const collection of [window.session.pcs, window.session.rpcs]) {
        for (const candidate of Object.values(collection || {})) {
          if (candidate && typeof candidate.getStats === "function") {
            peerConnections.add(candidate);
          } else if (
            candidate &&
            candidate.pc &&
            typeof candidate.pc.getStats === "function"
          ) {
            peerConnections.add(candidate.pc);
          }
        }
      }
    }
    const peers = [];
    for (const peerConnection of peerConnections) {
      const peer = {
        connectionState: peerConnection.connectionState,
        iceConnectionState: peerConnection.iceConnectionState,
        inbound: [],
        selectedCandidatePair: null,
      };
      try {
        const stats = await peerConnection.getStats();
        const byId = new Map();
        stats.forEach((stat) => byId.set(stat.id, stat));
        stats.forEach((stat) => {
          if (stat.type === "inbound-rtp" && !stat.isRemote) {
            const codec = byId.get(stat.codecId);
            peer.inbound.push({
              kind: stat.kind || stat.mediaType || "",
              codec: codec ? codec.mimeType || "" : "",
              bytesReceived: stat.bytesReceived || 0,
              packetsReceived: stat.packetsReceived || 0,
              packetsLost: stat.packetsLost || 0,
              packetsDiscarded: stat.packetsDiscarded || 0,
              framesDecoded: stat.framesDecoded || 0,
              framesDropped: stat.framesDropped || 0,
              freezeCount: stat.freezeCount || 0,
              totalFreezesDuration: stat.totalFreezesDuration || 0,
              keyFramesDecoded: stat.keyFramesDecoded || 0,
              nackCount: stat.nackCount || 0,
              pliCount: stat.pliCount || 0,
              firCount: stat.firCount || 0,
              fecPacketsReceived: stat.fecPacketsReceived || 0,
              fecPacketsDiscarded: stat.fecPacketsDiscarded || 0,
              jitter: stat.jitter || 0,
              jitterBufferDelay: stat.jitterBufferDelay || 0,
              jitterBufferEmittedCount: stat.jitterBufferEmittedCount || 0,
              concealedSamples: stat.concealedSamples || 0,
              concealmentEvents: stat.concealmentEvents || 0,
            });
          }
          if (
            stat.type === "candidate-pair" &&
            (stat.selected || stat.nominated) &&
            stat.state === "succeeded"
          ) {
            const local = byId.get(stat.localCandidateId);
            const remote = byId.get(stat.remoteCandidateId);
            peer.selectedCandidatePair = {
              currentRoundTripTime: stat.currentRoundTripTime || 0,
              availableIncomingBitrate: stat.availableIncomingBitrate || 0,
              availableOutgoingBitrate: stat.availableOutgoingBitrate || 0,
              localCandidateType: local ? local.candidateType || "" : "",
              localProtocol: local ? local.protocol || "" : "",
              remoteCandidateType: remote ? remote.candidateType || "" : "",
              remoteProtocol: remote ? remote.protocol || "" : "",
            };
          }
        });
      } catch (error) {
        peer.error = String(error);
      }
      peers.push(peer);
    }
    const receiverCodecs =
      window.RTCRtpReceiver && RTCRtpReceiver.getCapabilities
        ? (RTCRtpReceiver.getCapabilities("video").codecs || []).map(
            (codec) => codec.mimeType || "",
          )
        : [];
    return {
      timestamp: Date.now(),
      url: safeUrl.toString(),
      userAgent: navigator.userAgent,
      videos,
      peers,
      receiverCodecs,
    };
  });
}

function totalMetric(snapshot, kind, metric) {
  let total = 0;
  for (const peer of snapshot.peers) {
    for (const inbound of peer.inbound || []) {
      if (!kind || inbound.kind === kind) {
        total += Number(inbound[metric]) || 0;
      }
    }
  }
  return total;
}

function videoTime(snapshot) {
  return snapshot.videos.reduce(
    (maximum, video) => Math.max(maximum, Number(video.currentTime) || 0),
    0,
  );
}

function hasPlayableVideo(snapshot) {
  return snapshot.videos.some(
    (video) =>
      video.videoTracks > 0 && video.videoWidth > 0 && video.videoHeight > 0,
  );
}

function assessNetworkEffect(phase, metrics, expectedMediaKbps) {
  const requested = parseCustomNetwork(phase.customNetwork);
  if (!requested) {
    return {
      status: "unproven",
      reason:
        "Named profiles and malformed customNetwork values cannot be verified from WebRTC stats",
      requested: null,
    };
  }

  const capTolerance = 1.25;
  const capWasExceeded =
    requested.downloadKbps > 0 &&
    metrics.receivedKbps > requested.downloadKbps * capTolerance;
  const lossSignalObserved =
    metrics.packetsLost > 0 ||
    metrics.nackCount > 0 ||
    metrics.pliCount > 0 ||
    metrics.fecPacketsReceived > 0 ||
    metrics.concealmentEvents > 0;
  const capIsTestable =
    requested.downloadKbps > 0 &&
    expectedMediaKbps > requested.downloadKbps * capTolerance;
  const capEffectObserved =
    capIsTestable &&
    metrics.receivedKbps <= requested.downloadKbps * capTolerance;

  if (capWasExceeded) {
    return {
      status: "not-applied-to-media",
      reason: `WebRTC received ${metrics.receivedKbps.toFixed(
        0,
      )} kbps despite a ${requested.downloadKbps} kbps download cap`,
      requested,
      capWasExceeded,
      lossSignalObserved,
      capIsTestable,
      capEffectObserved,
    };
  }
  if (
    (requested.packetLossPercent > 0 && lossSignalObserved) ||
    capEffectObserved
  ) {
    return {
      status: "observed",
      reason:
        requested.packetLossPercent > 0 && lossSignalObserved
          ? "WebRTC receiver stats contain loss or repair signals during the requested loss phase"
          : "Received media stayed below a cap that was lower than the expected stream rate",
      requested,
      capWasExceeded,
      lossSignalObserved,
      capIsTestable,
      capEffectObserved,
    };
  }
  return {
    status: "unproven",
    reason:
      "The BrowserStack API accepted the network setting, but WebRTC stats do not prove that it affected media",
    requested,
    capWasExceeded,
    lossSignalObserved,
    capIsTestable,
    capEffectObserved,
  };
}

function phaseDelta(first, last, phase, defaultExpectedMediaKbps) {
  const durationSeconds = Math.max(
    0.001,
    (last.timestamp - first.timestamp) / 1000,
  );
  const bytesReceived =
    totalMetric(last, "", "bytesReceived") -
    totalMetric(first, "", "bytesReceived");
  const framesDecoded =
    totalMetric(last, "video", "framesDecoded") -
    totalMetric(first, "video", "framesDecoded");
  const currentTimeAdvanced = videoTime(last) - videoTime(first);
  const decodedAndPlayable = framesDecoded > 0 && hasPlayableVideo(last);
  const metrics = {
    name: phase.name,
    network: phase.customNetwork || phase.networkProfile,
    durationSeconds,
    bytesReceived,
    receivedKbps: (bytesReceived * 8) / 1000 / durationSeconds,
    framesDecoded,
    currentTimeAdvanced,
    packetsLost:
      totalMetric(last, "", "packetsLost") -
      totalMetric(first, "", "packetsLost"),
    audioPacketsLost:
      totalMetric(last, "audio", "packetsLost") -
      totalMetric(first, "audio", "packetsLost"),
    videoPacketsLost:
      totalMetric(last, "video", "packetsLost") -
      totalMetric(first, "video", "packetsLost"),
    nackCount:
      totalMetric(last, "video", "nackCount") -
      totalMetric(first, "video", "nackCount"),
    pliCount:
      totalMetric(last, "video", "pliCount") -
      totalMetric(first, "video", "pliCount"),
    firCount:
      totalMetric(last, "video", "firCount") -
      totalMetric(first, "video", "firCount"),
    fecPacketsReceived:
      totalMetric(last, "video", "fecPacketsReceived") -
      totalMetric(first, "video", "fecPacketsReceived"),
    fecPacketsDiscarded:
      totalMetric(last, "video", "fecPacketsDiscarded") -
      totalMetric(first, "video", "fecPacketsDiscarded"),
    freezeCount:
      totalMetric(last, "video", "freezeCount") -
      totalMetric(first, "video", "freezeCount"),
    totalFreezesDuration:
      totalMetric(last, "video", "totalFreezesDuration") -
      totalMetric(first, "video", "totalFreezesDuration"),
    concealedSamples:
      totalMetric(last, "audio", "concealedSamples") -
      totalMetric(first, "audio", "concealedSamples"),
    concealmentEvents:
      totalMetric(last, "audio", "concealmentEvents") -
      totalMetric(first, "audio", "concealmentEvents"),
    playableAtEnd: hasPlayableVideo(last),
    advancedAsExpected:
      !phase.expectAdvance ||
      (decodedAndPlayable &&
        (!phase.requirePlaybackClock || currentTimeAdvanced > 0.4)),
    requirePlaybackClock: phase.requirePlaybackClock,
  };
  const expectedMediaKbps = phase.expectedMediaKbps || defaultExpectedMediaKbps;
  return {
    ...metrics,
    requireNetworkEffect: phase.requireNetworkEffect,
    expectedMediaKbps,
    networkEffect: assessNetworkEffect(phase, metrics, expectedMediaKbps),
  };
}

async function waitForPlayableVideo(page, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let latest = null;
  while (Date.now() < deadline) {
    latest = await collectSnapshot(page);
    if (
      hasPlayableVideo(latest) &&
      totalMetric(latest, "video", "bytesReceived") > 5000
    ) {
      return latest;
    }
    await page.waitForTimeout(1000);
  }
  throw new Error(
    `Remote viewer did not receive playable video: ${JSON.stringify(latest)}`,
  );
}

async function unlockPlayback(page) {
  await page.evaluate(() => {
    const resumePlayback = () => {
      for (const video of document.querySelectorAll("video")) {
        video.play().catch(() => {});
      }
      const contexts = [
        window.audioContext,
        window.audioCtx,
        window.session && window.session.audioContext,
        window.session && window.session.audioCtx,
      ];
      for (const context of contexts) {
        if (context && typeof context.resume === "function") {
          context.resume().catch(() => {});
        }
      }
    };
    document.addEventListener("pointerdown", resumePlayback, {
      capture: true,
      once: true,
    });
    document.addEventListener("touchstart", resumePlayback, {
      capture: true,
      once: true,
    });
    document.addEventListener("click", resumePlayback, {
      capture: true,
      once: true,
    });
  });
  const viewport = page.viewportSize() || { width: 1280, height: 720 };
  await page.mouse
    .click(Math.floor(viewport.width / 2), Math.floor(viewport.height / 2))
    .catch(() => {});
  await page.waitForTimeout(500);
  return page.evaluate(() =>
    Array.from(document.querySelectorAll("video")).map((video, index) => ({
      index,
      paused: video.paused,
      muted: video.muted,
      currentTime: video.currentTime,
      readyState: video.readyState,
    })),
  );
}

async function markSession(page, status, reason) {
  if (page.browserStackSessionDetails) {
    return;
  }
  const payload = JSON.stringify({
    action: "setSessionStatus",
    arguments: {
      status,
      reason: String(reason).slice(0, 255),
    },
  });
  await page
    .evaluate(
      function browserStackExecutor() {},
      `browserstack_executor: ${payload}`,
    )
    .catch(() => {});
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const profileName = args.profile || "android-pixel9-chrome";
  const profile = PROFILES[profileName];
  if (!profile) {
    throw new Error(`Unknown BrowserStack profile: ${profileName}`);
  }
  if (!args.url) {
    throw new Error("Pass the complete VDO.Ninja viewer URL with --url");
  }
  loadSecrets(args["secret-file"] || process.env.BROWSERSTACK_SECRET_FILE);
  const requireNetworkEffect = isEnabled(args["require-network-effect"]);
  const expectedMediaKbps = asNonNegativeNumber(args["expected-media-kbps"]);
  const requirePresentation = isEnabled(args["require-presentation"]);
  const requireVisualSequence = isEnabled(args["require-visual-sequence"]);
  const requirePlaybackClock = isEnabled(args["require-playback-clock"]);
  const requireZeroAudioConcealment = isEnabled(
    args["require-zero-audio-concealment"],
  );
  const capturePresentation =
    requirePresentation ||
    requireVisualSequence ||
    isEnabled(args["capture-presentation"]);
  const expectedFps = Math.max(
    1,
    asNonNegativeNumber(args["expected-fps"], 30),
  );
  const minimumAverageFpsRatio = asNonNegativeNumber(
    args["minimum-average-fps-ratio"],
    0.95,
  );
  const maximumCallbackDeviationMs = asNonNegativeNumber(
    args["maximum-callback-deviation-ms"],
    Math.max(8, (1000 / expectedFps) * 0.75),
  );
  const maximumPresentationStallMs = asNonNegativeNumber(
    args["maximum-presentation-stall-ms"],
    Math.max(100, (1000 / expectedFps) * 3.5),
  );
  const phases = parsePhases(args.phases || process.env.VDONINJA_BS_PHASES).map(
    (phase) => ({
      ...phase,
      requireNetworkEffect: requireNetworkEffect || phase.requireNetworkEffect,
      requirePlaybackClock,
    }),
  );
  const buildName =
    args.build ||
    `obs-plugin-viewer-${new Date().toISOString().replace(/[:.]/g, "-")}`;
  const caps = buildCapabilities(profile, profileName, phases[0], buildName);
  const viewerUrl = new URL(args.url);
  const extraViewParams = new URLSearchParams(
    String(args["view-params"] || "").replace(/^[?&]+/, ""),
  );
  extraViewParams.forEach((value, key) => {
    viewerUrl.searchParams.set(key, value);
  });
  viewerUrl.searchParams.set("autostart", "1");
  viewerUrl.searchParams.set("cleanoutput", "1");
  viewerUrl.searchParams.set("debug", "");
  const outputPath = path.resolve(
    args.output ||
      path.join(
        "artifacts",
        `browserstack-viewer-${profileName}-${Date.now()}.json`,
      ),
  );
  fs.mkdirSync(path.dirname(outputPath), { recursive: true });

  const report = {
    ok: false,
    profile: profileName,
    profileLabel: profile.label,
    buildName,
    viewUrl: redactViewerUrl(viewerUrl.toString()),
    phases,
    startedAt: new Date().toISOString(),
    console: [],
    pageErrors: [],
    phaseResults: [],
    validation: {
      capturePresentation,
      requirePresentation,
      requireVisualSequence,
      requirePlaybackClock,
      requireZeroAudioConcealment,
      expectedFps,
      minimumAverageFpsRatio,
      maximumCallbackDeviationMs,
      maximumPresentationStallMs,
    },
    outputPath,
  };
  let connection;
  let page;
  let presentationCaptureActive = false;
  try {
    connection = await connectBrowserStack(profile, caps);
    await installPeerCapture(connection.context);
    page = await connection.context.newPage();
    page.on("console", (message) => {
      if (message.type() === "warning" || message.type() === "error") {
        report.console.push({
          type: message.type(),
          text: message.text().slice(0, 1000),
        });
      }
    });
    page.on("pageerror", (error) => {
      report.pageErrors.push(
        String(error && error.stack ? error.stack : error),
      );
    });
    await page.goto(viewerUrl.toString(), {
      waitUntil: "domcontentloaded",
      timeout: asPositiveInteger(args["navigation-timeout-ms"], 90000),
    });
    const viewport = page.viewportSize() || { width: 1280, height: 720 };
    await page.mouse
      .click(Math.floor(viewport.width / 2), Math.floor(viewport.height / 2))
      .catch(() => {});
    report.firstPlayable = await waitForPlayableVideo(
      page,
      asPositiveInteger(args["connect-timeout-ms"], 120000),
    );
    report.playbackUnlock = await unlockPlayback(page);
    const sessionDetails = await getSessionDetails(page);
    const sessionId = findSessionId(sessionDetails);
    if (!sessionId) {
      throw new Error("BrowserStack did not return a session id");
    }
    report.sessionId = sessionId;
    report.sessionDetails = sanitizeSessionDetails(sessionDetails);
    if (capturePresentation) {
      report.presentationCaptureStart = await startPresentationCapture(
        page,
        requireVisualSequence,
        String(args["marker-format"] || "counter-complement"),
      );
      presentationCaptureActive = true;
    }

    for (const phase of phases) {
      const networkUpdate = await updateNetwork(sessionId, phase);
      await page.waitForTimeout(1000);
      const first = await collectSnapshot(page);
      const samples = [first];
      const deadline = Date.now() + phase.durationMs;
      while (Date.now() < deadline) {
        await page.waitForTimeout(
          Math.min(
            asPositiveInteger(args["sample-ms"], 1000),
            Math.max(1, deadline - Date.now()),
          ),
        );
        samples.push(await collectSnapshot(page));
      }
      const last = samples[samples.length - 1];
      report.phaseResults.push({
        ...phaseDelta(first, last, phase, expectedMediaKbps),
        networkUpdate,
        first,
        last,
        samples,
      });
    }

    if (presentationCaptureActive) {
      const capture = await stopPresentationCapture(page);
      presentationCaptureActive = false;
      report.presentationCapture = {
        markerError: capture.markerError,
        markerDiagnostics: capture.markerDiagnostics,
        markerSamples: capture.markerSamples,
        truncated: capture.truncated,
        recordCount: capture.records.length,
      };
      report.presentationContinuityAnalysis = analyzePresentationContinuity(
        capture.records,
        {
          expectedFps,
          minimumAverageFpsRatio,
          maximumCallbackDeviationMs,
          maximumPresentationStallMs,
          requireMarker: requireVisualSequence,
        },
      );
      if (requireVisualSequence) {
        report.visualSequenceAnalysis = analyzeVisualSequence(capture.records);
      }
    }

    report.finalSnapshot = await collectSnapshot(page);
    const requiredCandidateType = String(
      args["require-candidate-type"] || "",
    ).toLowerCase();
    const selectedCandidatePairs = report.finalSnapshot.peers
      .map((peer) => peer.selectedCandidatePair)
      .filter(Boolean);
    report.candidateTypeRequirement = requiredCandidateType
      ? {
          required: requiredCandidateType,
          matched: selectedCandidatePairs.some(
            (pair) =>
              String(pair.localCandidateType).toLowerCase() ===
                requiredCandidateType ||
              String(pair.remoteCandidateType).toLowerCase() ===
                requiredCandidateType,
          ),
        }
      : null;
    report.ok =
      hasPlayableVideo(report.finalSnapshot) &&
      report.phaseResults.every((phase) => phase.advancedAsExpected) &&
      report.phaseResults.every(
        (phase) =>
          !phase.requireNetworkEffect ||
          phase.networkEffect.status === "observed",
      ) &&
      (!report.candidateTypeRequirement ||
        report.candidateTypeRequirement.matched) &&
      (!requireZeroAudioConcealment ||
        report.phaseResults.every(
          (phase) =>
            phase.audioPacketsLost === 0 &&
            phase.concealedSamples === 0 &&
            phase.concealmentEvents === 0,
        )) &&
      (!requirePresentation ||
        (report.presentationContinuityAnalysis &&
          report.presentationContinuityAnalysis.ok)) &&
      (!requireVisualSequence ||
        (report.visualSequenceAnalysis && report.visualSequenceAnalysis.ok));
    await page
      .screenshot({
        path: outputPath.replace(/\.json$/i, ".png"),
        fullPage: true,
      })
      .catch((error) => {
        report.screenshotError = String(
          error && error.stack ? error.stack : error,
        );
      });
    await markSession(
      page,
      report.ok ? "passed" : "failed",
      report.ok
        ? "Remote VDO.Ninja viewer recovery phases passed"
        : "One or more recovery phases did not advance",
    );
  } catch (error) {
    report.ok = false;
    report.error = String(error && error.stack ? error.stack : error);
    if (page) {
      await markSession(page, "failed", report.error);
    }
  } finally {
    if (presentationCaptureActive && page) {
      await stopPresentationCapture(page).catch(() => {});
      presentationCaptureActive = false;
    }
    report.finishedAt = new Date().toISOString();
    fs.writeFileSync(
      outputPath,
      `${JSON.stringify(report, null, 2)}\n`,
      "utf8",
    );
    if (connection) {
      await connection.close();
    }
  }

  if (args.quiet === "1") {
    process.stdout.write(
      `${JSON.stringify({
        ok: report.ok,
        profile: report.profile,
        sessionId: report.sessionId || null,
        presentationContinuityAnalysis:
          report.presentationContinuityAnalysis || null,
        visualSequenceAnalysis: report.visualSequenceAnalysis || null,
        phaseResults: report.phaseResults.map((phase) => ({
          name: phase.name,
          network: phase.network,
          receivedKbps: phase.receivedKbps,
          framesDecoded: phase.framesDecoded,
          packetsLost: phase.packetsLost,
          audioPacketsLost: phase.audioPacketsLost,
          videoPacketsLost: phase.videoPacketsLost,
          nackCount: phase.nackCount,
          pliCount: phase.pliCount,
          fecPacketsReceived: phase.fecPacketsReceived,
          freezeCount: phase.freezeCount,
          concealedSamples: phase.concealedSamples,
          concealmentEvents: phase.concealmentEvents,
          currentTimeAdvanced: phase.currentTimeAdvanced,
          advancedAsExpected: phase.advancedAsExpected,
          networkEffect: phase.networkEffect,
        })),
        outputPath,
        error: report.error || null,
      })}\n`,
    );
  } else {
    process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
  }
  if (!report.ok) {
    process.exitCode = 1;
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exitCode = 1;
});
