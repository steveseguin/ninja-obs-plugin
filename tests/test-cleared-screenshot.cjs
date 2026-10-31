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

test("small black-level rounding is allowed, but brighter pixels fail", () => {
  const image = { width: 1, height: 1, rgba: Buffer.from([2, 1, 0, 255]) };
  assert.equal(analyzeClearedImage(image).ok, true);
  image.rgba[2] = 3;
  assert.equal(analyzeClearedImage(image).ok, false);
});

test("empty or truncated captures cannot pass as cleared output", () => {
  for (const image of [null, { width: 0, height: 0, rgba: Buffer.alloc(0) },
    { width: 2, height: 1, rgba: Buffer.alloc(4) }]) {
    assert.throws(() => analyzeClearedImage(image), /nonempty RGBA/);
  }
});

test("invalid tolerance cannot silently accept stale output", () => {
  const image = { width: 1, height: 1, rgba: Buffer.alloc(4) };
  for (const tolerance of [NaN, -1, 256, 1.5]) {
    assert.throws(() => analyzeClearedImage(image, tolerance), /tolerance/);
  }
});
