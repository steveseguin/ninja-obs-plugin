function analyzeObsPerformance(samples, options = {}) {
  if (!Array.isArray(samples) || samples.length < 2) throw new Error('At least two OBS samples are required');
  for (const key of ['maximumRenderSkippedFrames', 'maximumOutputSkippedFrames']) {
    if (options[key] != null && (!Number.isFinite(options[key]) || options[key] < 0)) {
      throw new Error(`Invalid OBS threshold: ${key}`);
    }
  }
  for (let i = 0; i < samples.length; i++) {
    if (!Number.isFinite(samples[i].timestampMs) ||
        (i && samples[i].timestampMs <= samples[i - 1].timestampMs)) {
      throw new Error('OBS sample timestamps must advance');
    }
  }
  const first = samples[0], last = samples[samples.length - 1];
  const seconds = (last.timestampMs - first.timestampMs) / 1000;
  if (!(seconds > 0)) throw new Error('OBS sample timestamps must advance');
  const delta = key => {
    for (let i = 0; i < samples.length; i++) {
      const value = samples[i].stats[key];
      if (!Number.isFinite(value) || value < 0 || (i && value < samples[i - 1].stats[key])) {
        throw new Error(`Invalid or reset OBS counter: ${key}`);
      }
    }
    return last.stats[key] - first.stats[key];
  };
  const renderFrames = delta('renderTotalFrames');
  const renderSkippedFrames = delta('renderSkippedFrames');
  const outputFrames = delta('outputTotalFrames');
  const outputSkippedFrames = delta('outputSkippedFrames');
  const renderTimes = samples.map(s => s.stats.averageFrameRenderTime).filter(Number.isFinite).sort((a, b) => a - b);
  const failures = [];
  if (renderSkippedFrames > (options.maximumRenderSkippedFrames ?? 0)) failures.push(`OBS skipped ${renderSkippedFrames} render frames`);
  if (outputSkippedFrames > (options.maximumOutputSkippedFrames ?? 0)) failures.push(`OBS skipped ${outputSkippedFrames} output frames`);
  if (!renderFrames || !outputFrames) failures.push('OBS produced no render/output frames during the measurement');
  return {
    ok: failures.length === 0, failures, durationSeconds: seconds,
    renderFrames, renderSkippedFrames,
    renderSkippedPercent: renderFrames ? 100 * renderSkippedFrames / renderFrames : null,
    outputFrames, outputSkippedFrames, outputFps: outputFrames / seconds,
    averageRenderTimeMs: renderTimes.length ? renderTimes.reduce((a, b) => a + b, 0) / renderTimes.length : null,
    p95RenderTimeMs: renderTimes.length ? renderTimes[Math.ceil(renderTimes.length * 0.95) - 1] : null,
  };
}
module.exports = { analyzeObsPerformance };
