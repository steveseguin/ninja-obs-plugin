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
          let framesDecoded = 0;

          stats.forEach((s) => {
            if (s.type === "inbound-rtp" && !s.isRemote) {
              if (s.kind === "video") {
                inboundVideoBytes += s.bytesReceived || 0;
                framesDecoded += s.framesDecoded || 0;
              }
              if (s.kind === "audio") {
                inboundAudioBytes += s.bytesReceived || 0;
              }
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
            framesDecoded,
          });
        } catch (error) {
          pcStats.push({ error: String(error) });
        }
      }
    }

    return {
      url: location.href,
      title: document.title || "",
      textSample: (document.body ? document.body.innerText || "" : "").slice(0, 240),
      videos,
      pcStats,
      timestamp: Date.now(),
    };
  });
}

function sumInbound(snapshot) {
  return snapshot.pcStats.reduce(
    (acc, s) => acc + (s.inboundVideoBytes || 0) + (s.inboundAudioBytes || 0),
    0
  );
}

function sumOutbound(snapshot) {
  return snapshot.pcStats.reduce(
    (acc, s) => acc + (s.outboundVideoBytes || 0) + (s.outboundAudioBytes || 0),
    0
  );
}

function hasTracks(snapshot) {
  return snapshot.videos.some((v) => v.videoTracks > 0 || v.audioTracks > 0);
}

function hasVideoMetadata(snapshot) {
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

test("vdo.ninja publish -> view -> reload keeps active media", async ({ browser }, testInfo) => {
  test.setTimeout(240000);

  const { streamId, noPassword, password, roomId, bitrate, includeRoomInView, includeSceneInView, pushUrl, viewUrl } =
    buildScenarioUrls();

  const publisherContext = await browser.newContext({
    permissions: ["camera", "microphone"],
  });
  const viewerContext = await browser.newContext();

  const publisher = await publisherContext.newPage();
  const viewer = await viewerContext.newPage();

  await publisher.addInitScript(installPeerCollectorScript());
  await viewer.addInitScript(installPeerCollectorScript());

  await publisher.goto(pushUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
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

  await viewer.goto(viewUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  await nudgePage(viewer);

  await expect
    .poll(
      async () => {
        const snap = await collectSnapshot(viewer);
        return hasTracks(snap) && sumInbound(snap) > 5000;
      },
      { timeout: 70000, intervals: [1000, 2000, 3000] }
    )
    .toBeTruthy();

  const publisherBefore = await collectSnapshot(publisher);
  const viewerBefore = await collectSnapshot(viewer);
  await viewer.waitForTimeout(7000);
  const viewerAfter = await collectSnapshot(viewer);

  expect(hasTracks(viewerAfter)).toBeTruthy();
  expect(hasVideoMetadata(viewerAfter)).toBeTruthy();
  expect(sumInbound(viewerAfter)).toBeGreaterThan(5000);
  expect(playbackAdvanced(viewerBefore, viewerAfter)).toBeTruthy();

  const inboundBeforeReload = sumInbound(viewerAfter);

  await viewer.reload({ waitUntil: "domcontentloaded", timeout: 60000 });
  await nudgePage(viewer);

  await expect
    .poll(
      async () => {
        const snap = await collectSnapshot(viewer);
        return hasTracks(snap) && sumInbound(snap) > 5000;
      },
      { timeout: 70000, intervals: [1000, 2000, 3000] }
    )
    .toBeTruthy();

  const viewerAfterReload1 = await collectSnapshot(viewer);
  await viewer.waitForTimeout(7000);
  const viewerAfterReload2 = await collectSnapshot(viewer);

  expect(hasTracks(viewerAfterReload2)).toBeTruthy();
  expect(hasVideoMetadata(viewerAfterReload2)).toBeTruthy();
  expect(sumInbound(viewerAfterReload2)).toBeGreaterThan(5000);
  expect(playbackAdvanced(viewerAfterReload1, viewerAfterReload2)).toBeTruthy();

  const publisherAfter = await collectSnapshot(publisher);
  expect(publisherAfter.pcStats.length).toBeGreaterThan(0);

  const report = {
    streamId,
    noPassword,
    password,
    roomId,
    bitrate,
    includeRoomInView,
    includeSceneInView,
    pushUrl,
    viewUrl,
    inboundBeforeReload,
    inboundAfterReload: sumInbound(viewerAfterReload2),
    outboundAtPublisher: sumOutbound(publisherAfter),
    publisherBefore,
    publisherAfter,
    viewerBefore,
    viewerAfter,
    viewerAfterReload1,
    viewerAfterReload2,
  };

  const reportPath = testInfo.outputPath("report.json");
  fs.writeFileSync(reportPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
  await testInfo.attach("e2e-report", { path: reportPath, contentType: "application/json" });

  await publisher.screenshot({ path: testInfo.outputPath("publisher.png"), fullPage: true });
  await viewer.screenshot({ path: testInfo.outputPath("viewer.png"), fullPage: true });

  await viewerContext.close();
  await publisherContext.close();
});
