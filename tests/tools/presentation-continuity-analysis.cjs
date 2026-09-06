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

function analyzePresentationContinuity(records, options = {}) {
  if (!Array.isArray(records) || records.length < 2) {
    throw new Error("Presentation continuity analysis requires at least two frames");
  }

  const expectedFps = Math.max(1, Number(options.expectedFps || 30));
  const expectedIntervalMs = 1000 / expectedFps;
  const minimumAverageFpsRatio = Number(options.minimumAverageFpsRatio ?? 0.95);
  const maximumCallbackDeviationMs = Number(
    options.maximumCallbackDeviationMs ?? Math.max(8, expectedIntervalMs * 0.75),
  );
  const maximumPresentationStallMs = Number(
    options.maximumPresentationStallMs ?? Math.max(100, expectedIntervalMs * 3.5),
  );
  const maximumPresentedFrameJumps = Number(
    options.maximumPresentedFrameJumps ?? Infinity,
  );
  const maximumMarkerErrors = Number(options.maximumMarkerErrors ?? 0);
  const requireMarker = options.requireMarker === true;

  const valid = records.filter(
    (record) =>
      Number.isFinite(Number(record.callbackTime)) &&
      Number.isFinite(Number(record.mediaTime)) &&
      Number.isFinite(Number(record.presentedFrames)),
  );
  if (valid.length < 2) {
    throw new Error("Presentation continuity records did not contain two valid frames");
  }

  // requestVideoFrameCallback is best-effort: counter jumps indicate missed callbacks,
  // not necessarily missing video. Prefer the compositor submission timestamp.
  // https://wicg.github.io/video-rvfc/
  const timeKey = valid.every((record) => Number.isFinite(record.presentationTime))
    ? "presentationTime" : "callbackTime";
  const timingBasis = timeKey === "presentationTime" ? "compositor submission" : "callback arrival";
  let rtpComparedIntervals = 0;
  let missingMediaFrames = 0;
  let excessPresentedFrames = 0;
  const completeRtpClock = valid.every(record => Number.isInteger(record.rtpTimestamp));
  const mediaProgressBasis = completeRtpClock ? "RTP source clock" : "mediaTime";
  const mediaTimelineAvailable = !valid.every(record => Number(record.mediaTime) === 0);
  let nonForwardRtpTimestamps = 0;
  const callbackIntervalsMs = [];
  const callbackDeviationsMs = [];
  const mediaIntervalsMs = [];
  const driftMs = [];
  let duplicatePresentedFrames = 0;
  let nonForwardPresentedFrames = 0;
  let presentedFrameJumps = 0;
  let nonForwardMediaTimes = 0;
  let markerInvalid = 0;
  let markerDuplicates = 0;
  let markerBackwards = 0;
  let markerSkipped = 0;
  let markerCompared = 0;
  const first = valid[0];

  for (let index = 1; index < valid.length; index += 1) {
    const before = valid[index - 1];
    const after = valid[index];
    const callbackInterval = Number(after[timeKey]) - Number(before[timeKey]);
    const mediaInterval = (Number(after.mediaTime) - Number(before.mediaTime)) * 1000;
    callbackIntervalsMs.push(callbackInterval);
    const counterDelta = Number(after.presentedFrames) - Number(before.presentedFrames);
    callbackDeviationsMs.push(Math.abs(callbackInterval / Math.max(1, counterDelta) - expectedIntervalMs));
    // WebRTC mediaTime may follow the receiver playout clock. Only the RTP
    // source clock can establish missing fixed-rate source frames here.
    const hasRtp = Number.isInteger(before.rtpTimestamp) && Number.isInteger(after.rtpTimestamp);
    const rtpTicks = hasRtp ? (after.rtpTimestamp - before.rtpTimestamp) >>> 0 : 0;
    const mediaFrames = Math.round(rtpTicks / (90000 / expectedFps));
    if (hasRtp && (rtpTicks === 0 || rtpTicks >= 0x80000000)) nonForwardRtpTimestamps += 1;
    if (hasRtp && counterDelta > 0 && rtpTicks < 0x80000000) {
      rtpComparedIntervals += 1;
      missingMediaFrames += Math.max(0, mediaFrames - counterDelta);
      excessPresentedFrames += Math.max(0, counterDelta - mediaFrames);
    }
    mediaIntervalsMs.push(mediaInterval);
    driftMs.push(
      Math.abs(
        (Number(after[timeKey]) - Number(first[timeKey])) -
          (Number(after.mediaTime) - Number(first.mediaTime)) * 1000,
      ),
    );

    const presentedDelta =
      Number(after.presentedFrames) - Number(before.presentedFrames);
    if (presentedDelta === 0) {
      duplicatePresentedFrames += 1;
    } else if (presentedDelta < 0) {
      nonForwardPresentedFrames += 1;
    } else if (presentedDelta > 1) {
      presentedFrameJumps += presentedDelta - 1;
    }
    if (!(mediaInterval > 0)) {
      nonForwardMediaTimes += 1;
    }

    if (requireMarker) {
      if (before.markerFrame == null || after.markerFrame == null) {
        markerInvalid += after.markerFrame == null ? 1 : 0;
        continue;
      }
      markerCompared += 1;
      const markerDelta =
        (Number(after.markerFrame) - Number(before.markerFrame) + 65536) % 65536;
      if (markerDelta === 0) {
        markerDuplicates += 1;
      } else if (markerDelta > 32768) {
        markerBackwards += 1;
      } else if (markerDelta < presentedDelta) {
        markerDuplicates += presentedDelta - markerDelta;
      } else if (markerDelta > presentedDelta) {
        markerSkipped += markerDelta - presentedDelta;
      }
    }
  }
  if (requireMarker && valid[0].markerFrame == null) {
    markerInvalid += 1;
  }

  const durationSeconds =
    (Number(valid[valid.length - 1][timeKey]) -
      Number(valid[0][timeKey])) /
    1000;
  if (!(durationSeconds > 0)) {
    throw new Error("Presentation callbacks must have increasing timestamps");
  }
  const averagePresentedFps =
    (Number(valid[valid.length - 1].presentedFrames) - Number(first.presentedFrames)) / durationSeconds;
  const averageFpsRatio = averagePresentedFps / expectedFps;
  const maximumCallbackIntervalMs = Math.max(...callbackIntervalsMs);
  const p99CallbackDeviationMs = percentile(callbackDeviationsMs, 0.99);
  const observedMaximumCallbackDeviationMs = Math.max(...callbackDeviationsMs);
  const markerErrors =
    markerInvalid + markerDuplicates + markerBackwards + markerSkipped;
  const failures = [];

  if (averageFpsRatio < minimumAverageFpsRatio) {
    failures.push(
      `average presented FPS ${averagePresentedFps.toFixed(2)} was below ` +
        `${(minimumAverageFpsRatio * 100).toFixed(0)}% of ${expectedFps} FPS`,
    );
  }
  if (maximumCallbackIntervalMs > maximumPresentationStallMs) {
    failures.push(
      `maximum presentation interval ${maximumCallbackIntervalMs.toFixed(2)} ms ` +
        `exceeded ${maximumPresentationStallMs} ms`,
    );
  }
  if (observedMaximumCallbackDeviationMs > maximumCallbackDeviationMs) {
    failures.push(
      `maximum presentation cadence deviation ${observedMaximumCallbackDeviationMs.toFixed(2)} ms ` +
        `exceeded ${maximumCallbackDeviationMs} ms`,
    );
  }
  if (
    duplicatePresentedFrames > 0 ||
    nonForwardPresentedFrames > 0 ||
    presentedFrameJumps > maximumPresentedFrameJumps
  ) {
    failures.push(
      `presented-frame counter recorded ${duplicatePresentedFrames} duplicate, ` +
        `${nonForwardPresentedFrames} backwards, and ${presentedFrameJumps} missed callback(s)`,
    );
  }
  if (missingMediaFrames > 0 || excessPresentedFrames > 0) {
    failures.push(`${missingMediaFrames} missing media frame(s), ${excessPresentedFrames} excess presented frame(s)`);
  }
  if (nonForwardRtpTimestamps > 0) {
    failures.push(`${nonForwardRtpTimestamps} RTP timestamp(s) did not advance`);
  }
  if (!completeRtpClock && nonForwardMediaTimes > 0) {
    failures.push(`${nonForwardMediaTimes} media timestamp(s) did not advance`);
  }
  if (requireMarker && markerCompared === 0) {
    failures.push("no valid visual sequence markers were decoded");
  }
  if (requireMarker && markerErrors > maximumMarkerErrors) {
    failures.push(
      `visual sequence recorded ${markerInvalid} invalid, ${markerDuplicates} duplicate, ` +
        `${markerBackwards} backwards, and ${markerSkipped} skipped frame(s)`,
    );
  }

  return {
    ok: failures.length === 0,
    failures,
    expectedFps,
    timingBasis,
    mediaProgressBasis,
    mediaTimelineAvailable,
    nonForwardRtpTimestamps,
    rtpComparedIntervals,
    missingMediaFrames,
    excessPresentedFrames,
    expectedIntervalMs,
    durationSeconds,
    frameCount: valid.length,
    averagePresentedFps,
    averageFpsRatio,
    maximumCallbackIntervalMs,
    p50CallbackIntervalMs: percentile(callbackIntervalsMs, 0.5),
    p95CallbackIntervalMs: percentile(callbackIntervalsMs, 0.95),
    p99CallbackIntervalMs: percentile(callbackIntervalsMs, 0.99),
    p99CallbackDeviationMs,
    maximumCallbackDeviationMs: observedMaximumCallbackDeviationMs,
    maximumWallMediaDriftMs: mediaTimelineAvailable ? Math.max(0, ...driftMs) : null,
    minimumMediaIntervalMs: Math.min(...mediaIntervalsMs),
    maximumMediaIntervalMs: Math.max(...mediaIntervalsMs),
    duplicatePresentedFrames,
    nonForwardPresentedFrames,
    presentedFrameJumps,
    nonForwardMediaTimes,
    markerCompared,
    markerInvalid,
    markerDuplicates,
    markerBackwards,
    markerSkipped,
    markerErrors,
    thresholds: {
      minimumAverageFpsRatio,
      maximumCallbackDeviationMs,
      maximumPresentationStallMs,
      maximumPresentedFrameJumps,
      maximumMarkerErrors,
      requireMarker,
    },
  };
}

function analyzeVisualSequence(records, options = {}) {
  if (!Array.isArray(records) || records.length < 2) {
    throw new Error("Visual sequence analysis requires at least two decoded frames");
  }
  const maximumMarkerErrors = Number(options.maximumMarkerErrors ?? 0);
  let markerInvalid = 0;
  let markerDuplicates = 0;
  let markerBackwards = 0;
  let markerSkipped = 0;
  let markerCompared = 0;
  for (let index = 0; index < records.length; index += 1) {
    const current = records[index];
    if (current.markerFrame == null) {
      markerInvalid += 1;
      continue;
    }
    if (index === 0 || records[index - 1].markerFrame == null) {
      continue;
    }
    markerCompared += 1;
    const markerDelta =
      (Number(current.markerFrame) -
        Number(records[index - 1].markerFrame) +
        65536) %
      65536;
    if (markerDelta === 0) {
      markerDuplicates += 1;
    } else if (markerDelta > 32768) {
      markerBackwards += 1;
    } else if (markerDelta > 1) {
      markerSkipped += markerDelta - 1;
    }
  }
  const markerErrors =
    markerInvalid + markerDuplicates + markerBackwards + markerSkipped;
  const failures = [];
  if (markerCompared === 0) {
    failures.push("no valid visual sequence markers were decoded");
  }
  if (markerErrors > maximumMarkerErrors) {
    failures.push(
      `visual sequence recorded ${markerInvalid} invalid, ${markerDuplicates} duplicate, ` +
        `${markerBackwards} backwards, and ${markerSkipped} skipped frame(s)`,
    );
  }
  return {
    ok: failures.length === 0,
    failures,
    frameCount: records.length,
    markerCompared,
    markerInvalid,
    markerDuplicates,
    markerBackwards,
    markerSkipped,
    markerErrors,
    maximumMarkerErrors,
  };
}

module.exports = {
  analyzePresentationContinuity,
  analyzeVisualSequence,
};
