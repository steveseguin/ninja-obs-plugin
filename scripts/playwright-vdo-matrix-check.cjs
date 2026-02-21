const fs = require("fs");
const path = require("path");
const { chromium } = require("playwright");

function sanitizeForFile(value, index) {
  const cleaned = value.replace(/[^a-zA-Z0-9_-]+/g, "_").slice(0, 80);
  return `${String(index).padStart(2, "0")}_${cleaned || "url"}`;
}

async function collectSample(page) {
  return page.evaluate(async () => {
    const waitingText = document.body ? document.body.innerText || "" : "";
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
          });
          pcStats.push({
            state: pc.connectionState,
            inboundVideoBytes,
            inboundAudioBytes,
            framesDecoded,
          });
        } catch (error) {
          pcStats.push({ error: String(error) });
        }
      }
    }

    return {
      url: location.href,
      containsWaitingText: /Waiting for the stream/i.test(waitingText),
      textSample: waitingText.slice(0, 400),
      videoElements: videos,
      pcStats,
      timestamp: Date.now(),
    };
  });
}

async function runSingleUrl(browser, url, index, perUrlWaitMs, sampleGapMs, outputDir) {
  const context = await browser.newContext();
  const page = await context.newPage();

  await page.addInitScript(() => {
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

  const result = {
    inputUrl: url,
    gotoOk: false,
    gotoError: "",
    sample1: null,
    sample2: null,
    playbackAdvanced: false,
    hasInboundBytes: false,
    hasVideoMetadata: false,
    hasAudioTrack: false,
    hasVideoTrack: false,
    verdictMediaActive: false,
    screenshotPath: "",
  };

  try {
    await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
    result.gotoOk = true;
  } catch (error) {
    result.gotoError = String(error);
  }

  try {
    const viewport = page.viewportSize() || { width: 1280, height: 720 };
    await page.waitForTimeout(Math.max(2000, Math.floor(perUrlWaitMs / 2)));
    await page.mouse.click(Math.floor(viewport.width / 2), Math.floor(viewport.height / 2));
    await page.waitForTimeout(Math.max(2000, Math.floor(perUrlWaitMs / 2)));

    result.sample1 = await collectSample(page);
    await page.waitForTimeout(sampleGapMs);
    result.sample2 = await collectSample(page);

    result.playbackAdvanced = result.sample2.videoElements.some((v2) => {
      const v1 = result.sample1.videoElements.find((x) => x.index === v2.index);
      return v1 && v2.currentTime > v1.currentTime + 0.4;
    });

    result.hasVideoMetadata = result.sample2.videoElements.some((v) => v.videoWidth > 0 && v.videoHeight > 0);
    result.hasAudioTrack = result.sample2.videoElements.some((v) => v.audioTracks > 0);
    result.hasVideoTrack = result.sample2.videoElements.some((v) => v.videoTracks > 0);
    result.hasInboundBytes = result.sample2.pcStats.some(
      (s) => (s.inboundVideoBytes || 0) > 0 || (s.inboundAudioBytes || 0) > 0 || (s.framesDecoded || 0) > 0
    );

    result.verdictMediaActive =
      result.playbackAdvanced ||
      (result.hasInboundBytes && result.hasVideoTrack) ||
      (result.hasInboundBytes && result.hasAudioTrack);
  } catch (error) {
    result.sampleError = String(error);
  }

  const shotName = `${sanitizeForFile(new URL(url).search, index)}.png`;
  const shotPath = path.join(outputDir, shotName);
  try {
    await page.screenshot({ path: shotPath, fullPage: true });
    result.screenshotPath = shotPath;
  } catch (error) {
    result.screenshotError = String(error);
  }

  await context.close();
  return result;
}

async function main() {
  const urls = process.argv.slice(2);
  const targets =
    urls.length > 0
      ? urls
      : [
          "https://vdo.ninja/?view=Alsosuitbc&pasword=somepassword",
          "https://vdo.ninja/?view=Alsosuitbc&password=somepassword",
          "https://vdo.ninja/?view=CoatdevdavER",
        ];

  const perUrlWaitMs = Number(process.env.PER_URL_WAIT_MS || 18000);
  const sampleGapMs = Number(process.env.SAMPLE_GAP_MS || 7000);

  const outputDir = path.join("test-results", "playwright-vdo-matrix");
  fs.mkdirSync(outputDir, { recursive: true });

  const browser = await chromium.launch({
    headless: process.env.HEADLESS === "0" ? false : true,
    args: ["--autoplay-policy=no-user-gesture-required"],
  });

  const startedAt = new Date().toISOString();
  const results = [];
  for (let i = 0; i < targets.length; i += 1) {
    const url = targets[i];
    const single = await runSingleUrl(browser, url, i + 1, perUrlWaitMs, sampleGapMs, outputDir);
    results.push(single);
  }
  await browser.close();

  const summary = {
    startedAt,
    finishedAt: new Date().toISOString(),
    total: results.length,
    mediaActiveCount: results.filter((r) => r.verdictMediaActive).length,
    anyMediaActive: results.some((r) => r.verdictMediaActive),
  };

  const payload = { summary, results };
  const outPath = "playwright-vdo-matrix-check.json";
  fs.writeFileSync(outPath, `${JSON.stringify(payload, null, 2)}\n`, "utf8");
  console.log(JSON.stringify(payload, null, 2));
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});

