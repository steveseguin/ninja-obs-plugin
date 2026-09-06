// Minimal loopback WebRTC receiver experiment; no OBS, VDO.Ninja or relay.
// Synthetic encoded-frame delay creates arrival variation. The diagnostic
// Chromium patch can force timing-sample exclusion without faking actual NACKs.
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { chromium } = require('playwright');
const { startPresentationCapture, stopPresentationCapture, collectViewerSnapshot } =
  require('./obs-websocket-vdoninja-publish-check.cjs');
const { analyzePresentationContinuity, analyzeVisualSequence } =
  require('../tests/tools/presentation-continuity-analysis.cjs');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function main() {
  const output = path.resolve(process.argv[2] || 'artifacts/minimal-receiver');
  fs.mkdirSync(output, { recursive: true });
  const server = http.createServer((req, res) => { res.setHeader('Content-Type', 'text/html');
    res.end('<!doctype html><canvas width="640" height="360"></canvas><video autoplay muted playsinline></video>'); });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const fieldTrials = process.env.VDONINJA_CHROMIUM_FIELD_TRIALS || '';
  const executable = process.env.VDONINJA_BROWSER_EXECUTABLE;
  const logFile = path.join(output, 'chromium.log');
  const report = { ok: false, fieldTrials, executable: executable || null, logFile,
    syntheticTimingExclusionRequested: fieldTrials.includes('WebRTC-ForceTimingSampleExclusion/Enabled/'),
    note: 'Loopback with synthetic encoded-frame delay; does not simulate network NACK feedback.' };
  let browser;
  try {
    browser = await chromium.launch({ ...(executable ? { executablePath: executable } : {}),
      env: { ...process.env, CHROME_LOG_FILE: logFile },
      args: ['--autoplay-policy=no-user-gesture-required', '--enable-logging', `--log-file=${logFile}`,
        ...(fieldTrials ? [`--force-fieldtrials=${fieldTrials}`] : [])] });
    report.browserVersion = browser.version();
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${server.address().port}`);
    report.setup = await page.evaluate(async () => {
      const canvas = document.querySelector('canvas'), ctx = canvas.getContext('2d');
      if (typeof MediaStreamTrackGenerator !== 'function') throw new Error('Video track generator is required');
      const track = new MediaStreamTrackGenerator({ kind:'video' });
      const writer = track.writable.getWriter(), media = new MediaStream([track]);
      const send = new RTCPeerConnection({ encodedInsertableStreams: true });
      const receive = new RTCPeerConnection();
      window.__pcList = [receive]; window.__minimalPeers = [send, receive];
      send.onicecandidate = e => { if (e.candidate) receive.addIceCandidate(e.candidate).catch(console.error); };
      receive.onicecandidate = e => { if (e.candidate) send.addIceCandidate(e.candidate).catch(console.error); };
      receive.ontrack = e => { document.querySelector('video').srcObject = e.streams[0];
        e.receiver.playoutDelayHint = 0.3; e.receiver.jitterBufferTarget = 300; };
      const sender = send.addTrack(track, media);
      const codecs = RTCRtpSender.getCapabilities('video').codecs.filter(c => c.mimeType.toLowerCase() === 'video/h264');
      if (!codecs.length) throw new Error('H264 is required');
      send.getTransceivers()[0].setCodecPreferences(codecs);
      const encoded = sender.createEncodedStreams(); let encodedFrames = 0;
      encoded.readable.pipeThrough(new TransformStream({ async transform(frame, controller) {
        if (++encodedFrames % 2) await new Promise(resolve => setTimeout(resolve, 24));
        controller.enqueue(frame);
      }})).pipeTo(encoded.writable).catch(console.error);
      await send.setLocalDescription(await send.createOffer());
      await receive.setRemoteDescription(send.localDescription);
      await receive.setLocalDescription(await receive.createAnswer());
      await send.setRemoteDescription(receive.localDescription);
      let frame = 0;
      const start = performance.now();
      async function draw() {
        ctx.fillStyle = `hsl(${frame % 360},70%,40%)`; ctx.fillRect(0,0,640,360);
        ctx.fillStyle = '#777';ctx.fillRect(64,920/3,512,32);
        for (let cell=0;cell<32;cell++) {
          const value = cell<8 ? (0xd3 >> (7-cell))&1 : cell<24 ? (frame>>(23-cell))&1 : (~frame>>(31-cell))&1;
          ctx.fillStyle=value?'white':'black';ctx.fillRect(64+cell*16+1,923/3,14,30);
        }
        // Give the source an exact RTP cadence independent of JS timer jitter.
        const picture = new VideoFrame(canvas, { timestamp: Math.round((start + frame*1000/30)*1000) });
        await writer.write(picture); picture.close(); ++frame;
        window.__minimalTimer = setTimeout(draw, Math.max(0, start + frame*1000/30 - performance.now()));
      }
      draw().catch(console.error);
      return { codec: codecs[0].mimeType, requestedBufferMs: 300, encodedDelayMs: 24, sourceClock:'explicit 30 FPS VideoFrame timestamps' };
    });
    const ready = Date.now()+30000;
    while (!(await collectViewerSnapshot(page)).videos.some(v => v.videoWidth > 0 || v.width > 0)) {
      if (Date.now()>ready) throw new Error('Minimal receiver did not start');
      await sleep(100);
    }
    await sleep(20000);
    await startPresentationCapture(page, true, 'counter-complement');
    await sleep(Number(process.env.VDONINJA_SOAK_MS || 30000));
    report.capture = await stopPresentationCapture(page);
    report.snapshot = await collectViewerSnapshot(page);
    report.negotiatedCodecs = await page.evaluate(async () => {
      const stats = await window.__pcList[0].getStats();
      return Array.from(stats.values()).filter(s => s.type === 'inbound-rtp' && s.kind === 'video')
        .map(s => stats.get(s.codecId)?.mimeType || null);
    });
    if (!report.negotiatedCodecs.length || report.negotiatedCodecs.some(codec => codec?.toLowerCase() !== 'video/h264')) {
      throw new Error('The minimal receiver did not negotiate H264');
    }
    report.presentation = analyzePresentationContinuity(report.capture.records, { expectedFps:30, requireMarker:false });
    report.pixels = analyzeVisualSequence(report.capture.decodedRecords);
    report.ok = report.presentation.ok && report.pixels.ok;
  } catch (error) { report.error = String(error.stack || error); }
  finally { if (browser) await browser.close(); await new Promise(resolve => server.close(resolve));
    fs.writeFileSync(path.join(output,'report.json'),JSON.stringify(report,null,2)); }
  console.log(JSON.stringify({ ok:report.ok, output, error:report.error }));
  if (!report.ok) process.exitCode=1;
}
main().catch(e => { console.error(e); process.exitCode=1; });
