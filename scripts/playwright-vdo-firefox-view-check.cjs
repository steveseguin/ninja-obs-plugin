const { chromium, firefox } = require("playwright");

function hasEnv(name) {
  return Object.prototype.hasOwnProperty.call(process.env, name);
}

function getEnvOrDefault(name, fallback) {
  if (!hasEnv(name)) {
    return fallback;
  }
  return process.env[name] ?? fallback;
}

function getOptionalEnv(name) {
  if (!hasEnv(name)) {
    return "";
  }
  return process.env[name] ?? "";
}

function buildScenarioUrls() {
  const streamId = getEnvOrDefault("VDO_STREAM_ID", "Alsosuitbc");
  const noPassword = getOptionalEnv("VDO_NO_PASSWORD") === "1";
  const password = noPassword ? "" : hasEnv("VDO_PASSWORD") ? process.env.VDO_PASSWORD ?? "" : "somepassword";
  const roomId = getOptionalEnv("VDO_ROOM_ID");
  const bitrate = getOptionalEnv("VDO_BITRATE");
  const includeRoomInView = roomId ? getOptionalEnv("VDO_VIEW_INCLUDE_ROOM") !== "0" : false;
  const includeSceneInView = roomId ? getOptionalEnv("VDO_VIEW_INCLUDE_SCENE") !== "0" : false;

  const params = new URLSearchParams({ cleanoutput: "1" });
  if (password) {
    params.set("password", password);
  }

  const pushParams = new URLSearchParams(params);
  pushParams.set("autostart", "1");
  pushParams.set("webcam", "1");
  if (roomId) {
    pushParams.set("room", roomId);
    if (includeRoomInView) {
      params.set("room", roomId);
      if (includeSceneInView) {
        params.set("scene", "1");
      }
    }
  }
  if (bitrate) {
    pushParams.set("bitrate", bitrate);
  }

  const pushUrl =
    process.env.VDO_PUSH_URL ||
    `https://vdo.ninja/?push=${encodeURIComponent(streamId)}&${pushParams.toString()}`;
  const viewUrl =
    process.env.VDO_VIEW_URL ||
    `https://vdo.ninja/?view=${encodeURIComponent(streamId)}&${params.toString()}`;

  return {
    streamId,
    noPassword,
    password,
    roomId,
    bitrate,
    includeRoomInView,
    includeSceneInView,
    pushUrl,
    viewUrl,
  };
}

function installPeerCollectorScript() {
  return () => {
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
  };
}

async function collectSnapshot(page) {
  return page.evaluate(async () => {
    const videos = Array.from(document.querySelectorAll("video")).map((v, index) => {
      const stream = v.srcObject;
      const audioTracks = stream && stream.getAudioTracks ? stream.getAudioTracks().length : 0;
      const videoTracks = stream && stream.getVideoTracks ? stream.getVideoTracks().length : 0;
      return {
        index,
        currentTime: v.currentTime,
        videoWidth: v.videoWidth,
        videoHeight: v.videoHeight,
        audioTracks,
        videoTracks,
      };
    });

    const pcStats = [];
    if (Array.isArray(window.__pcList)) {
      for (const pc of window.__pcList) {
        try {
          const stats = await pc.getStats();
          let inboundVideoBytes = 0;
          let inboundAudioBytes = 0;
          stats.forEach((s) => {
            if (s.type === "inbound-rtp" && !s.isRemote) {
              if (s.kind === "video") inboundVideoBytes += s.bytesReceived || 0;
              if (s.kind === "audio") inboundAudioBytes += s.bytesReceived || 0;
            }
          });
          pcStats.push({
            state: pc.connectionState,
            inboundVideoBytes,
            inboundAudioBytes,
          });
        } catch (error) {
          pcStats.push({ error: String(error) });
        }
      }
    }

    return { videos, pcStats, timestamp: Date.now(), url: location.href };
  });
}

function sumInbound(snapshot) {
  return snapshot.pcStats.reduce(
    (acc, s) => acc + (s.inboundVideoBytes || 0) + (s.inboundAudioBytes || 0),
    0
  );
}

function hasTracks(snapshot) {
  return snapshot.videos.some((v) => v.videoTracks > 0 || v.audioTracks > 0);
}

function hasMetadata(snapshot) {
  return snapshot.videos.some((v) => v.videoWidth > 0 && v.videoHeight > 0);
}

function playbackAdvanced(before, after) {
  return after.videos.some((v2) => {
    const v1 = before.videos.find((x) => x.index === v2.index);
    return v1 && v2.currentTime > v1.currentTime + 0.4;
  });
}

async function nudgePage(page) {
  const viewport = page.viewportSize() || { width: 1280, height: 720 };
  await page.mouse.click(Math.floor(viewport.width / 2), Math.floor(viewport.height / 2));
}

async function waitForViewerPlayback(viewerPage, timeoutMs) {
  const started = Date.now();
  while (Date.now() - started < timeoutMs) {
    const snap = await collectSnapshot(viewerPage);
    if (hasTracks(snap) && hasMetadata(snap) && sumInbound(snap) > 5000) {
      return snap;
    }
    await viewerPage.waitForTimeout(2000);
  }
  throw new Error(`Timed out waiting for Firefox viewer media after ${timeoutMs}ms`);
}

async function main() {
  const scenario = buildScenarioUrls();

  const publisherBrowser = await chromium.launch({
    headless: true,
    args: [
      "--autoplay-policy=no-user-gesture-required",
      "--use-fake-ui-for-media-stream",
      "--use-fake-device-for-media-stream",
      "--allow-http-screen-capture",
    ],
  });
  const viewerBrowser = await firefox.launch({
    headless: true,
    firefoxUserPrefs: {
      "media.autoplay.default": 0,
      "media.navigator.permission.disabled": true,
      "media.navigator.streams.fake": true,
    },
  });

  const publisherContext = await publisherBrowser.newContext({
    permissions: ["camera", "microphone"],
  });
  const viewerContext = await viewerBrowser.newContext();
  const publisher = await publisherContext.newPage();
  const viewer = await viewerContext.newPage();

  await publisher.addInitScript(installPeerCollectorScript());
  await viewer.addInitScript(installPeerCollectorScript());

  await publisher.goto(scenario.pushUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  await publisher.waitForTimeout(12000);

  const cameraButtonByRole = publisher.getByRole("button", { name: /share your camera/i });
  const cameraButtonByText = publisher.getByText(/share your camera/i).first();
  const roleVisible = await cameraButtonByRole.isVisible().catch(() => false);
  const textVisible = await cameraButtonByText.isVisible().catch(() => false);
  if (roleVisible || textVisible) {
    const target = roleVisible ? cameraButtonByRole : cameraButtonByText;
    await target.click({ timeout: 3000 }).catch(() => {});
  }
  await nudgePage(publisher);
  await publisher.waitForTimeout(7000);

  await viewer.goto(scenario.viewUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  await nudgePage(viewer);

  const viewerSample1 = await waitForViewerPlayback(viewer, 90000);
  await viewer.waitForTimeout(7000);
  const viewerSample2 = await collectSnapshot(viewer);

  const result = {
    scenario,
    viewerSample1,
    viewerSample2,
    hasTracks: hasTracks(viewerSample2),
    hasMetadata: hasMetadata(viewerSample2),
    inboundBytes: sumInbound(viewerSample2),
    playbackAdvanced: playbackAdvanced(viewerSample1, viewerSample2),
  };

  console.log(JSON.stringify(result, null, 2));

  if (!result.hasTracks || !result.hasMetadata || result.inboundBytes <= 5000 || !result.playbackAdvanced) {
    throw new Error("Firefox viewer playback validation failed");
  }

  await viewerContext.close();
  await publisherContext.close();
  await viewerBrowser.close();
  await publisherBrowser.close();
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
