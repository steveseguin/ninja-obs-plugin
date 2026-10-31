// Drive an isolated portable OBS through obs-websocket: publish with a WHEP link key,
// watch the link in real Chrome through the VDO.Ninja player, and cycle the output.
const fs = require("node:fs");
const http = require("node:http");
const path = require("node:path");
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const { chromium } = require("playwright");
const { ObsWebSocketClient } = require("./obs-websocket-vdoninja-source-check.cjs");

const [url, output, cyclesArg, brokerArg] = process.argv.slice(2);
const cycles = Math.max(1, Number(cyclesArg || 2));
// The HTTP side of the broker the OBS host joined: the public service, or a loopback copy under test.
const brokerBase = (brokerArg || "https://whep.vdo.ninja").replace(/\/$/, "");
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const now = () => Date.now();

async function until(fn, label, timeout = 40000, step = 250) {
  const end = now() + timeout;
  while (now() < end) {
    if (await fn()) return;
    await wait(step);
  }
  throw new Error("Timed out: " + label);
}

(async () => {
  const client = new ObsWebSocketClient(url);
  const token = crypto.randomUUID();
  const report = {
    streamId: "whepobs" + crypto.randomBytes(6).toString("hex"),
    whepLink: brokerBase + "/" + token,
    cycles: [],
  };
  let browser, hostProxy;
  // A unique name survives an earlier run that OBS saved before its cleanup ran.
  const inputName = "WHEP review color " + crypto.randomBytes(3).toString("hex");
  try {
    let viewerEndpoint = report.whepLink;
    if (/^http:\/\/127\.0\.0\.1:\d+$/.test(brokerBase)) {
      // The broker selects its per-viewer WHEP handler by Host header, which a browser cannot set for a
      // loopback address, so viewer requests pass through a tiny proxy that presents the public host name.
      const target = new URL(brokerBase);
      hostProxy = http.createServer((req, res) => {
        const upstream = http.request({ host: target.hostname, port: target.port, method: req.method, path: req.url,
          headers: { ...req.headers, host: "whep.vdo.ninja" } }, (up) => { res.writeHead(up.statusCode, up.headers); up.pipe(res); });
        upstream.on("error", () => { res.statusCode = 502; res.end(); });
        req.pipe(upstream);
      });
      await new Promise((r) => hostProxy.listen(0, "127.0.0.1", r));
      viewerEndpoint = `http://127.0.0.1:${hostProxy.address().port}/${token}`;
      report.viewerProxy = viewerEndpoint;
    }
    // The player needs the complete endpoint URL: a bare token posts the offer to vdo.ninja itself.
    report.viewerUrl = "https://vdo.ninja/?whep=" + encodeURIComponent(viewerEndpoint) + "&cleanoutput";
    await client.connect();
    report.obs = await client.request("GetVersion");
    const { inputs } = await client.request("GetInputList");
    for (const input of inputs) await client.request("RemoveInput", { inputName: input.inputName });
    await wait(500);
    const scene = (await client.request("GetCurrentProgramScene")).currentProgramSceneName;
    await client.request("CreateInput", {
      sceneName: scene,
      inputName,
      inputKind: "color_source_v3",
      inputSettings: { color: 0xff2a7fff, width: 1280, height: 720 },
      sceneItemEnabled: true,
    });
    await client.request("SetStreamServiceSettings", {
      streamServiceType: "vdoninja_service",
      streamServiceSettings: {
        stream_id: report.streamId,
        password: crypto.randomBytes(8).toString("hex"),
        whep_token: token,
        max_viewers: 10,
        enable_data_channel: true,
        auto_reconnect: true,
      },
    });
    const applied = await client.request("GetStreamServiceSettings");
    assert.equal(applied.streamServiceType, "vdoninja_service");
    assert.equal(applied.streamServiceSettings.whep_token, token, "service kept the WHEP key");

    const streamActive = async () => (await client.request("GetStreamStatus")).outputActive;
    async function start(label) {
      const begin = now();
      await client.request("StartStream");
      await until(streamActive, label + " active", 40000);
      return now() - begin;
    }
    async function stop(label) {
      const begin = now();
      await client.request("StopStream");
      await until(async () => !(await streamActive()), label + " inactive", 60000);
      return now() - begin;
    }

    browser = await chromium.launch({
      channel: "chrome",
      headless: true,
      args: [
        "--autoplay-policy=no-user-gesture-required",
        // The hosted player must reach a loopback broker during local validation; headless Chrome
        // cannot show the local-network-access permission prompt, so skip those checks here only.
        "--disable-features=LocalNetworkAccessChecks,PrivateNetworkAccessSendPreflights," +
          "PrivateNetworkAccessRespectPreflightResults,BlockInsecurePrivateNetworkRequests",
      ],
    });
    report.browser = browser.version();

    async function watch(cycle) {
      const page = await browser.newPage();
      const consoleLines = [], whepRequests = [];
      page.on("console", (m) => consoleLines.push(m.type() + ": " + m.text().slice(0, 300)));
      page.on("response", (r) => { if (/whep/i.test(r.url())) whepRequests.push(r.request().method() + " " + r.status() + " " + r.url()); });
      await page.addInitScript(() => {
        window.reviewPeers = [];
        window.RTCPeerConnection = new Proxy(window.RTCPeerConnection, {
          construct(target, args) {
            const pc = new target(...args);
            window.reviewPeers.push(pc);
            return pc;
          },
        });
      });
      const begin = now();
      await page.goto(report.viewerUrl, { waitUntil: "domcontentloaded" });
      try {
        await page.waitForFunction(
          () => window.reviewPeers.some((pc) => pc.connectionState === "connected"), null, { timeout: 45000 });
      } catch (error) {
        const peers = await page.evaluate(() => window.reviewPeers.map((pc) => pc.connectionState + "/" + pc.iceConnectionState));
        await page.screenshot({ path: path.join(output, `cycle-${cycle}-viewer-failed.png`) }).catch(() => {});
        fs.writeFileSync(path.join(output, `cycle-${cycle}-viewer-failed.json`),
          JSON.stringify({ peers, whepRequests, console: consoleLines.slice(-60) }, null, 2));
        throw error;
      }
      const connectedMs = now() - begin;
      async function stats() {
        return page.evaluate(async () => {
          const pc = window.reviewPeers.find((p) => p.connectionState === "connected");
          const entries = [...(await pc.getStats()).values()];
          const video = entries.find((s) => s.type === "inbound-rtp" && s.kind === "video");
          const audio = entries.find((s) => s.type === "inbound-rtp" && s.kind === "audio");
          const pair = entries.find((s) => s.type === "candidate-pair" && s.state === "succeeded" && s.nominated);
          const codec = (s) => (s && entries.find((c) => c.id === s.codecId)) || null;
          const element = [...document.querySelectorAll("video")].find((v) => v.videoWidth > 0);
          return {
            framesDecoded: video ? video.framesDecoded : 0,
            frameWidth: video ? video.frameWidth : 0,
            frameHeight: video ? video.frameHeight : 0,
            videoPackets: video ? video.packetsReceived : 0,
            audioPackets: audio ? audio.packetsReceived : 0,
            videoCodec: codec(video) && codec(video).mimeType,
            audioCodec: codec(audio) && codec(audio).mimeType,
            sdpFmtp: codec(video) && codec(video).sdpFmtpLine,
            candidateTypes: pair ? [
              (entries.find((c) => c.id === pair.localCandidateId) || {}).candidateType,
              (entries.find((c) => c.id === pair.remoteCandidateId) || {}).candidateType] : null,
            elementSize: element ? [element.videoWidth, element.videoHeight] : null,
            currentTime: element ? element.currentTime : null,
          };
        });
      }
      await until(async () => (await stats()).framesDecoded >= 15, "decoded frames", 30000, 500);
      const first = await stats();
      await wait(3000);
      const second = await stats();
      await page.screenshot({ path: path.join(output, `cycle-${cycle}-viewer.png`) });
      const result = {
        connectedMs,
        firstFramesMs: now() - begin,
        ...second,
        framesDecodedDelta3s: second.framesDecoded - first.framesDecoded,
        currentTimeAdvanced: first.currentTime !== null && second.currentTime > first.currentTime,
      };
      assert(result.framesDecodedDelta3s >= 30, "viewer keeps decoding frames: " + JSON.stringify(result));
      assert(result.elementSize && result.elementSize[0] > 0, "viewer renders video");
      assert.equal(result.videoCodec, "video/H264", "WHEP viewer negotiated H.264");
      assert(result.audioPackets > 50, "viewer receives audio packets");
      return { page, result };
    }

    for (let cycle = 1; cycle <= cycles; ++cycle) {
      const entry = { cycle };
      entry.startMs = await start(`cycle ${cycle}`);
      await wait(3000); // The broker connection follows the output start.
      const { page, result } = await watch(cycle);
      entry.viewer = result;
      entry.stopMs = await stop(`cycle ${cycle}`);
      // A stopped output must drop its WHEP viewers; the player may begin retrying.
      await until(() => page.evaluate(() => !window.reviewPeers.some((pc) => pc.connectionState === "connected")),
        "viewer disconnected after stop", 20000).then(() => { entry.viewerDroppedAfterStop = true; },
        () => { entry.viewerDroppedAfterStop = false; });
      await page.close();
      report.cycles.push(entry);
      assert(entry.stopMs < 20000, `stop must not stall (${entry.stopMs} ms)`);
      if (cycle === 1) {
        // Stop while the start thread may still be connecting; the output must settle quickly.
        const begin = now();
        await client.request("StartStream");
        await wait(300);
        await client.request("StopStream");
        await until(async () => !(await streamActive()), "immediate stop inactive", 60000);
        report.immediateStopMs = now() - begin;
        assert(report.immediateStopMs < 30000, `immediate stop stalled (${report.immediateStopMs} ms)`);
        await wait(1000);
      }
    }
    await client.request("RemoveInput", { inputName });
    report.ok = true;
  } catch (error) {
    report.error = String(error.stack || error);
    throw error;
  } finally {
    try {
      if ((await client.request("GetStreamStatus")).outputActive) await client.request("StopStream");
    } catch (_) {}
    if (!report.ok) await client.request("RemoveInput", { inputName }).catch(() => {});
    if (browser) await browser.close();
    if (hostProxy) await new Promise((r) => hostProxy.close(r));
    fs.writeFileSync(path.join(output, "whep-workflow.json"), JSON.stringify(report, null, 2));
    await client.close();
  }
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
