function sampleAt(pcm, index) {
  return pcm.readInt16LE(index * 2) / 32768;
}

function median(values) {
  if (values.length === 0) {
    return 0;
  }

  const sorted = [...values].sort((left, right) => left - right);
  const middle = Math.floor(sorted.length / 2);
  if (sorted.length % 2 === 0) {
    return (sorted[middle - 1] + sorted[middle]) / 2;
  }
  return sorted[middle];
}

function analyzePcm16Le(pcm, options = {}) {
  if (!Buffer.isBuffer(pcm) || pcm.length % 2 !== 0) {
    throw new Error(
      "PCM input must be a whole number of signed 16-bit little-endian samples",
    );
  }

  const sampleRate = Number(options.sampleRate || 48000);
  const toneHz = Number(options.toneHz || 997);
  const trimStartSamples = Math.floor(
    Number(options.trimStartSeconds ?? 0.5) * sampleRate,
  );
  const trimEndSamples = Math.floor(
    Number(options.trimEndSeconds ?? 0.25) * sampleRate,
  );
  const totalSamples = pcm.length / 2;
  const startSample = Math.min(trimStartSamples, totalSamples);
  const endSample = Math.max(startSample, totalSamples - trimEndSamples);
  const analyzedSamples = endSample - startSample;
  const windowSamples = Math.max(
    1,
    Math.floor((Number(options.rmsWindowMs || 10) * sampleRate) / 1000),
  );

  if (analyzedSamples < windowSamples * 2) {
    throw new Error(
      `PCM capture is too short to analyze (${analyzedSamples} usable samples)`,
    );
  }

  const windowRms = [];
  let peak = 0;
  let clippedSamples = 0;
  for (
    let windowStart = startSample;
    windowStart + windowSamples <= endSample;
    windowStart += windowSamples
  ) {
    let sumSquares = 0;
    for (
      let index = windowStart;
      index < windowStart + windowSamples;
      index += 1
    ) {
      const sample = sampleAt(pcm, index);
      const magnitude = Math.abs(sample);
      sumSquares += sample * sample;
      peak = Math.max(peak, magnitude);
      if (magnitude >= 0.98) {
        clippedSamples += 1;
      }
    }
    windowRms.push(Math.sqrt(sumSquares / windowSamples));
  }

  const medianRms = median(windowRms);
  const minimumToneRms = Number(options.minimumToneRms || 0.002);
  const dropoutThreshold = Math.max(
    1 / 32768,
    medianRms * Number(options.dropoutRmsRatio || 0.2),
  );
  let dropoutWindows = 0;
  let longestDropoutWindows = 0;
  let currentDropoutWindows = 0;
  for (const rms of windowRms) {
    if (rms < dropoutThreshold) {
      dropoutWindows += 1;
      currentDropoutWindows += 1;
      longestDropoutWindows = Math.max(
        longestDropoutWindows,
        currentDropoutWindows,
      );
    } else {
      currentDropoutWindows = 0;
    }
  }

  // A steady sine obeys x[n] = 2*cos(w)*x[n-1] - x[n-2]. A click or
  // discontinuity produces a prediction residual far above codec noise.
  const predictor = 2 * Math.cos((2 * Math.PI * toneHz) / sampleRate);
  const clickThreshold = Math.max(
    Number(options.minimumClickResidual || 0.01),
    medianRms * Number(options.clickResidualRmsRatio || 0.75),
  );
  let clickCandidates = 0;
  let clickEventCount = 0;
  const clickEvents = [];
  let lastClickCandidateSample = -10;
  let storedClickEventIndex = -1;
  let maxPredictionResidual = 0;
  let maxAdjacentJump = 0;
  let previousPrevious = sampleAt(pcm, startSample);
  let previous = sampleAt(pcm, startSample + 1);
  for (let index = startSample + 2; index < endSample; index += 1) {
    const current = sampleAt(pcm, index);
    const predictionResidual = Math.abs(
      current - (predictor * previous - previousPrevious),
    );
    maxPredictionResidual = Math.max(maxPredictionResidual, predictionResidual);
    maxAdjacentJump = Math.max(maxAdjacentJump, Math.abs(current - previous));
    if (predictionResidual > clickThreshold) {
      clickCandidates += 1;
      const adjacentJump = Math.abs(current - previous);
      if (index - lastClickCandidateSample > 4) {
        clickEventCount += 1;
        if (clickEvents.length < 20) {
          clickEvents.push({
            sampleIndex: index,
            timeSeconds: index / sampleRate,
            predictionResidual,
            adjacentJump,
            previous,
            current,
          });
          storedClickEventIndex = clickEvents.length - 1;
        } else {
          storedClickEventIndex = -1;
        }
      } else if (
        storedClickEventIndex >= 0 &&
        predictionResidual >
          clickEvents[storedClickEventIndex].predictionResidual
      ) {
        clickEvents[storedClickEventIndex] = {
          sampleIndex: index,
          timeSeconds: index / sampleRate,
          predictionResidual,
          adjacentJump,
          previous,
          current,
        };
      }
      lastClickCandidateSample = index;
    }
    previousPrevious = previous;
    previous = current;
  }

  const tonePresent = medianRms >= minimumToneRms;
  const ok =
    tonePresent &&
    dropoutWindows === 0 &&
    clickCandidates === 0 &&
    clippedSamples === 0;
  return {
    ok,
    sampleRate,
    toneHz,
    totalSamples,
    analyzedSamples,
    analyzedDurationSeconds: analyzedSamples / sampleRate,
    medianRms,
    peak,
    minimumToneRms,
    dropoutThreshold,
    dropoutWindows,
    longestDropoutMs:
      (longestDropoutWindows * windowSamples * 1000) / sampleRate,
    clickThreshold,
    clickCandidates,
    clickEventCount,
    clickEvents,
    maxPredictionResidual,
    maxAdjacentJump,
    clippedSamples,
  };
}

function createPcm16Wav(pcm, sampleRate, channels = 1) {
  const header = Buffer.alloc(44);
  const bytesPerSample = 2;
  const byteRate = sampleRate * channels * bytesPerSample;
  const blockAlign = channels * bytesPerSample;

  header.write("RIFF", 0, "ascii");
  header.writeUInt32LE(36 + pcm.length, 4);
  header.write("WAVE", 8, "ascii");
  header.write("fmt ", 12, "ascii");
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(byteRate, 28);
  header.writeUInt16LE(blockAlign, 32);
  header.writeUInt16LE(bytesPerSample * 8, 34);
  header.write("data", 36, "ascii");
  header.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([header, pcm]);
}

module.exports = {
  analyzePcm16Le,
  createPcm16Wav,
};
