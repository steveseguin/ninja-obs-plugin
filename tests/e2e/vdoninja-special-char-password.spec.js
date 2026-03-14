/**
 * E2E test: verify VDO.Ninja publish->view works with a special-character password.
 *
 * This validates that:
 * 1. The special-char password is correctly URL-encoded in push/view URLs
 * 2. Both sides land in the same signaling room (hashes match)
 * 3. SDP encryption/decryption succeeds with the encoded password
 * 4. Video actually flows end-to-end
 */
const { test, expect } = require("@playwright/test");

const STREAM_ID = "e2eSpecCharPw_" + Date.now().toString(36);
const PASSWORD = "GamerTime420$";

test.use({
  launchOptions: {
    args: [
      "--autoplay-policy=no-user-gesture-required",
      "--use-fake-ui-for-media-stream",
      "--use-fake-device-for-media-stream",
      "--allow-http-screen-capture",
    ],
  },
});

function installPeerCollectorScript() {
  return () => {
    window.__pcList = [];
    const NativePC = window.RTCPeerConnection;
    if (!NativePC) return;
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
    const videos = Array.from(document.querySelectorAll("video")).map((v, i) => ({
      index: i,
      currentTime: v.currentTime,
      videoWidth: v.videoWidth,
      videoHeight: v.videoHeight,
      videoTracks: v.srcObject?.getVideoTracks?.().length ?? 0,
      audioTracks: v.srcObject?.getAudioTracks?.().length ?? 0,
    }));
    const pcStats = [];
    if (Array.isArray(window.__pcList)) {
      for (const pc of window.__pcList) {
        try {
          const stats = await pc.getStats();
          let inboundVideoBytes = 0, inboundAudioBytes = 0;
          stats.forEach((s) => {
            if (s.type === "inbound-rtp" && !s.isRemote) {
              if (s.kind === "video") inboundVideoBytes += s.bytesReceived || 0;
              if (s.kind === "audio") inboundAudioBytes += s.bytesReceived || 0;
            }
          });
          pcStats.push({ state: pc.connectionState, inboundVideoBytes, inboundAudioBytes });
        } catch (e) {
          pcStats.push({ error: String(e) });
        }
      }
    }
    return { videos, pcStats };
  });
}

function sumInbound(snap) {
  return snap.pcStats.reduce((a, s) => a + (s.inboundVideoBytes || 0) + (s.inboundAudioBytes || 0), 0);
}

function hasTracks(snap) {
  return snap.videos.some((v) => v.videoTracks > 0 || v.audioTracks > 0);
}

test("special-char password ($) — publish -> view receives media", async ({ browser }) => {
  test.setTimeout(180000);

  const pushParams = new URLSearchParams({
    autostart: "1",
    webcam: "1",
    cleanoutput: "1",
    password: PASSWORD,
  });
  const viewParams = new URLSearchParams({
    cleanoutput: "1",
    password: PASSWORD,
  });

  const pushUrl = `https://vdo.ninja/?push=${encodeURIComponent(STREAM_ID)}&${pushParams}`;
  const viewUrl = `https://vdo.ninja/?view=${encodeURIComponent(STREAM_ID)}&${viewParams}`;

  const publisherCtx = await browser.newContext({ permissions: ["camera", "microphone"] });
  const viewerCtx = await browser.newContext();
  const publisher = await publisherCtx.newPage();
  const viewer = await viewerCtx.newPage();

  await publisher.addInitScript(installPeerCollectorScript());
  await viewer.addInitScript(installPeerCollectorScript());

  // Start publisher
  await publisher.goto(pushUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  await publisher.waitForTimeout(12000);

  const cameraBtn = publisher.getByRole("button", { name: /share your camera/i });
  const cameraTxt = publisher.getByText(/share your camera/i).first();
  if (await cameraBtn.isVisible().catch(() => false)) {
    await cameraBtn.click({ timeout: 3000 }).catch(() => {});
  } else if (await cameraTxt.isVisible().catch(() => false)) {
    await cameraTxt.click({ timeout: 3000 }).catch(() => {});
  }

  const vp = publisher.viewportSize() || { width: 1280, height: 720 };
  await publisher.mouse.click(vp.width / 2, vp.height / 2);
  await publisher.waitForTimeout(7000);

  // Start viewer
  await viewer.goto(viewUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  const vp2 = viewer.viewportSize() || { width: 1280, height: 720 };
  await viewer.mouse.click(vp2.width / 2, vp2.height / 2);

  // Wait for media to flow
  await expect
    .poll(
      async () => {
        const snap = await collectSnapshot(viewer);
        return hasTracks(snap) && sumInbound(snap) > 5000;
      },
      { timeout: 70000, intervals: [1000, 2000, 3000] }
    )
    .toBeTruthy();

  const snap = await collectSnapshot(viewer);
  expect(hasTracks(snap)).toBeTruthy();
  expect(sumInbound(snap)).toBeGreaterThan(5000);

  await viewerCtx.close();
  await publisherCtx.close();
});
