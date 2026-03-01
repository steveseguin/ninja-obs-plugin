const fs = require("fs");
const { test, expect } = require("@playwright/test");
const { buildScenarioUrls } = require("./vdo-config");

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

function sumInboundAudio(snapshot) {
  return snapshot.pcStats.reduce((acc, s) => acc + (s.inboundAudioBytes || 0), 0);
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

test("one publisher can serve multiple viewers with active media", async ({ browser }, testInfo) => {
  test.setTimeout(240000);

  const { streamId, roomId, bitrate, includeRoomInView, includeSceneInView, pushUrl, viewUrl } =
    buildScenarioUrls();

  const publisherContext = await browser.newContext({ permissions: ["camera", "microphone"] });
  const viewerContextA = await browser.newContext();
  const viewerContextB = await browser.newContext();

  const publisher = await publisherContext.newPage();
  const viewerA = await viewerContextA.newPage();
  const viewerB = await viewerContextB.newPage();

  await publisher.addInitScript(installPeerCollectorScript());
  await viewerA.addInitScript(installPeerCollectorScript());
  await viewerB.addInitScript(installPeerCollectorScript());

  await publisher.goto(pushUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  await publisher.waitForTimeout(10000);

  const cameraButtonByRole = publisher.getByRole("button", { name: /share your camera/i });
  const cameraButtonByText = publisher.getByText(/share your camera/i).first();
  const roleVisible = await cameraButtonByRole.isVisible().catch(() => false);
  const textVisible = await cameraButtonByText.isVisible().catch(() => false);
  if (roleVisible || textVisible) {
    const target = roleVisible ? cameraButtonByRole : cameraButtonByText;
    await target.click({ timeout: 3000 }).catch(() => {});
  }

  await nudgePage(publisher);
  await publisher.waitForTimeout(6000);

  await viewerA.goto(viewUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  await viewerB.goto(viewUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  await nudgePage(viewerA);
  await nudgePage(viewerB);

  await expect
    .poll(
      async () => {
        const snapA = await collectSnapshot(viewerA);
        const snapB = await collectSnapshot(viewerB);
        const okA = hasTracks(snapA) && hasMetadata(snapA) && sumInbound(snapA) > 5000 && sumInboundAudio(snapA) > 500;
        const okB = hasTracks(snapB) && hasMetadata(snapB) && sumInbound(snapB) > 5000 && sumInboundAudio(snapB) > 500;
        return okA && okB;
      },
      { timeout: 70000, intervals: [1000, 2000, 3000] }
    )
    .toBeTruthy();

  const beforeA = await collectSnapshot(viewerA);
  const beforeB = await collectSnapshot(viewerB);
  await viewerA.waitForTimeout(7000);
  await viewerB.waitForTimeout(7000);
  const afterA = await collectSnapshot(viewerA);
  const afterB = await collectSnapshot(viewerB);

  expect(hasTracks(afterA)).toBeTruthy();
  expect(hasTracks(afterB)).toBeTruthy();
  expect(hasMetadata(afterA)).toBeTruthy();
  expect(hasMetadata(afterB)).toBeTruthy();
  expect(sumInbound(afterA)).toBeGreaterThan(5000);
  expect(sumInbound(afterB)).toBeGreaterThan(5000);
  expect(sumInboundAudio(afterA)).toBeGreaterThan(500);
  expect(sumInboundAudio(afterB)).toBeGreaterThan(500);
  expect(playbackAdvanced(beforeA, afterA)).toBeTruthy();
  expect(playbackAdvanced(beforeB, afterB)).toBeTruthy();

  const report = {
    streamId,
    roomId,
    bitrate,
    includeRoomInView,
    includeSceneInView,
    viewUrl,
    pushUrl,
    viewerA: { before: beforeA, after: afterA },
    viewerB: { before: beforeB, after: afterB },
    generatedAt: new Date().toISOString(),
  };
  const reportPath = testInfo.outputPath("multi-viewer-report.json");
  fs.writeFileSync(reportPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
  await testInfo.attach("multi-viewer-report", { path: reportPath, contentType: "application/json" });

  await viewerContextA.close();
  await viewerContextB.close();
  await publisherContext.close();
});
