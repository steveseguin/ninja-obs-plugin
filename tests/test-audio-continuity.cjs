const test = require("node:test");
const assert = require("node:assert/strict");
const {
  analyzePcm16Le,
  createPcm16Wav,
} = require("./tools/audio-continuity-analysis.cjs");

const sampleRate = 48000;
const toneHz = 997;

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
