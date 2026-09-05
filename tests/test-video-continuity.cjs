const test = require("node:test");
const assert = require("node:assert/strict");
const {
  analyzeVideoContinuity,
  hasDecodedVideoProgress,
} = require("./tools/video-continuity-analysis.cjs");

test("a cached first picture cannot satisfy decoded-video warmup", () => {
  assert.equal(hasDecodedVideoProgress(snapshot(0, 1), snapshot(2000, 1), 30), false);
  assert.equal(hasDecodedVideoProgress(snapshot(0, 1), snapshot(2000, 2), 30), false);
  assert.equal(hasDecodedVideoProgress(snapshot(0, 1), snapshot(2000, 31), 30), true);
});

test("received packets and a decoder counter reset cannot satisfy warmup", () => {
  assert.equal(
    hasDecodedVideoProgress(snapshot(0, 10), snapshot(2000, 100, { framesDecoded: 10 }), 30),
    false,
  );
  assert.equal(hasDecodedVideoProgress(snapshot(0, 100), snapshot(2000, 1), 30), false);
});

function snapshot(timestamp, frames, options = {}) {
  return {
    timestamp,
    videos: [
      {
        currentTime: options.currentTime ?? timestamp / 1000,
        videoTracks: 1,
      },
    ],
    pcStats: [
      {
        framesReceived: frames,
        framesDecoded: options.framesDecoded ?? frames,
        framesDropped: options.framesDropped ?? 0,
        videoPacketsLost: options.packetsLost ?? 0,
        freezeCount: options.freezeCount ?? 0,
        totalFreezesDuration: options.totalFreezesDuration ?? 0,
        totalInterFrameDelay: options.totalInterFrameDelay ?? frames / 30,
        totalSquaredInterFrameDelay:
          options.totalSquaredInterFrameDelay ?? frames / 900,
        videoJitter: options.videoJitter ?? 0.002,
      },
    ],
  };
}

test("steady 30 FPS delivery passes strict continuity analysis", () => {
  const samples = [
    snapshot(0, 0),
    snapshot(250, 8),
    snapshot(500, 15),
    snapshot(750, 23),
    snapshot(1000, 30),
  ];

  const result = analyzeVideoContinuity(samples, { expectedFps: 30 });
  assert.equal(result.ok, true);
  assert.equal(result.framesDecoded, 30);
  assert.equal(result.maximumObservedFrameStallMs, 0);
  assert.equal(result.framesDropped, 0);
});

test("a sub-second frame stall is detected even when average FPS recovers", () => {
  const samples = [
    snapshot(0, 0),
    snapshot(250, 8),
    snapshot(500, 8, { currentTime: 0.25 }),
    snapshot(750, 8, { currentTime: 0.25 }),
    snapshot(1000, 30, { currentTime: 1 }),
  ];

  const result = analyzeVideoContinuity(samples, {
    expectedFps: 30,
    maximumFrameStallMs: 400,
  });
  assert.equal(result.averageDecodedFps, 30);
  assert.equal(result.ok, false);
  assert.equal(result.maximumObservedFrameStallMs, 500);
  assert.match(result.failures.join(" "), /frame stall/);
});

test("low sustained frame rate is detected", () => {
  const samples = [
    snapshot(0, 0),
    snapshot(500, 8),
    snapshot(1000, 16),
    snapshot(1500, 24),
    snapshot(2000, 32),
  ];

  const result = analyzeVideoContinuity(samples, { expectedFps: 30 });
  assert.equal(result.ok, false);
  assert.ok(result.averageFpsRatio < 0.95);
  assert.match(result.failures.join(" "), /average decoded FPS/);
});

test("a modest but sustained frame-rate deficit fails the strict default", () => {
  const samples = [
    snapshot(0, 0),
    snapshot(1000, 28),
    snapshot(2000, 56),
    snapshot(3000, 84),
  ];

  const result = analyzeVideoContinuity(samples, { expectedFps: 30 });
  assert.equal(result.averageFpsRatio, 28 / 30);
  assert.equal(result.ok, false);
  assert.match(result.failures.join(" "), /below 95%/);
});

test("inter-frame timing variance is detected", () => {
  const samples = [
    snapshot(0, 0),
    snapshot(1000, 30, {
      totalInterFrameDelay: 1,
      totalSquaredInterFrameDelay: 0.08,
    }),
  ];

  const result = analyzeVideoContinuity(samples, {
    expectedFps: 30,
    maximumInterFrameStddevMs: 20,
  });
  assert.equal(result.ok, false);
  assert.ok(result.interFrameStddevMs > 20);
  assert.match(result.failures.join(" "), /inter-frame standard deviation/);
});

test("packet loss, dropped frames, freezes, and excessive jitter fail", () => {
  const samples = [
    snapshot(0, 0),
    snapshot(1000, 30, {
      framesDropped: 1,
      packetsLost: 2,
      freezeCount: 1,
      totalFreezesDuration: 0.25,
      videoJitter: 0.2,
    }),
  ];

  const result = analyzeVideoContinuity(samples, { expectedFps: 30 });
  assert.equal(result.ok, false);
  assert.match(result.failures.join(" "), /dropped/);
  assert.match(result.failures.join(" "), /lost/);
  assert.match(result.failures.join(" "), /freeze/);
  assert.match(result.failures.join(" "), /RTP jitter/);
});
