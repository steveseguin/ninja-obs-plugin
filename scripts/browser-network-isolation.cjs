// Opt-in browser publisher/viewer control against the same private TURN relay.
// Synthetic Y4M input only. Reports unsupported browser probes explicitly.
const fs = require("node:fs");
const http = require("node:http");
const path = require("node:path");
const { chromium, firefox } = require("playwright");
const {
  startPresentationCapture,
  stopPresentationCapture,
} = require("./obs-websocket-vdoninja-publish-check.cjs");
const {
  analyzePresentationContinuity,
  analyzeVisualSequence,
} = require("../tests/tools/presentation-continuity-analysis.cjs");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const {
  installProbes,
  stats,
} = require("../tests/tools/rtc-timing-probes.cjs");
async function main() {
  const type = process.env.ISOLATION_BROWSER || "chromium";
  const mode = process.env.ISOLATION_MODE || "vdo";
  if (!["vdo", "direct"].includes(mode))
    throw new Error("Unknown ISOLATION_MODE");
  if (!["chromium", "firefox"].includes(type))
    throw new Error("ISOLATION_BROWSER must be chromium or firefox");
  const source = path.resolve(
    process.env.ISOLATION_Y4M || "/tmp/ninja-counter720p30.y4m",
  );
  fs.accessSync(source);
  const output = path.resolve(
    process.env.ISOLATION_OUTPUT || `artifacts/browser-isolation/${Date.now()}`,
  );
  fs.mkdirSync(output, { recursive: true });
  const buffer = Number(process.env.ISOLATION_BUFFER_MS || 300);
  const fixedBuffer =
    process.env.ISOLATION_FIXED_BUFFER === "1" ? buffer : null;
  const duration = Number(process.env.ISOLATION_DURATION_MS || 30000);
  const warmup = Number(process.env.ISOLATION_WARMUP_MS || 10000);
  if (
    ![buffer, duration, warmup].every(Number.isFinite) ||
    buffer < 0 ||
    buffer > 4000 ||
    duration <= 0 ||
    warmup < 0
  )
    throw new Error("Invalid duration or buffer target");
  const iceServers = JSON.parse(
    process.env.ISOLATION_ICE_JSON ||
      '[{"urls":"turn:172.17.0.2:3478","username":"e2e","credential":"syntheticTest42"}]',
  );
  const id = `BrowserIsolation${Date.now()}`;
  const base = "https://vdo.ninja/alpha/";
  const push = `${base}?push=${id}&password=false&autostart&webcam&codec=h264&bitrate=3000&maxvideobitrate=3000&width=1280&height=720&fps=30&relay`;
  const view = `${base}?view=${id}&password=false&cleanoutput&codec=h264&buffer=${buffer}&relay`;
  const report = {
    id,
    type,
    mode,
    source,
    push,
    view,
    buffer,
    fixedBuffer,
    duration,
    warmup,
    samples: [],
    errors: [],
  };
  let publisher, viewer, pubPage, viewPage, server;
  try {
    publisher = await chromium.launch({
      headless: true,
      args: [
        "--autoplay-policy=no-user-gesture-required",
        "--use-fake-ui-for-media-stream",
        "--use-fake-device-for-media-stream",
        `--use-file-for-fake-video-capture=${source}`,
      ],
    });
    viewer = await { chromium, firefox }[type].launch({
      headless: true,
      ...(type === "chromium"
        ? { args: ["--autoplay-policy=no-user-gesture-required"] }
        : { firefoxUserPrefs: { "media.autoplay.default": 0 } }),
    });
    const pubContext = await publisher.newContext({
      permissions: ["camera", "microphone"],
    });
    const viewContext = await viewer.newContext();
    await installProbes(pubContext, iceServers, null);
    await installProbes(viewContext, iceServers, fixedBuffer);
    pubPage = await pubContext.newPage();
    viewPage = await viewContext.newPage();
    const capabilities = await viewPage.evaluate(
      () => RTCRtpReceiver.getCapabilities("video").codecs,
    );
    report.viewerCodecs = capabilities;
    if (!capabilities.some((c) => c.mimeType.toLowerCase() === "video/h264"))
      throw new Error(
        "H264 is unavailable in the viewer; install OpenH264 before this codec-matched comparison",
      );
    report.console = [];
    for (const [name, page] of [
      ["publisher", pubPage],
      ["viewer", viewPage],
    ])
      page.on("console", (message) => {
        if (report.console.length < 2000)
          report.console.push({ name, text: message.text() });
      });
    for (const [name, page] of [
      ["publisher", pubPage],
      ["viewer", viewPage],
    ])
      page.on("pageerror", (error) =>
        report.errors.push({ name, error: String(error) }),
      );
    if (mode === "direct") {
      server = http.createServer((request, response) => {
        response.setHeader("Content-Type", "text/html");
        response.end(
          '<video autoplay muted playsinline style="width:100%;height:100%"></video>',
        );
      });
      await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
      const url = `http://127.0.0.1:${server.address().port}`;
      await pubPage.goto(url);
      await viewPage.goto(url);
      await pubPage.evaluate(
        async (startBitrate) => {
          const pc = new RTCPeerConnection();
          window.controlPC = pc;
          const stream = await navigator.mediaDevices.getUserMedia({
            video: { width: 1280, height: 720, frameRate: 30 },
            audio: true,
          });
          const video = document.querySelector("video");
          video.srcObject = stream;
          await video.play();
          for (const track of stream.getTracks()) pc.addTrack(track, stream);
          for (const t of pc.getTransceivers()) {
            if (t.sender.track.kind === "video") {
              const codecs = RTCRtpSender.getCapabilities(
                "video",
              ).codecs.filter((c) => c.mimeType.toLowerCase() === "video/h264");
              if (!codecs.length) throw new Error("H264 unavailable");
              t.setCodecPreferences(codecs);
              const params = t.sender.getParameters();
              params.encodings[0].maxBitrate = 3000000;
              await t.sender.setParameters(params);
            }
          }
          const offer = await pc.createOffer();
          if (startBitrate)
            offer.sdp = offer.sdp.replace(
              /^(a=fmtp:\d+ .*profile-level-id=.*)$/gm,
              (line) =>
                line.trimEnd() +
                `;x-google-start-bitrate=${startBitrate};x-google-min-bitrate=${startBitrate};x-google-max-bitrate=${startBitrate}\r`,
            );
          await pc.setLocalDescription(offer);
          if (pc.iceGatheringState !== "complete")
            await new Promise((resolve, reject) => {
              const timer = setTimeout(
                () => reject(new Error("ICE gathering timed out")),
                30000,
              );
              pc.addEventListener("icegatheringstatechange", () => {
                if (pc.iceGatheringState === "complete") {
                  clearTimeout(timer);
                  resolve();
                }
              });
            });
        },
        Number(process.env.ISOLATION_DIRECT_START_KBPS || 0),
      );
      const offer = await pubPage.evaluate(() =>
        window.controlPC.localDescription.toJSON(),
      );
      const answer = await viewPage.evaluate(async (offer) => {
        const pc = new RTCPeerConnection();
        window.controlPC = pc;
        pc.addEventListener("track", (event) => {
          const video = document.querySelector("video");
          video.srcObject = event.streams[0];
          video.play();
        });
        await pc.setRemoteDescription(offer);
        await pc.setLocalDescription(await pc.createAnswer());
        if (pc.iceGatheringState !== "complete")
          await new Promise((resolve, reject) => {
            const timer = setTimeout(
              () => reject(new Error("ICE gathering timed out")),
              30000,
            );
            pc.addEventListener("icegatheringstatechange", () => {
              if (pc.iceGatheringState === "complete") {
                clearTimeout(timer);
                resolve();
              }
            });
          });
        return pc.localDescription.toJSON();
      }, offer);
      await pubPage.evaluate(
        (answer) => window.controlPC.setRemoteDescription(answer),
        answer,
      );
    } else {
      await pubPage.goto(push, { waitUntil: "domcontentloaded" });
      await pubPage.mouse.click(640, 360);
      const button = pubPage.getByRole("button", {
        name: /share your camera/i,
      });
      if (await button.isVisible().catch(() => false))
        await button.click({ timeout: 1500 }).catch(() => {});
      await viewPage.goto(view, { waitUntil: "domcontentloaded" });
      await viewPage.mouse.click(640, 360);
    }
    await viewPage.waitForFunction(
      () =>
        Array.from(document.querySelectorAll("video")).some(
          (v) => v.srcObject && v.videoWidth > 0 && !v.paused,
        ),
      null,
      { timeout: 60000 },
    );
    console.log("Playable; warming up", warmup);
    await sleep(warmup);
    const decodedProbe = await viewPage.evaluate(
      () => typeof MediaStreamTrackProcessor === "function",
    );
    report.decodedProbe = decodedProbe
      ? "MediaStreamTrackProcessor"
      : "unavailable";
    await startPresentationCapture(
      viewPage,
      decodedProbe,
      "counter-complement",
    );
    const until = Date.now() + duration;
    do {
      report.samples.push({
        publisher: await stats(pubPage),
        viewer: await stats(viewPage),
      });
      await sleep(Math.min(1000, Math.max(0, until - Date.now())));
    } while (Date.now() < until);
    report.samples.push({
      publisher: await stats(pubPage),
      viewer: await stats(viewPage),
    });
    const capture = await stopPresentationCapture(viewPage);
    fs.writeFileSync(
      path.join(output, "presentation.json"),
      JSON.stringify(capture),
    );
    for (const [name, page] of [
      ["publisher", pubPage],
      ["viewer", viewPage],
    ]) {
      const trace = await page.evaluate(() => ({
        records: window.__encodedTiming,
        overflow: window.__encodedOverflow,
        capabilities: window.__timingCapabilities,
        error: window.__encodedError || "",
        timeOrigin: performance.timeOrigin,
      }));
      fs.writeFileSync(
        path.join(output, `${name}-encoded.json`),
        JSON.stringify(trace),
      );
      if (trace.error || trace.overflow)
        throw new Error(
          `${name} timing probe failed: ${trace.error || "overflow"}`,
        );
    }
    const finalPcs = report.samples.at(-1).viewer.pcs;
    report.negotiatedVideoCodecs = finalPcs.flatMap((p) =>
      p.stats
        .filter(
          (s) =>
            s.type === "inbound-rtp" && (s.kind || s.mediaType) === "video",
        )
        .map((s) => p.stats.find((c) => c.id === s.codecId)?.mimeType),
    );
    if (
      !report.negotiatedVideoCodecs.length ||
      report.negotiatedVideoCodecs.some(
        (codec) => codec?.toLowerCase() !== "video/h264",
      )
    )
      throw new Error("The control did not negotiate H264");
    report.presentation = analyzePresentationContinuity(capture.records, {
      expectedFps: 30,
    });
    report.visual = decodedProbe
      ? analyzeVisualSequence(capture.decodedRecords)
      : null;
    report.ok =
      report.presentation.ok && (report.visual === null || report.visual.ok);
    console.log(
      JSON.stringify({
        ok: report.ok,
        presentation: report.presentation,
        visual: report.visual,
      }),
    );
    if (!report.ok) process.exitCode = 1;
  } catch (error) {
    report.error = String(error.stack || error);
    report.ok = false;
    process.exitCode = 1;
    console.error(report.error);
  } finally {
    for (const [name, page] of [
      ["publisher", pubPage],
      ["viewer", viewPage],
    ]) {
      if (page && !page.isClosed()) {
        report[`${name}Final`] = await stats(page).catch((error) => ({
          error: String(error),
        }));
        report[`${name}DOM`] = await page
          .evaluate(() => ({
            text: document.body.innerText.slice(0, 1000),
            videos: Array.from(document.querySelectorAll("video")).map((v) => ({
              width: v.videoWidth,
              height: v.videoHeight,
              paused: v.paused,
              tracks: v.srcObject
                ?.getTracks()
                .map((t) => ({
                  kind: t.kind,
                  ready: t.readyState,
                  settings: t.getSettings(),
                })),
            })),
            encodedError: window.__encodedError,
            encodedCount: window.__encodedTiming?.length,
          }))
          .catch(() => null);
      }
    }
    if (viewer) await viewer.close();
    if (publisher) await publisher.close();
    fs.writeFileSync(
      path.join(output, "report.json"),
      JSON.stringify(report, null, 2),
    );
    if (server) await new Promise((resolve) => server.close(resolve));
  }
}
if (require.main === module)
  main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
module.exports = { installProbes, stats };
