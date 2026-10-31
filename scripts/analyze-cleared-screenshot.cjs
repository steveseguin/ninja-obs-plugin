const fs = require("node:fs");
const { decodePng } = require("./obs-websocket-vdoninja-source-check.cjs");

function analyzeClearedImage(image, tolerance = 2) {
  if (!image || !Number.isSafeInteger(image.width) || image.width <= 0 ||
      !Number.isSafeInteger(image.height) || image.height <= 0 ||
      !(image.rgba instanceof Uint8Array) || image.rgba.length !== image.width * image.height * 4) {
    throw new Error("Expected a nonempty RGBA image matching its dimensions");
  }
  if (!Number.isInteger(tolerance) || tolerance < 0 || tolerance > 255) {
    throw new Error("Pixel tolerance must be an integer from 0 to 255");
  }
  let maximumChannel = 0;
  let nonBlackPixels = 0;
  for (let offset = 0; offset < image.rgba.length; offset += 4) {
    const maximum = Math.max(image.rgba[offset], image.rgba[offset + 1], image.rgba[offset + 2]);
    maximumChannel = Math.max(maximumChannel, maximum);
    if (maximum > tolerance) nonBlackPixels++;
  }
  return { ok: nonBlackPixels === 0, width: image.width, height: image.height, maximumChannel, nonBlackPixels };
}

if (require.main === module) {
  const result = analyzeClearedImage(decodePng(fs.readFileSync(process.argv[2])));
  console.log(JSON.stringify(result));
  process.exitCode = result.ok ? 0 : 1;
}

module.exports = { analyzeClearedImage };
