const { chromium } = require("playwright");

async function run() {
  const url = process.argv[2] || "https://vdo.ninja/?view=CoatdevdavER";

  const browser = await chromium.launch({
    headless: process.env.HEADLESS === "0" ? false : true,
    args: ["--autoplay-policy=no-user-gesture-required"],
  });
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

  await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
  await page.waitForTimeout(25000);
  const viewport = page.viewportSize() || { width: 1280, height: 720 };
  await page.mouse.click(Math.floor(viewport.width / 2), Math.floor(viewport.height / 2));
  await page.waitForTimeout(12000);

  const result = await page.evaluate(async () => {
    const waitingText = document.body ? document.body.innerText || "" : "";
    const videos = Array.from(document.querySelectorAll("video")).map((v, index) => {
      const s = v.srcObject;
      const audioTracks = s && s.getAudioTracks ? s.getAudioTracks().length : 0;
      const videoTracks = s && s.getVideoTracks ? s.getVideoTracks().length : 0;
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
        } catch (e) {
          pcStats.push({ error: String(e) });
        }
      }
    }

    return {
      url: location.href,
      containsWaitingText: /Waiting for the stream/i.test(waitingText),
      textSample: waitingText.slice(0, 400),
      videoElements: videos,
      pcStats,
    };
  });

  await page.screenshot({ path: "playwright-vdo-view-check.png", fullPage: true });
  console.log(JSON.stringify(result, null, 2));
  await browser.close();
}

run().catch((err) => {
  console.error(err);
  process.exit(1);
});
