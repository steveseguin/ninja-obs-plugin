const { test } = require("node:test");
const assert = require("node:assert/strict");
const { analyzeMotionDemoImage } = require("../scripts/analyze-motion-demo-screenshot.cjs");

test("gray browser window with a small colored icon is not the motion fixture", () => {
  const image = { width: 100, height: 100, rgba: Buffer.alloc(100 * 100 * 4, 60) };
  image.rgba[0] = 255;
  assert.equal(analyzeMotionDemoImage(image).ok, false);
});

test("rendered colorful fixture passes", () => {
  const image = { width: 100, height: 100, rgba: Buffer.alloc(100 * 100 * 4) };
  for (let offset = 0; offset < 1000 * 4; offset += 4) image.rgba[offset] = 255;
  assert.equal(analyzeMotionDemoImage(image).ok, true);
});

test("black or missing frames fail", () => {
  assert.equal(analyzeMotionDemoImage({ width: 1, height: 1, rgba: Buffer.alloc(4) }).ok, false);
  assert.throws(() => analyzeMotionDemoImage({ width: 0, height: 0, rgba: Buffer.alloc(0) }));
});
