const childProcess = require("child_process");
const {
  analyzeVisualSequence,
} = require("../tests/tools/presentation-continuity-analysis.cjs");

function crc8(value) {
  let crc = 0;
  for (const byte of [(value >>> 8) & 255, value & 255]) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = crc & 0x80 ? ((crc << 1) ^ 0x07) & 255 : (crc << 1) & 255;
    }
  }
  return crc;
}

function grayToBinary(gray) {
  let binary = gray;
  for (let shift = 1; shift < 16; shift <<= 1) {
    binary ^= binary >>> shift;
  }
  return binary & 0xffff;
}

function decodeMarker(luma, format) {
  const minimum = Math.min(...luma);
  const maximum = Math.max(...luma);
  if (maximum - minimum < 64) {
    return { markerFrame: null, markerError: "low-contrast" };
  }
  const threshold = (minimum + maximum) / 2;
  let sync = 0;
  let payload = 0;
  let checksum = 0;
  for (let bit = 0; bit < 8; bit += 1) {
    sync = (sync << 1) | (luma[bit] > threshold ? 1 : 0);
  }
  for (let bit = 8; bit < 24; bit += 1) {
    payload = (payload << 1) | (luma[bit] > threshold ? 1 : 0);
  }
  for (let bit = 24; bit < 32; bit += 1) {
    checksum = (checksum << 1) | (luma[bit] > threshold ? 1 : 0);
  }
  if (sync !== 0xd3) {
    return { markerFrame: null, markerError: "sync" };
  }
  const markerFrame =
    format === "gray-crc" ? grayToBinary(payload) : payload;
  const expectedChecksum =
    format === "gray-crc" ? crc8(markerFrame) : (~markerFrame) & 255;
  if (checksum !== expectedChecksum) {
    return { markerFrame: null, markerError: "checksum" };
  }
  return { markerFrame, markerError: "" };
}

async function main() {
  const input = process.argv[2];
  const format = process.argv[3] || "counter-complement";
  if (!input) {
    throw new Error(
      "Usage: node scripts/analyze-publish-sequence-video.cjs VIDEO [counter-complement|gray-crc]",
    );
  }
  const ffmpeg = childProcess.spawn(
    process.env.FFMPEG || "ffmpeg",
    [
      "-v",
      "error",
      "-i",
      input,
      "-map",
      "0:v:0",
      "-vf",
      "crop=iw*0.8:ih*(96/1080):iw*0.1:ih*(920/1080),scale=32:1:flags=area,format=gray",
      // Inspect decoded frames exactly once; default CFR output can insert
      // duplicates when the video starts later than the audio in a recording.
      "-fps_mode",
      "passthrough",
      "-f",
      "rawvideo",
      "-",
    ],
    { stdio: ["ignore", "pipe", "inherit"] },
  );
  const records = [];
  let pending = Buffer.alloc(0);
  for await (const chunk of ffmpeg.stdout) {
    pending = Buffer.concat([pending, chunk]);
    while (pending.length >= 32) {
      records.push(decodeMarker([...pending.subarray(0, 32)], format));
      pending = pending.subarray(32);
    }
  }
  const status = await new Promise((resolve, reject) => {
    ffmpeg.once("error", reject);
    ffmpeg.once("close", resolve);
  });
  if (status !== 0) {
    throw new Error(`ffmpeg exited with status ${status}`);
  }
  const analysis = analyzeVisualSequence(records);
  console.log(JSON.stringify({ input, format, ...analysis }, null, 2));
  process.exitCode = analysis.ok ? 0 : 1;
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
