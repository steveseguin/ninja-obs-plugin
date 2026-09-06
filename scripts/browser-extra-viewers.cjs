// Additional measured browser viewers for an already-running synthetic publisher.
// Usage: VDONINJA_VIEWER_ICE_SERVERS_JSON='[...]' node scripts/browser-extra-viewers.cjs URL OUTPUT
const fs = require("node:fs");
const path = require("node:path");
const { chromium, firefox } = require("playwright");
const { installProbes } = require("../tests/tools/rtc-timing-probes.cjs");
const { startPresentationCapture, stopPresentationCapture, collectViewerSnapshot } =
  require("./obs-websocket-vdoninja-publish-check.cjs");
const { analyzeVideoContinuity } = require("../tests/tools/video-continuity-analysis.cjs");
const { analyzePresentationContinuity, analyzeVisualSequence } =
  require("../tests/tools/presentation-continuity-analysis.cjs");
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function main() {
  const [url, output] = process.argv.slice(2);
  if (!url || !output) throw new Error("Specify the synthetic viewer URL and output directory");
  const ice = JSON.parse(process.env.VDONINJA_VIEWER_ICE_SERVERS_JSON || "null");
  if (!Array.isArray(ice) || !ice.length) throw new Error("Explicit private relay ICE servers are required");
  const count = Number(process.env.EXTRA_VIEWER_COUNT || 2);
  const expectedFps = Number(process.env.EXTRA_VIEWER_FPS || 30);
  const durationMs = Number(process.env.EXTRA_VIEWER_DURATION_MS || 20000);
  const reloads = Number(process.env.EXTRA_VIEWER_RELOADS || 0);
  if (!Number.isInteger(count) || count < 1 || count > 8 ||
      !Number.isInteger(reloads) || reloads < 0 || reloads > 5 ||
      !Number.isFinite(expectedFps) || expectedFps <= 0 ||
      !Number.isFinite(durationMs) || durationMs <= 0) throw new Error("Invalid viewer count, FPS or duration");
  fs.mkdirSync(output, { recursive: true });
  const viewerBrowser = process.env.VDONINJA_VIEWER_BROWSER || "chromium";
  const fieldTrials = process.env.VDONINJA_CHROMIUM_FIELD_TRIALS || "";
  const browserExecutable = process.env.VDONINJA_BROWSER_EXECUTABLE || null;
  const requirePixels = process.env.VDONINJA_REQUIRE_VISUAL_SEQUENCE !== "0";
  if (!["chromium", "firefox"].includes(viewerBrowser)) throw new Error("Unknown viewer browser");
  if (fieldTrials && viewerBrowser !== "chromium") throw new Error("Chromium field trials require Chromium");
  const report = { url, count, expectedFps, durationMs, reloads, viewerBrowser, fieldTrials,
    browserExecutable, requirePixels, viewers: [], ok: false };
  let browser;
  try {
    browser = await ({ chromium, firefox }[viewerBrowser]).launch({
      ...(browserExecutable ? { executablePath: browserExecutable } : {}),
      ...(browserExecutable && viewerBrowser === "chromium" ? {
        env: { ...process.env, CHROME_LOG_FILE: path.join(output, "chromium-process.log") },
      } : {}),
      ...(viewerBrowser === "chromium" ? { args: ["--autoplay-policy=no-user-gesture-required",
        ...(browserExecutable ? ["--enable-logging"] : []),
        ...(fieldTrials ? [`--force-fieldtrials=${fieldTrials}`] : [])] }
        : { firefoxUserPrefs: { "media.autoplay.default": 0 } }),
    });
    report.browserVersion = browser.version();
    const context = await browser.newContext();
    await installProbes(context, ice, null);
    const pages = await Promise.all(Array.from({ length: count }, async (_, index) => {
      const page = await context.newPage();
      page.on("crash", () => {
        report.error ||= `Viewer ${index} renderer crashed`;
        // A getStats() promise in another page may otherwise never settle.
        browser.close().catch(() => {});
      });
      await page.goto(url, { waitUntil: "domcontentloaded" });
      await page.mouse.click(320, 180);
      return page;
    }));
    for (let round = 0; round <= reloads; round++) {
      if (round) await Promise.all(pages.map(page => page.reload({ waitUntil: "domcontentloaded" })));
      const readyDeadline = Date.now() + 60000;
      while (true) {
        const states = await Promise.all(pages.map(collectViewerSnapshot));
        if (states.every(state => (state.pcStats || []).reduce(
          (total, pc) => total + (Number(pc.framesDecoded) || 0), 0) >= expectedFps)) break;
        if (Date.now() >= readyDeadline) throw new Error("Additional viewers did not decode live video");
        await sleep(250);
      }
      await sleep(20000);
      await Promise.all(pages.map(page => startPresentationCapture(page, requirePixels, "counter-complement")));
      const samples = pages.map(() => []);
      const deadline = Date.now() + durationMs;
      do {
        const states = await Promise.all(pages.map(collectViewerSnapshot));
        states.forEach((state, index) => samples[index].push(state));
        await sleep(Math.min(1000, Math.max(0, deadline - Date.now())));
      } while (Date.now() < deadline);
      const last = await Promise.all(pages.map(collectViewerSnapshot));
      last.forEach((state, index) => samples[index].push(state));
      const codecs = await Promise.all(pages.map(page => page.evaluate(async () => {
        const result = [];
        for (const pc of window.__pcList || []) {
          const stats = await pc.getStats();
          for (const s of stats.values()) {
            if (s.type === "inbound-rtp" && s.kind === "video") {
              const codec = stats.get(s.codecId);
              if (codec) result.push(codec.mimeType);
            }
          }
        }
        return result;
      })));
      const captures = await Promise.all(pages.map(stopPresentationCapture));
      report.viewers.push(...captures.map((capture, index) => {
        const video = analyzeVideoContinuity(samples[index], { expectedFps,
          maximumLostPackets: Number(process.env.VDONINJA_MAX_LOST_VIDEO_PACKETS || 0) });
        const presentation = analyzePresentationContinuity(capture.records, { expectedFps, requireMarker: false });
        const pixels = capture.decodedRecords.length ? analyzeVisualSequence(capture.decodedRecords) : null;
        const pixelCoverage = pixels ? "measured" : "unavailable";
        const relayVerified = (last[index].pcStats || []).some(pc =>
          (pc.selectedCandidatePairs || []).some(pair =>
            pair.localCandidateType === "relay" || pair.remoteCandidateType === "relay"));
        const expectedCodec = process.env.VDONINJA_EXPECT_VIDEO_CODEC;
        const codecVerified = !expectedCodec || (codecs[index].length > 0 &&
          codecs[index].every(codec => codec.toLowerCase() === expectedCodec.toLowerCase()));
        return { index, round, relayVerified, codecVerified, codecs: codecs[index], samples: samples[index], capture, video, presentation, pixels, pixelCoverage,
          ok: relayVerified && codecVerified && video.ok && presentation.ok && (!requirePixels || Boolean(pixels && pixels.ok)) };
      }));
    }
    report.ok = report.viewers.every(viewer => viewer.ok);
    if (!report.ok) process.exitCode = 1;
  } catch (error) {
    report.error ||= String(error.stack || error);
    process.exitCode = 1;
  } finally {
    if (browser) await browser.close();
    fs.writeFileSync(path.join(output, "extra-viewers.json"), JSON.stringify(report, null, 2));
  }
  console.log(JSON.stringify({ ok: report.ok, count, output, error: report.error }));
}
main().catch(error => { console.error(error); process.exitCode = 1; });
