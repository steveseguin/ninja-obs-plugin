const test = require("node:test");
const assert = require("node:assert/strict");
const {
  hasAudioConcealment,
  analyzePcm16Le,
  createPcm16Wav,
} = require("./tools/audio-continuity-analysis.cjs");

const sampleRate = 48000;
const toneHz = 997;

test("discarded redundant audio is not evidence of a transport gap", () => {
  // Browser-only Opus RED loopback: 778 packets received, clean PCM, no loss
  // or concealment, but 747 redundant packets discarded by NetEq.
  assert.equal(
    hasAudioConcealment({
      packetsLost: 0,
      packetsDiscarded: 747,
      concealedSamples: 0,
      concealmentEvents: 0,
    }),
    false,
  );
});

test("audio concealment still fails independently of discarded redundancy", () => {
  for (const metric of ["concealedSamples", "concealmentEvents"]) {
    assert.equal(
      hasAudioConcealment({
        packetsLost: 0,
        packetsDiscarded: 747,
        concealedSamples: 0,
        concealmentEvents: 0,
        [metric]: 1,
      }),
      true,
      metric,
    );
  }
});

test("RED can recover lost packets without an audio gap", () => {
  // Impaired relay run: four missing primary packets, but clean decoded PCM
  // and zero concealment because the redundant copies supplied the audio.
  assert.equal(
    hasAudioConcealment({
      packetsLost: 4,
      packetsDiscarded: 1492,
      concealedSamples: 0,
      concealmentEvents: 0,
    }),
    false,
  );
});

function makeTone(seconds = 2, amplitude = 0.08) {
  const sampleCount = Math.floor(sampleRate * seconds);
  const pcm = Buffer.alloc(sampleCount * 2);
  for (let index = 0; index < sampleCount; index += 1) {
    const value =
      Math.sin((2 * Math.PI * toneHz * index) / sampleRate) * amplitude;
    pcm.writeInt16LE(Math.round(value * 32767), index * 2);
  }
  return pcm;
}

test("steady tone passes continuity analysis", () => {
  const result = analyzePcm16Le(makeTone(), { sampleRate, toneHz });
  assert.equal(result.ok, true);
  assert.equal(result.dropoutWindows, 0);
  assert.equal(result.clickCandidates, 0);
});

test("silence inserted into a steady tone is reported as a dropout", () => {
  const pcm = makeTone();
  const dropoutStart = sampleRate;
  pcm.fill(0, dropoutStart * 2, (dropoutStart + sampleRate * 0.03) * 2);

  const result = analyzePcm16Le(pcm, { sampleRate, toneHz });
  assert.equal(result.ok, false);
  assert.ok(result.dropoutWindows >= 2);
  assert.ok(result.longestDropoutMs >= 20);
});

test("a single-sample spike is reported as a click candidate", () => {
  const pcm = makeTone();
  pcm.writeInt16LE(30000, sampleRate * 2);

  const result = analyzePcm16Le(pcm, { sampleRate, toneHz });
  assert.equal(result.ok, false);
  assert.ok(result.clickCandidates >= 1);
});

test("WAV wrapper preserves the PCM payload", () => {
  const pcm = makeTone(0.1);
  const wav = createPcm16Wav(pcm, sampleRate);
  assert.equal(wav.subarray(0, 4).toString("ascii"), "RIFF");
  assert.equal(wav.subarray(8, 12).toString("ascii"), "WAVE");
  assert.equal(wav.readUInt32LE(24), sampleRate);
  assert.deepEqual(wav.subarray(44), pcm);
});
