const { chromium } = require("playwright");

function ensureQuery(url, key, value) {
  const u = new URL(url);
  if (!u.searchParams.has(key)) {
    u.searchParams.set(key, value);
  }
  return u.toString();
}

async function collectMediaSnapshot(page) {
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
          let outboundVideoBytes = 0;
          let outboundAudioBytes = 0;
          stats.forEach((s) => {
            if (s.type === "inbound-rtp" && !s.isRemote) {
              if (s.kind === "video") inboundVideoBytes += s.bytesReceived || 0;
              if (s.kind === "audio") inboundAudioBytes += s.bytesReceived || 0;
            }
            if (s.type === "outbound-rtp" && !s.isRemote) {
              if (s.kind === "video") outboundVideoBytes += s.bytesSent || 0;
              if (s.kind === "audio") outboundAudioBytes += s.bytesSent || 0;
            }
          });
          pcStats.push({
            state: pc.connectionState,
            inboundVideoBytes,
            inboundAudioBytes,
            outboundVideoBytes,
            outboundAudioBytes,
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
      videoElements: videos,
      pcStats,
      timestamp: Date.now(),
    };
  });
}

async function main() {
  const pushUrlInput = process.argv[2] || "https://vdo.ninja/?push=Alsosuitbc&password=somepassword";
  const viewUrlInput = process.argv[3] || "https://vdo.ninja/?view=Alsosuitbc&password=somepassword";
  const durationMs = Number(process.env.PUBLISH_DURATION_MS || 15 * 60 * 1000);
  const startupWaitMs = Number(process.env.PUBLISH_STARTUP_WAIT_MS || 20000);
  const viewProbeWaitMs = Number(process.env.VIEW_PROBE_WAIT_MS || 25000);

  const pushUrl = ensureQuery(ensureQuery(pushUrlInput, "autostart", "1"), "webcam", "1");
  const viewUrl = ensureQuery(viewUrlInput, "cleanoutput", "1");

  const browser = await chromium.launch({
    headless: process.env.HEADLESS === "0" ? false : true,
    args: [
      "--autoplay-policy=no-user-gesture-required",
      "--use-fake-ui-for-media-stream",
      "--use-fake-device-for-media-stream",
      "--allow-http-screen-capture",
    ],
  });

  const context = await browser.newContext({
    permissions: ["camera", "microphone"],
  });

  await context.addInitScript(() => {
    window.__pcList = [];
    const NativePC = window.RTCPeerConnection;
    if (NativePC) {
      window.RTCPeerConnection = function (...args) {
        const pc = new NativePC(...args);
        window.__pcList.push(pc);
        return pc;
      };
      window.RTCPeerConnection.prototype = NativePC.prototype;
    }
  });

  const publishPage = await context.newPage();
  await publishPage.goto(pushUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  await publishPage.waitForTimeout(startupWaitMs);

  const cameraButtonByRole = publishPage.getByRole("button", { name: /share your camera/i });
  const cameraButtonByText = publishPage.getByText(/share your camera/i).first();
  const canClickRole = await cameraButtonByRole.isVisible().catch(() => false);
  const canClickText = await cameraButtonByText.isVisible().catch(() => false);
  if (canClickRole || canClickText) {
    const target = canClickRole ? cameraButtonByRole : cameraButtonByText;
    await target.click({ timeout: 3000 }).catch(() => {});
    await publishPage.waitForTimeout(5000);
  }

  // Nudge UI in case autostart gating still requires interaction.
  const vp = publishPage.viewportSize() || { width: 1280, height: 720 };
  await publishPage.mouse.click(Math.floor(vp.width / 2), Math.floor(vp.height / 2));
  await publishPage.waitForTimeout(4000);

  const publisherSnapshot = await collectMediaSnapshot(publishPage);

  const viewerPage = await context.newPage();
  await viewerPage.goto(viewUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  await viewerPage.waitForTimeout(viewProbeWaitMs);
  const viewerSample1 = await collectMediaSnapshot(viewerPage);
  await viewerPage.waitForTimeout(7000);
  const viewerSample2 = await collectMediaSnapshot(viewerPage);

  const playbackAdvanced = viewerSample2.videoElements.some((v2) => {
    const v1 = viewerSample1.videoElements.find((x) => x.index === v2.index);
    return v1 && v2.currentTime > v1.currentTime + 0.4;
  });

  const hasInboundBytes = viewerSample2.pcStats.some(
    (s) => (s.inboundVideoBytes || 0) > 0 || (s.inboundAudioBytes || 0) > 0
  );
  const hasTracks = viewerSample2.videoElements.some((v) => v.videoTracks > 0 || v.audioTracks > 0);

  const report = {
    startedAt: new Date().toISOString(),
    pushUrl,
    viewUrl,
    publisherSnapshot,
    viewerSample1,
    viewerSample2,
    playbackAdvanced,
    hasInboundBytes,
    hasTracks,
    likelyConnected: playbackAdvanced || (hasInboundBytes && hasTracks),
    keepAliveMs: durationMs,
  };

  console.log(JSON.stringify(report, null, 2));

  // Keep publisher alive so the user can check live playout.
  await viewerPage.close();
  await publishPage.waitForTimeout(durationMs);

  await context.close();
  await browser.close();
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
