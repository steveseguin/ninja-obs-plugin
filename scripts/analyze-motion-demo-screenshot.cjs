const fs = require("node:fs");
const { decodePng } = require("./obs-websocket-vdoninja-source-check.cjs");
const { analyzeClearedImage } = require("./analyze-cleared-screenshot.cjs");

function analyzeMotionDemoImage(image) {
  analyzeClearedImage(image); // Reject empty/truncated image data.
  let colorfulPixels = 0;
  for (let offset = 0; offset < image.rgba.length; offset += 4) {
    const r = image.rgba[offset], g = image.rgba[offset + 1], b = image.rgba[offset + 2];
    if (Math.max(r, g, b) > 80 && Math.max(r, g, b) - Math.min(r, g, b) > 40) colorfulPixels++;
  }
  // The fixture's large colored circles occupy far more area than browser chrome.
  // A gray capture window, black frame or small colored title icon must fail.
  const colorfulFraction = colorfulPixels / (image.width * image.height);
  return { ok: colorfulFraction >= 0.01, width: image.width, height: image.height, colorfulPixels, colorfulFraction };
}

if (require.main === module) {
  const result = analyzeMotionDemoImage(decodePng(fs.readFileSync(process.argv[2])));
  console.log(JSON.stringify(result));
  process.exitCode = result.ok ? 0 : 1;
}

module.exports = { analyzeMotionDemoImage };
