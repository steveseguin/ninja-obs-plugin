const assert = require("node:assert/strict");
const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { test } = require("node:test");

// Real FFmpeg integration: video begins after audio, as in OBS recordings.
// Preserve that offset without synthesizing frames in either fixture or probe.
function analyze(t, duplicate) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "obs-sequence-"));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const input = path.join(directory, "offset.mkv");
  const frames = [];
  for (let index = 0; index < 30; index++) {
    const marker = duplicate && index === 15 ? 14 : index;
    const bits = 0xd3.toString(2).padStart(8, "0") +
      marker.toString(2).padStart(16, "0") +
      ((~marker) & 255).toString(2).padStart(8, "0");
    const frame = Buffer.alloc(320 * 180, 80);
    for (let y = 150; y < 175; y++) {
      for (let bit = 0; bit < 32; bit++) {
        frame.fill(bits[bit] === "1" ? 255 : 0,
          y * 320 + 32 + bit * 8, y * 320 + 32 + (bit + 1) * 8);
      }
    }
    frames.push(frame);
  }
  const encoded = spawnSync(process.env.FFMPEG || "ffmpeg", [
    "-v", "error", "-y", "-itsoffset", "0.067", "-f", "rawvideo",
    "-pixel_format", "gray", "-video_size", "320x180", "-framerate", "30",
    "-i", "pipe:0", "-f", "lavfi", "-i", "anullsrc=r=48000:cl=mono",
    "-map", "0:v", "-map", "1:a", "-c:v", "ffv1", "-fps_mode", "passthrough",
    "-c:a", "pcm_s16le", "-t", "1.1", input,
  ], { input: Buffer.concat(frames), encoding: "utf8" });
  assert.ifError(encoded.error);
  assert.equal(encoded.status, 0, encoded.stderr);
  const result = spawnSync(process.execPath, [
    path.join(__dirname, "../scripts/analyze-publish-sequence-video.cjs"), input,
  ], { encoding: "utf8" });
  assert.ifError(result.error);
  return { status: result.status, report: JSON.parse(result.stdout) };
}

test("recording analysis does not manufacture frames for an audio/video start offset", t => {
  const { status, report } = analyze(t, false);
  assert.equal(status, 0);
  assert.equal(report.ok, true);
  assert.equal(report.frameCount, 30);
  assert.equal(report.markerErrors, 0);
});

test("recording analysis still rejects actual duplicated and skipped markers", t => {
  const { status, report } = analyze(t, true);
  assert.equal(status, 1);
  assert.equal(report.ok, false);
  assert.equal(report.frameCount, 30);
  assert.equal(report.markerDuplicates, 1);
  assert.equal(report.markerSkipped, 1);
});
