function selectMeasuredPcm(pcm, packets, rate, startEpochMs, endEpochMs) {
  if (!(endEpochMs > startEpochMs) || !(rate > 0)) throw new Error('Invalid measured audio window');
  const chunks = [], packetGapsMs = [];
  let cursor = 0, previousEnd = null;
  for (const packet of packets) {
    const start = Number(packet.pts_time)*1000, size = Number(packet.size), count = size/2;
    if (!Number.isFinite(start) || !Number.isInteger(size) || size <= 0 || size % 2) throw new Error('Invalid PCM packet metadata');
    const begin = Math.max(0,Math.min(count,Math.ceil((startEpochMs-start)*rate/1000)));
    const end = Math.max(0,Math.min(count,Math.ceil((endEpochMs-start)*rate/1000)));
    if (end > begin) {
      chunks.push(pcm.subarray(cursor+begin*2,cursor+end*2));
      if (previousEnd !== null) packetGapsMs.push(start-previousEnd);
      previousEnd = start+count*1000/rate;
    }
    cursor += size;
  }
  if (cursor !== pcm.length) throw new Error('PCM packet timestamp coverage differs from decoded bytes');
  const measured = Buffer.concat(chunks);
  const durationSeconds = measured.length/2/rate;
  const expectedDurationSeconds = (endEpochMs-startEpochMs)/1000;
  return { measured, durationSeconds, expectedDurationSeconds, packetGapsMs,
    // Pulse timestamps are latency-compensated wall-clock measurements. Retain
    // small clock jitter, but fail missing coverage rather than concatenate a
    // missing packet interval into apparently continuous PCM.
    timestampCoverageOk: measured.length > 0 && Math.abs(durationSeconds-expectedDurationSeconds) <= 0.1 &&
      packetGapsMs.every(gap => Math.abs(gap) <= 1) };
}
module.exports = { selectMeasuredPcm };
