const { test } = require("node:test");
const assert = require("node:assert/strict");
const { analyzeClearedImage } = require("../scripts/analyze-cleared-screenshot.cjs");

test("cleared 1080p output passes with opaque alpha", () => {
  const image = { width: 1920, height: 1080, rgba: Buffer.alloc(1920 * 1080 * 4) };
  for (let offset = 3; offset < image.rgba.length; offset += 4) image.rgba[offset] = 255;
  assert.equal(analyzeClearedImage(image).ok, true);
});

test("a single stale colored pixel fails even in an otherwise black frame", () => {
  const image = { width: 1920, height: 1080, rgba: Buffer.alloc(1920 * 1080 * 4) };
  image.rgba[image.rgba.length - 3] = 255;
  const result = analyzeClearedImage(image);
  assert.equal(result.ok, false);
  assert.equal(result.nonBlackPixels, 1);
});
