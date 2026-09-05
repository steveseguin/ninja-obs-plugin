const test = require('node:test');
const assert = require('node:assert/strict');
const { analyzeObsPerformance } = require('./tools/obs-performance-analysis.cjs');
function sample(t, render, output, skipped = 0, outputSkipped = 0) {
  return { timestampMs: t, stats: { renderTotalFrames: render, outputTotalFrames: output,
    renderSkippedFrames: skipped, outputSkippedFrames: outputSkipped, averageFrameRenderTime: 9 } };
}
test('uses interval deltas rather than historical startup skips', () => {
  const result = analyzeObsPerformance([sample(1000, 100, 80, 5, 2), sample(3000, 160, 140, 5, 2)]);
  assert.equal(result.ok, true);
  assert.equal(result.outputFps, 30);
  assert.equal(result.renderSkippedFrames, 0);
});
test('flags rendering loss even when the encoder outputs a full 60 fps', () => {
  const result = analyzeObsPerformance([sample(0, 0, 0), sample(1000, 60, 60, 15)]);
  assert.equal(result.ok, false);
  assert.equal(result.outputFps, 60);
  assert.equal(result.renderSkippedPercent, 25);
});
test('checks encoder skips separately from render skips', () => {
  const result = analyzeObsPerformance([sample(0, 0, 0), sample(1000, 30, 30, 0, 2)]);
  assert.match(result.failures[0], /output frames/);
});
test('detects a counter reset between valid-looking endpoints', () => {
  assert.throws(() => analyzeObsPerformance([sample(0, 50, 50), sample(1000, 1, 1), sample(2000, 70, 70)]), /reset/);
});
test('rejects a measurement with no output', () => {
  assert.equal(analyzeObsPerformance([sample(0, 0, 0), sample(1000, 30, 0)]).ok, false);
});
test('allows an explicit render-skip budget without hiding the count', () => {
  const result = analyzeObsPerformance([sample(0, 0, 0), sample(1000, 30, 30, 1)], { maximumRenderSkippedFrames: 1 });
  assert.equal(result.ok, true);
  assert.equal(result.renderSkippedFrames, 1);
});
test('rejects invalid counters and timing', () => {
  assert.throws(() => analyzeObsPerformance([sample(1000, 0, 0), sample(1000, 30, 30)]), /timestamps/);
  assert.throws(() => analyzeObsPerformance([sample(0, 0, 0), sample(1000, NaN, 30)]), /counter/);
});

test('rejects invalid thresholds and intermediate clock reversal', () => {
  const samples = [sample(0, 0, 0), sample(1000, 30, 30)];
  assert.throws(() => analyzeObsPerformance(samples, { maximumRenderSkippedFrames: NaN }), /threshold/);
  assert.throws(() => analyzeObsPerformance(samples, { maximumOutputSkippedFrames: -1 }), /threshold/);
  assert.throws(() => analyzeObsPerformance([sample(0, 0, 0), sample(-1, 1, 1), sample(1000, 30, 30)]), /timestamps/);
});
