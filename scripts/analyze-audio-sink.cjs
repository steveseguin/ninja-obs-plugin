// Analyze the actual dedicated sink separately from raw-track and Web Audio taps.
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { analyzePcm16Le, createPcm16Wav } = require('../tests/tools/audio-continuity-analysis.cjs');
const { selectMeasuredPcm } = require('../tests/tools/audio-sink-coverage.cjs');
const root = path.resolve(process.argv[2]);
const report = JSON.parse(fs.readFileSync(path.join(root,'av-capture.json')));
if (!report.compareAudioTaps || !report.ok) throw new Error('Require a successful constant-tone capture with audio tap comparison');
const file = path.join(root,'audio.nut');
const info = JSON.parse(execFileSync('ffprobe',['-v','error','-select_streams','a:0','-show_packets','-show_streams',
  '-show_entries','stream=codec_name,sample_rate,channels:packet=pts_time,size','-of','json',file],{encoding:'utf8',maxBuffer:32*1024*1024}));
const stream = info.streams[0];
if (stream.codec_name !== 'pcm_s16le' || stream.channels !== 1 || Number(stream.sample_rate) !== 48000) throw new Error('Require PCM16 mono 48 kHz sink capture');
const pcm = execFileSync('ffmpeg',['-v','error','-i',file,'-f','s16le','-acodec','pcm_s16le','-'],{maxBuffer:128*1024*1024});
const rate = 48000;
const { measured, durationSeconds, packetGapsMs, timestampCoverageOk } = selectMeasuredPcm(
  pcm,info.packets,rate,report.captureStartEpochMs,report.captureEndEpochMs);
const nativeLog = path.join(root,'chromium-process.log');
const events = fs.existsSync(nativeLog) ? fs.readFileSync(nativeLog,'utf8').split('\n').filter(l=>l.includes('VDONINJA_AUDIO ')).map(line=>{
  const fields = Object.fromEntries([...line.matchAll(/(\w+)=([^\s]+)/g)].map(m=>[m[1],m[2]]));
  for (const key of ['now_us','available','requested','sample_rate']) fields[key]=Number(fields[key]);
  const first = report.audioTaps?.rawTrack?.firstTimestamp;
  fields.relativeToRawCaptureMs = Number.isFinite(first) ? (fields.now_us-first)/1000 : null;
  return fields;
}) : null;
const result = {scope:'Constant 997 Hz tone at a dedicated virtual sink; no physical output validation.',
  sink:analyzePcm16Le(measured,{sampleRate:rate,toneHz:997}),
  webAudio:report.audioTaps.analysis,rawTrack:report.audioTaps.rawTrack?.analysis || null,
  nativeFifoEvents:events,
  nativeTraceRequested:report.fieldTrials.includes('WebRTC-AudioFifoTrace/Enabled/'),
  measuredDurationSeconds:durationSeconds,
  timestampCoverageOk,
  packetTimestampGapsMs:{minimum:Math.min(...packetGapsMs),maximum:Math.max(...packetGapsMs)},
  defaultSinkRestored:report.defaultBefore===report.defaultAfter};
result.allAudioProbesPassed = Boolean(result.sink.ok && result.webAudio.ok && result.rawTrack?.ok && result.defaultSinkRestored && result.timestampCoverageOk);
fs.writeFileSync(path.join(root,'sink-measured.wav'),createPcm16Wav(measured,rate));
fs.writeFileSync(path.join(root,'audio-comparison.json'),JSON.stringify(result,null,2));
console.log(JSON.stringify({sink:result.sink.ok,webAudio:result.webAudio.ok,rawTrack:result.rawTrack?.ok,nativeFifoEvents:events?.length,
  allAudioProbesPassed:result.allAudioProbesPassed}));
