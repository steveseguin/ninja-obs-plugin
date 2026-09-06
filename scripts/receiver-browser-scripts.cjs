// Reuse the exact measurement functions through native Firefox WebDriver.
// No Playwright protocol patches are required in the diagnostic Firefox build.
const fs = require('node:fs');
const helpers = require('./obs-websocket-vdoninja-publish-check.cjs');
const { installApplicationDiagnostics } = require('../tests/tools/application-diagnostics.cjs');
const { installProbes } = require('../tests/tools/rtc-timing-probes.cjs');
const { analyzePresentationContinuity } = require('../tests/tools/presentation-continuity-analysis.cjs');
const { analyzeVideoContinuity } = require('../tests/tools/video-continuity-analysis.cjs');
const { analyzePcm16Le, createPcm16Wav } = require('../tests/tools/audio-continuity-analysis.cjs');
(async () => {
  if (process.argv[2] === 'analyze') {
    const file = process.argv[3], r = JSON.parse(fs.readFileSync(file));
    r.presentation = analyzePresentationContinuity(r.capture.records, { expectedFps: r.expectedFps });
    r.video = analyzeVideoContinuity(r.samples, { expectedFps:r.expectedFps, maximumLostPackets:0 });
    if (r.audio) {
      const pcm = Buffer.from(r.audio.pcmBase64, 'base64');
      r.audio.analysis = analyzePcm16Le(pcm, { sampleRate:r.audio.sampleRate, toneHz:997 });
      fs.writeFileSync(file.replace(/\.json$/, '-web-audio.wav'), createPcm16Wav(pcm, r.audio.sampleRate));
      delete r.audio.pcmBase64;
    }
    r.ok = Boolean(r.presentation.ok && r.video.ok && r.audio?.analysis.ok && r.relayVerified && r.codecVerified && r.relayProtocolVerified !== false);
    fs.writeFileSync(file, JSON.stringify(r,null,2));
    console.log(JSON.stringify({ok:r.ok,presentation:r.presentation.ok,video:r.video.ok,audio:r.audio?.analysis.ok}));
    return;
  }
  const iceServers = JSON.parse(process.env.VDONINJA_VIEWER_ICE_SERVERS_JSON || 'null');
  if (!Array.isArray(iceServers) || !iceServers.length) throw new Error('Explicit relay configuration is required');
  let preload;
  await installProbes({addInitScript:async(fn,arg) => {preload={source:fn.toString(),arg};}},iceServers,null,{preserveIceConfiguration:process.env.VDONINJA_PRESERVE_VIEWER_ICE_CONFIGURATION === "1"});
  const page = {evaluate:async(fn,arg) => ({source:fn.toString(),arg:arg ?? null})};
  console.log(JSON.stringify({preload,applicationDiagnostics:installApplicationDiagnostics.toString(),
    start:await helpers.startPresentationCapture(page,false,'counter-complement'),
    stop:await helpers.stopPresentationCapture(page),
    snapshot:await helpers.collectViewerSnapshot(page),
    startAudio:await helpers.startDecodedAudioCapture(page),
    stopAudio:await helpers.stopDecodedAudioCapture(page)}));
})().catch(e => {console.error(e);process.exitCode=1;});
