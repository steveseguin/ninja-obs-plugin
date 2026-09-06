// Runs inside the viewer. Freeze every producer before exporting large arrays;
// serializing video records while audio is still recording perturbs its tail.
async function freezeContinuityCapture() {
  const video = window.__vdoninjaPresentationCapture;
  const audio = window.__vdoninjaDecodedAudioCapture;
  if (video) video.active = false;
  if (audio) {
    audio.processor.onaudioprocess = null;
    if (audio.rawTrack) audio.rawTrack.active = false;
  }
  const pending = [];
  if (audio?.freeze) pending.push(audio.freeze());
  if (video?.processorReader) pending.push(video.processorReader.cancel().catch(() => {}));
  if (audio?.rawTrack && !audio.freeze) pending.push(audio.rawTrack.reader.cancel().catch(() => {}));
  await Promise.all(pending);
  await Promise.all([video?.processorDone, audio?.rawTrack?.loop]);
}

module.exports = { freezeContinuityCapture };
