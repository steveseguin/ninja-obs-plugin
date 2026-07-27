const fs = require("fs");
const path = require("path");
const { chromium, _android } = require("playwright");
const playwrightPackage = require("@playwright/test/package.json");

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
  return (
    value === true || value === 1 || String(value).toLowerCase() === "true"
  );
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
    const peers = [];
    for (const peerConnection of window.__obsPluginPeerConnections || []) {
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
      (framesDecoded > 0 &&
        currentTimeAdvanced > 0.4 &&
        hasPlayableVideo(last)),
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

async function markSession(page, status, reason) {
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
  const phases = parsePhases(args.phases || process.env.VDONINJA_BS_PHASES).map(
    (phase) => ({
      ...phase,
      requireNetworkEffect: requireNetworkEffect || phase.requireNetworkEffect,
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
    outputPath,
  };
  let connection;
  let page;
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
    const sessionDetails = await getSessionDetails(page);
    const sessionId = findSessionId(sessionDetails);
    if (!sessionId) {
      throw new Error("BrowserStack did not return a session id");
    }
    report.sessionId = sessionId;
    report.sessionDetails = sanitizeSessionDetails(sessionDetails);

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
        report.candidateTypeRequirement.matched);
    await page.screenshot({
      path: outputPath.replace(/\.json$/i, ".png"),
      fullPage: true,
    });
    await markSession(
      page,
      report.ok ? "passed" : "failed",
      report.ok
        ? "Remote VDO.Ninja viewer recovery phases passed"
        : "One or more recovery phases did not advance",
    );
  } catch (error) {
    report.error = String(error && error.stack ? error.stack : error);
    if (page) {
      await markSession(page, "failed", report.error);
    }
  } finally {
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
        phaseResults: report.phaseResults.map((phase) => ({
          name: phase.name,
          network: phase.network,
          receivedKbps: phase.receivedKbps,
          framesDecoded: phase.framesDecoded,
          packetsLost: phase.packetsLost,
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
