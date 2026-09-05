function percentile(values, fraction) {
  if (values.length === 0) {
    return 0;
  }
  const sorted = [...values].sort((left, right) => left - right);
  const index = Math.min(
    sorted.length - 1,
    Math.max(0, Math.ceil(sorted.length * fraction) - 1),
  );
  return sorted[index];
}

function totalPcMetric(snapshot, key) {
  return (snapshot.pcStats || []).reduce(
    (total, stat) => total + (Number(stat[key]) || 0),
    0,
  );
}

function hasDecodedVideoProgress(before, after, minimumFrames) {
  return (
    totalPcMetric(after, "framesDecoded") - totalPcMetric(before, "framesDecoded") >=
    Math.max(2, minimumFrames)
  );
}

function primaryVideoTime(snapshot) {
  const video = (snapshot.videos || []).find(
    (candidate) => candidate.videoTracks > 0,
  );
  return video ? Number(video.currentTime) || 0 : 0;
}

function analyzeVideoContinuity(samples, options = {}) {
  if (!Array.isArray(samples) || samples.length < 2) {
    throw new Error("Video continuity analysis requires at least two samples");
  }

  const expectedFps = Math.max(1, Number(options.expectedFps || 30));
  const minimumAverageFpsRatio = Number(options.minimumAverageFpsRatio ?? 0.95);
  const maximumFrameStallMs = Number(options.maximumFrameStallMs ?? 500);
  const maximumInterFrameStddevMs = Number(
    options.maximumInterFrameStddevMs ?? 20,
  );
  const maximumVideoJitterMs = Number(options.maximumVideoJitterMs ?? 100);
  const maximumDroppedFrames = Number(options.maximumDroppedFrames ?? 0);
  const maximumLostPackets = Number(options.maximumLostPackets ?? 0);

  const first = samples[0];
  const last = samples[samples.length - 1];
  const durationSeconds = Math.max(
    0,
    (Number(last.timestamp) - Number(first.timestamp)) / 1000,
  );
  if (!(durationSeconds > 0)) {
    throw new Error("Video continuity samples must have increasing timestamps");
  }

  const framesReceived =
    totalPcMetric(last, "framesReceived") -
    totalPcMetric(first, "framesReceived");
  const framesDecoded =
    totalPcMetric(last, "framesDecoded") -
    totalPcMetric(first, "framesDecoded");
  const framesDropped =
    totalPcMetric(last, "framesDropped") -
    totalPcMetric(first, "framesDropped");
  const packetsLost =
    totalPcMetric(last, "videoPacketsLost") -
    totalPcMetric(first, "videoPacketsLost");
  const freezeCount =
    totalPcMetric(last, "freezeCount") - totalPcMetric(first, "freezeCount");
  const totalFreezesDuration =
    totalPcMetric(last, "totalFreezesDuration") -
    totalPcMetric(first, "totalFreezesDuration");
  const playbackSeconds = primaryVideoTime(last) - primaryVideoTime(first);
  const averageReceivedFps = framesReceived / durationSeconds;
  const averageDecodedFps = framesDecoded / durationSeconds;
  const averageFpsRatio = averageDecodedFps / expectedFps;

  const intervals = [];
  let currentStallMs = 0;
  let maximumObservedFrameStallMs = 0;
  for (let index = 1; index < samples.length; index += 1) {
    const before = samples[index - 1];
    const after = samples[index];
    const intervalSeconds =
      (Number(after.timestamp) - Number(before.timestamp)) / 1000;
    if (!(intervalSeconds > 0)) {
      continue;
    }
    const decoded =
      totalPcMetric(after, "framesDecoded") -
      totalPcMetric(before, "framesDecoded");
    const received =
      totalPcMetric(after, "framesReceived") -
      totalPcMetric(before, "framesReceived");
    const playbackDelta = primaryVideoTime(after) - primaryVideoTime(before);
    // Chrome can briefly publish a stale framesDecoded counter even while the
    // video element is visibly advancing. Treat the presentation clock as the
    // authoritative stall signal and retain decoded-frame deltas for cadence.
    const stalled =
      playbackDelta < Math.min(0.1, intervalSeconds * 0.25) && decoded <= 0;
    if (stalled) {
      currentStallMs += intervalSeconds * 1000;
      maximumObservedFrameStallMs = Math.max(
        maximumObservedFrameStallMs,
        currentStallMs,
      );
    } else {
      currentStallMs = 0;
    }
    intervals.push({
      durationMs: intervalSeconds * 1000,
      receivedFrames: received,
      decodedFrames: decoded,
      receivedFps: received / intervalSeconds,
      decodedFps: decoded / intervalSeconds,
      playbackSeconds: playbackDelta,
      stalled,
    });
  }

  const totalInterFrameDelay =
    totalPcMetric(last, "totalInterFrameDelay") -
    totalPcMetric(first, "totalInterFrameDelay");
  const totalSquaredInterFrameDelay =
    totalPcMetric(last, "totalSquaredInterFrameDelay") -
    totalPcMetric(first, "totalSquaredInterFrameDelay");
  const interFrameSampleCount = Math.max(1, framesDecoded);
  const meanInterFrameDelaySeconds =
    totalInterFrameDelay / interFrameSampleCount;
  const interFrameVariance = Math.max(
    0,
    totalSquaredInterFrameDelay / interFrameSampleCount -
      meanInterFrameDelaySeconds * meanInterFrameDelaySeconds,
  );
  const interFrameStddevMs = Math.sqrt(interFrameVariance) * 1000;
  const maximumObservedVideoJitterMs =
    Math.max(
      0,
      ...samples.flatMap((sample) =>
        (sample.pcStats || []).map(
          (stat) => (Number(stat.videoJitter) || 0) * 1000,
        ),
      ),
    ) || 0;

  const failures = [];
  if (averageFpsRatio < minimumAverageFpsRatio) {
    failures.push(
      `average decoded FPS ${averageDecodedFps.toFixed(2)} was below ` +
        `${(minimumAverageFpsRatio * 100).toFixed(0)}% of ${expectedFps} FPS`,
    );
  }
  if (maximumObservedFrameStallMs > maximumFrameStallMs) {
    failures.push(
      `maximum frame stall ${maximumObservedFrameStallMs.toFixed(0)} ms ` +
        `exceeded ${maximumFrameStallMs} ms`,
    );
  }
  if (interFrameStddevMs > maximumInterFrameStddevMs) {
    failures.push(
      `inter-frame standard deviation ${interFrameStddevMs.toFixed(2)} ms ` +
        `exceeded ${maximumInterFrameStddevMs} ms`,
    );
  }
  if (maximumObservedVideoJitterMs > maximumVideoJitterMs) {
    failures.push(
      `RTP jitter ${maximumObservedVideoJitterMs.toFixed(2)} ms exceeded ` +
        `${maximumVideoJitterMs} ms`,
    );
  }
  if (framesDropped > maximumDroppedFrames) {
    failures.push(
      `${framesDropped} decoded frame(s) were dropped; maximum is ` +
        `${maximumDroppedFrames}`,
    );
  }
  if (packetsLost > maximumLostPackets) {
    failures.push(
      `${packetsLost} packet(s) were lost; maximum is ${maximumLostPackets}`,
    );
  }
  if (freezeCount > 0 || totalFreezesDuration > 0) {
    failures.push(
      `Chrome reported ${freezeCount} freeze(s) lasting ` +
        `${totalFreezesDuration.toFixed(3)} seconds`,
    );
  }

  const decodedFpsValues = intervals.map((interval) => interval.decodedFps);
  return {
    ok: failures.length === 0,
    failures,
    expectedFps,
    durationSeconds,
    playbackSeconds,
    framesReceived,
    framesDecoded,
    framesDropped,
    packetsLost,
    freezeCount,
    totalFreezesDuration,
    averageReceivedFps,
    averageDecodedFps,
    averageFpsRatio,
    minimumIntervalDecodedFps:
      decodedFpsValues.length > 0 ? Math.min(...decodedFpsValues) : 0,
    p05IntervalDecodedFps: percentile(decodedFpsValues, 0.05),
    p95IntervalDecodedFps: percentile(decodedFpsValues, 0.95),
    maximumObservedFrameStallMs,
    meanInterFrameDelayMs: meanInterFrameDelaySeconds * 1000,
    interFrameStddevMs,
    maximumObservedVideoJitterMs,
    thresholds: {
      minimumAverageFpsRatio,
      maximumFrameStallMs,
      maximumInterFrameStddevMs,
      maximumVideoJitterMs,
      maximumDroppedFrames,
      maximumLostPackets,
    },
    intervals,
  };
}

module.exports = {
  analyzeVideoContinuity,
  hasDecodedVideoProgress,
};
