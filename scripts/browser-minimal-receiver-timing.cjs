// Minimal loopback WebRTC receiver experiment; no OBS, VDO.Ninja or relay.
// Synthetic encoded-frame delay creates arrival variation. The diagnostic
// Chromium patch can force timing-sample exclusion without faking actual NACKs.
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { execFileSync } = require('node:child_process');
const { chromium } = require('playwright');
const { startPresentationCapture, stopPresentationCapture, collectViewerSnapshot } =
  require('./obs-websocket-vdoninja-publish-check.cjs');
const { analyzePresentationContinuity, analyzeVisualSequence } =
  require('../tests/tools/presentation-continuity-analysis.cjs');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function main() {
  const output = path.resolve(process.argv[2] || 'artifacts/minimal-receiver');
  fs.mkdirSync(output, { recursive: true });
  const includeAudio = process.env.VDONINJA_MINIMAL_AUDIO === '1';
  const fps = Number(process.env.VDONINJA_MINIMAL_FPS || 30);
  if (![30,60].includes(fps)) throw new Error('Minimal source FPS must be 30 or 60');
  const audioDelayMs = Number(process.env.VDONINJA_MINIMAL_AUDIO_DELAY_MS || 0);
  if (!Number.isFinite(audioDelayMs) || audioDelayMs < 0 || audioDelayMs > 100 ||
      (audioDelayMs && process.env.VDONINJA_MINIMAL_AUDIO !== '1'))
    throw new Error('Audio delay requires audio enabled and a value from 0 to 100 ms');
  const server = http.createServer((req, res) => { res.setHeader('Content-Type', 'text/html');
    res.end('<!doctype html><canvas width="640" height="360"></canvas><video autoplay '+
      (includeAudio ? '' : 'muted ')+'playsinline></video>'); });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const fieldTrials = process.env.VDONINJA_CHROMIUM_FIELD_TRIALS || '';
  const executable = process.env.VDONINJA_BROWSER_EXECUTABLE;
  const logFile = path.join(output, 'chromium.log');
  const report = { ok: false, fieldTrials, includeAudio, fps, audioDelayMs, executable: executable || null, logFile,
    syntheticTimingExclusionRequested: fieldTrials.includes('WebRTC-ForceTimingSampleExclusion/Enabled/'),
    note: 'Loopback with synthetic encoded-frame delay; does not simulate network NACK feedback.' };
  const pactl = (...args) => execFileSync('pactl',args,{encoding:'utf8'}).trim();
  let browser, sink, moduleId, defaultBefore;
  try {
    if (includeAudio) {
      defaultBefore = pactl('get-default-sink');
      sink = `ninja-minimal-${process.pid}`;
      moduleId = pactl('load-module','module-null-sink',`sink_name=${sink}`,'rate=48000','channels=1');
      if (pactl('get-default-sink') !== defaultBefore) {
        pactl('set-default-sink',defaultBefore);
        throw new Error('Private audio sink unexpectedly changed the default');
      }
      report.audioSink = sink;
    }
    browser = await chromium.launch({ ...(executable ? { executablePath: executable } : {}),
      ...(includeAudio ? {ignoreDefaultArgs:['--mute-audio']} : {}),
      env: { ...process.env, ...(sink ? {PULSE_SINK:sink} : {}), CHROME_LOG_FILE: logFile },
      args: ['--autoplay-policy=no-user-gesture-required', '--enable-logging', `--log-file=${logFile}`,
        ...(fieldTrials ? [`--force-fieldtrials=${fieldTrials}`] : [])] });
    report.browserVersion = browser.version();
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${server.address().port}`);
    report.setup = await page.evaluate(async ({includeAudio,fps,audioDelayMs}) => {
      const canvas = document.querySelector('canvas'), ctx = canvas.getContext('2d');
      if (typeof MediaStreamTrackGenerator !== 'function') throw new Error('Video track generator is required');
      const track = new MediaStreamTrackGenerator({ kind:'video' });
      const writer = track.writable.getWriter(), media = new MediaStream([track]);
      let audioTrack;
      if (includeAudio) {
        audioTrack = new MediaStreamTrackGenerator({kind:'audio'});
        media.addTrack(audioTrack);
        const workerSource = `onmessage = async ({data}) => {
          const writer = data.getWriter(), start = performance.now();
          for (let block = 0; ; ++block) {
            await new Promise(resolve => setTimeout(resolve, Math.max(0,start+block*10-performance.now())));
            const pcm = new Float32Array(480);
            for(let i=0;i<480;i++) pcm[i]=0.125*Math.sin(2*Math.PI*997*(block*480+i)/48000);
            const frame = new AudioData({format:'f32-planar',sampleRate:48000,numberOfFrames:480,
              numberOfChannels:1,timestamp:block*10000,data:pcm});
            try { await writer.write(frame); } finally { frame.close(); }
          }
        };`;
        const workerUrl = URL.createObjectURL(new Blob([workerSource],{type:'text/javascript'}));
        const worker = new Worker(workerUrl);
        worker.onerror = event => {window.__minimalAudioError=event.message;};
        worker.postMessage(audioTrack.writable,[audioTrack.writable]);
        window.__minimalAudioWorker = worker;
        window.__minimalAudioWorkerUrl = workerUrl;
      }
      const send = new RTCPeerConnection({ encodedInsertableStreams: true });
      const receive = new RTCPeerConnection();
      window.__pcList = [receive]; window.__minimalPeers = [send, receive];
      send.onicecandidate = e => { if (e.candidate) receive.addIceCandidate(e.candidate).catch(console.error); };
      receive.onicecandidate = e => { if (e.candidate) send.addIceCandidate(e.candidate).catch(console.error); };
      receive.ontrack = e => { document.querySelector('video').srcObject = e.streams[0];
        e.receiver.playoutDelayHint = 0.3; e.receiver.jitterBufferTarget = 300; };
      const sender = send.addTrack(track, media);
      if (audioTrack) {
        const audioSender = send.addTrack(audioTrack,media);
        const audioEncoded = audioSender.createEncodedStreams();
        const pending = new Set();
        const audioReadable = audioDelayMs ? audioEncoded.readable.pipeThrough(new TransformStream({
          transform(frame,controller) {
            // Independent deadlines add latency without serializing audio
            // frames behind a per-frame sleep and reducing their throughput.
            const completion = new Promise(resolve => setTimeout(() => {
              try { controller.enqueue(frame); }
              catch(error) { window.__minimalAudioError=String(error); }
              resolve();
            },audioDelayMs));
            pending.add(completion); completion.then(()=>pending.delete(completion));
          },
          flush() { return Promise.all(pending); },
        })) : audioEncoded.readable;
        audioReadable.pipeTo(audioEncoded.writable).catch(error => {
          window.__minimalAudioError=String(error);
        });
      }
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
        const picture = new VideoFrame(canvas, { timestamp: Math.round((start + frame*1000/fps)*1000) });
        await writer.write(picture); picture.close(); ++frame;
        window.__minimalTimer = setTimeout(draw, Math.max(0, start + frame*1000/fps - performance.now()));
      }
      draw().catch(console.error);
      return { codec: codecs[0].mimeType, requestedBufferMs: 300, encodedDelayMs: 24,
        includeAudio, audioDelayMs, sourceClock:`explicit ${fps} FPS VideoFrame timestamps` };
    },{includeAudio,fps,audioDelayMs});
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
    if (includeAudio) {
      report.audio = await page.evaluate(async () => {
        const stats = await window.__pcList[0].getStats();
        const inbound = Array.from(stats.values()).filter(s=>s.type==='inbound-rtp'&&s.kind==='audio');
        return {error:window.__minimalAudioError||null,stats:inbound};
      });
      if (report.audio.error || !report.audio.stats.some(s=>s.totalSamplesReceived>0))
        throw new Error('Minimal synchronization control did not receive audio');
    }
    report.negotiatedCodecs = await page.evaluate(async () => {
      const stats = await window.__pcList[0].getStats();
      return Array.from(stats.values()).filter(s => s.type === 'inbound-rtp' && s.kind === 'video')
        .map(s => stats.get(s.codecId)?.mimeType || null);
    });
    if (!report.negotiatedCodecs.length || report.negotiatedCodecs.some(codec => codec?.toLowerCase() !== 'video/h264')) {
      throw new Error('The minimal receiver did not negotiate H264');
    }
    report.presentation = analyzePresentationContinuity(report.capture.records, { expectedFps:fps, requireMarker:false });
    report.pixels = analyzeVisualSequence(report.capture.decodedRecords);
    report.ok = report.presentation.ok && report.pixels.ok;
  } catch (error) { report.error = String(error.stack || error); }
  finally { if (browser) await browser.close(); await new Promise(resolve => server.close(resolve));
    if (moduleId) {
      pactl('unload-module',moduleId);
      report.defaultSinkRestored = pactl('get-default-sink') === defaultBefore;
      report.ok = report.ok && report.defaultSinkRestored;
    }
    if (includeAudio && fieldTrials.includes('WebRTC-AvSyncTrace/Enabled/')) {
      report.nativeSyncDecisionCount = fs.existsSync(logFile)
        ? (fs.readFileSync(logFile,'utf8').match(/VDONINJA_SYNC /g)||[]).length : 0;
      if (!report.nativeSyncDecisionCount) {
        report.ok = false;
        report.error ||= 'No native A/V synchronization decisions were traced';
      }
    }
    fs.writeFileSync(path.join(output,'report.json'),JSON.stringify(report,null,2)); }
  console.log(JSON.stringify({ ok:report.ok, output, error:report.error }));
  if (!report.ok) process.exitCode=1;
}
main().catch(e => { console.error(e); process.exitCode=1; });
