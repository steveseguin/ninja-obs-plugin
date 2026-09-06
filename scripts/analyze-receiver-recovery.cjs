// A separate, predeclared recovery window after a deliberate relay outage.
// Keeps the full-run failure intact; never rewrites the publisher report.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { analyzeVideoContinuity } = require('../tests/tools/video-continuity-analysis.cjs');
const { analyzePresentationContinuity, analyzeVisualSequence } =
  require('../tests/tools/presentation-continuity-analysis.cjs');
const { analyzePcm16Le } = require('../tests/tools/audio-continuity-analysis.cjs');
const directory = path.resolve(process.argv[2] || '');
function one(prefix) {
  const files = fs.readdirSync(directory).filter(name => name.startsWith(prefix) && name.endsWith('.json'));
  assert.equal(files.length, 1, `Exactly one ${prefix} JSON is required`);
  return JSON.parse(fs.readFileSync(path.join(directory, files[0]), 'utf8'));
}
const report = one('obs-publish-report-');
const capture = one('obs-presentation-records-');
const timing = one('obs-encoded-timing-');
const phase = one('outage-phase');
assert(!capture.truncated && !timing.overflow, 'Truncated captures cannot establish recovery');
const durationMs = 30000, minimumSettleMs = 20000;
const frames = capture.records;
assert(frames.length > 0 && capture.decodedRecords.length > 0, 'Both video probes are required');
const presentationEndMs = frames.at(-1).presentationTime;
const presentationStartMs = presentationEndMs - durationMs;
const startEpochMs = timing.timeOrigin + presentationStartMs;
assert(Number.isFinite(startEpochMs) && Number.isFinite(phase.outageEndEpochMs), 'Finite phase timestamps required');
assert(startEpochMs >= phase.outageEndEpochMs + minimumSettleMs, 'Recovery window overlaps outage or stabilization');
const presentationRecords = frames.filter(frame => frame.presentationTime >= presentationStartMs);
const decodedEndUs = capture.decodedRecords.at(-1).timestampUs;
const decodedRecords = capture.decodedRecords.filter(frame => frame.timestampUs >= decodedEndUs - durationMs * 1000);
const endEpochMs = report.samples.at(-1).timestamp;
const samples = report.samples.filter(sample => sample.timestamp >= endEpochMs - durationMs);
assert(samples.length >= 30, 'At least 30 one-second recovery snapshots are required');
function audioTail(file) {
  assert(file, 'Both raw-track and Web Audio WAV files are required');
  const wav = fs.readFileSync(file);
  // These are the fixed PCM WAV headers emitted by this repository's harness.
  assert.equal(wav.toString('ascii', 0, 4), 'RIFF');
  assert.equal(wav.toString('ascii', 36, 40), 'data');
  assert.equal(wav.readUInt16LE(20), 1); assert.equal(wav.readUInt16LE(22), 1);
  assert.equal(wav.readUInt16LE(34), 16);
  const sampleRate = wav.readUInt32LE(24), bytes = sampleRate * durationMs / 1000 * 2;
  assert(wav.length >= 44 + bytes, 'Audio capture is shorter than the recovery window');
  return analyzePcm16Le(wav.subarray(wav.length - bytes), { sampleRate });
}
const result = {
  fullRunOk: report.ok, fullRunVideoFailure: report.videoContinuityFailure,
  fullRunAudioFailure: report.audioContinuityFailure, phase,
  recoveryWindow: { durationMs, minimumSettleMs, presentationStartEpochMs: startEpochMs,
    basis: 'Final 30 seconds of each frozen probe; excludes the separately reported intentional outage.' },
  video: analyzeVideoContinuity(samples, { expectedFps:30, maximumLostPackets:0 }),
  presentation: analyzePresentationContinuity(presentationRecords, { expectedFps:30, requireMarker:false }),
  pixels: analyzeVisualSequence(decodedRecords),
  rawAudio: audioTail(report.decodedAudioCapture?.rawTrack?.wavPath),
  webAudio: audioTail(report.decodedAudioCapture?.wavPath),
};
result.recoveryOk = ['video','presentation','pixels','rawAudio','webAudio'].every(key => result[key].ok);
fs.writeFileSync(path.join(directory, 'recovery-report.json'), JSON.stringify(result, null, 2));
console.log(JSON.stringify(result));
if (!result.recoveryOk) process.exitCode = 1;
