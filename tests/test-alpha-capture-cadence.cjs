const assert = require("node:assert/strict");
const {
  analyzeAlphaCaptureCadence,
} = require("../scripts/obs-websocket-vdoninja-source-check.cjs");

function samplesFromGaps(start, gaps) {
  const times = [start];
  for (const gap of gaps) {
    times.push(times[times.length - 1] + gap);
  }
  return times.map((captureStartedAtMs) => ({ screenshot: { captureStartedAtMs } }));
}

const windowsJitter = analyzeAlphaCaptureCadence(
  samplesFromGaps(1000, [75, 75, 76, 103, 75, 75, 76, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75]),
  { inputCreatedAtMs: 999, requiredMaximumMs: 100 }
);
assert.equal(windowsJitter.ok, true, "one bounded scheduler outlier should not fail a real-time capture run");
assert.deepEqual(windowsJitter.gapsOverRequiredMaximumMs, [103]);

const stalledCapture = analyzeAlphaCaptureCadence(
  samplesFromGaps(2000, [75, 75, 151, 75]),
  { inputCreatedAtMs: 1999, requiredMaximumMs: 100 }
);
assert.equal(stalledCapture.ok, false, "a gap beyond the absolute bound must fail");
assert.match(stalledCapture.failureReasons.join(" "), /absolute 150ms limit/);

const repeatedSlowCaptures = analyzeAlphaCaptureCadence(
  samplesFromGaps(3000, [75, 110, 75, 110, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75]),
  { inputCreatedAtMs: 2999, requiredMaximumMs: 100 }
);
assert.equal(repeatedSlowCaptures.ok, false, "repeated slow captures must not be hidden as scheduler jitter");
assert.match(repeatedSlowCaptures.failureReasons.join(" "), /2 capture-start gaps exceeded 100ms/);

console.log("Alpha capture cadence regression passed");
