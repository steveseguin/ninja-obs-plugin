const test = require("node:test");
const assert = require("node:assert/strict");
const {
  analyzePresentationContinuity,
  analyzeVisualSequence,
} = require("./tools/presentation-continuity-analysis.cjs");

function frames(count, options = {}) {
  const interval = options.intervalMs || 1000 / 60;
  const result = [];
  let callbackTime = 0;
  let mediaTime = 0;
  let marker = options.markerStart ?? 100;
  for (let index = 0; index < count; index += 1) {
    const extraDelay = options.delays?.[index] || 0;
    callbackTime += index === 0 ? 0 : interval + extraDelay;
    mediaTime += index === 0 ? 0 : interval / 1000;
    result.push({
      callbackTime,
      mediaTime,
      presentedFrames: index + 1,
      rtpTimestamp: Math.round(index * interval * 90),
      markerFrame: marker,
    });
    marker = (marker + 1) & 0xffff;
  }
  return result;
}

test("steady frame-by-frame presentation passes", () => {
  const result = analyzePresentationContinuity(frames(600), {
    expectedFps: 60,
    requireMarker: true,
  });
  assert.equal(result.ok, true);
  assert.equal(result.markerErrors, 0);
  assert.ok(result.averagePresentedFps > 59.99);
});

test("a single short presentation hitch is detected by strict cadence", () => {
  const records = frames(600, { delays: { 300: 12 } });
  const result = analyzePresentationContinuity(records, {
    expectedFps: 60,
    maximumCallbackDeviationMs: 8,
  });
  assert.equal(result.ok, false);
  assert.match(result.failures.join(" "), /cadence deviation/);
  assert.equal(result.markerErrors, 0);
});

test("inconsistent media and compositor counters fail independently of average FPS", () => {
  const records = frames(300);
  records[150].presentedFrames += 1;
  for (let index = 151; index < records.length; index += 1) {
    records[index].presentedFrames += 1;
  }
  const result = analyzePresentationContinuity(records, { expectedFps: 60 });
  assert.equal(result.presentedFrameJumps, 1);
  assert.equal(result.ok, false);
  assert.match(result.failures.join(" "), /excess presented frame/);
});

test("duplicate, backwards, skipped, and corrupt visual markers fail", () => {
  const records = frames(120);
  records[20].markerFrame = records[19].markerFrame;
  records[40].markerFrame = records[39].markerFrame - 3;
  records[60].markerFrame = records[59].markerFrame + 2;
  records[80].markerFrame = null;
  const result = analyzeVisualSequence(records);
  assert.equal(result.ok, false);
  assert.ok(result.markerDuplicates > 0);
  assert.ok(result.markerBackwards > 0);
  assert.ok(result.markerSkipped > 0);
  assert.ok(result.markerInvalid > 0);
  assert.match(result.failures.join(" "), /visual sequence/);
});

test("16-bit marker rollover is forward and continuous", () => {
  const records = frames(5, { markerStart: 65534 });
  const result = analyzeVisualSequence(records);
  assert.equal(result.ok, true);
  assert.equal(result.markerErrors, 0);
});

test("a missed callback does not mean the compositor skipped a frame", () => {
  const records = frames(300).filter((_, index) => index !== 150);
  const result = analyzePresentationContinuity(records, { expectedFps: 60 });
  assert.equal(result.ok, true);
  assert.equal(result.presentedFrameJumps, 1);
  assert.equal(result.missingMediaFrames, 0);
  assert.ok(result.averagePresentedFps > 59.99);
});

test("compositor timestamps separate callback lateness from video cadence", () => {
  const records = frames(300);
  records.forEach((record, index) => {
    record.presentationTime = record.callbackTime;
    if (index >= 150) record.callbackTime += 16;
  });
  const result = analyzePresentationContinuity(records, { expectedFps: 60, maximumCallbackDeviationMs: 8 });
  assert.equal(result.ok, true);
  assert.equal(result.timingBasis, "compositor submission");
});

test("missing media is detected even when every compositor callback arrives", () => {
  const records = frames(300);
  for (let index = 150; index < records.length; ++index) {
    records[index].mediaTime += 1 / 60;
    records[index].rtpTimestamp += 1500;
  }
  const result = analyzePresentationContinuity(records, { expectedFps: 60 });
  assert.equal(result.ok, false);
  assert.equal(result.missingMediaFrames, 1);
});

test("receiver playout-clock adjustment is not source-frame loss", () => {
  const records = frames(300);
  for (let index = 150; index < records.length; ++index) records[index].mediaTime += 0.020;
  assert.equal(analyzePresentationContinuity(records, { expectedFps: 60 }).ok, true);
});

test("RTP timestamp rollover is continuous", () => {
  const records = frames(300);
  records.forEach(record => { record.rtpTimestamp = (record.rtpTimestamp + 0xffff0000) >>> 0; });
  assert.equal(analyzePresentationContinuity(records, { expectedFps: 60 }).ok, true);
});
