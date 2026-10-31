const { test } = require("node:test");
const assert = require("node:assert/strict");
const {
  observePreTransitionPeer,
  classifyObservedConnection,
} = require("../scripts/obs-websocket-vdoninja-source-check.cjs");

const bootstrap = { connected: true, transportKey: "bootstrap" };
const alpha = { connected: true, transportKey: "alpha" };
const replacement = { connected: true, transportKey: "replacement" };

test("alpha negotiation updates the baseline before the test action", () => {
  let baseline = observePreTransitionPeer(null, "pre", { peer: bootstrap });
  baseline = observePreTransitionPeer(baseline, "pre", { peer: alpha });
  assert.equal(classifyObservedConnection({ peer: alpha }, null, baseline), "pre");
});

test("missing or disconnected diagnostics do not erase the baseline", () => {
  for (const snapshot of [null, {}, { peer: { ...replacement, connected: false } }]) {
    assert.equal(observePreTransitionPeer(alpha, "pre", snapshot), alpha);
  }
});

test("a replacement cannot become the old connection once the action starts", () => {
  for (const phase of ["transition", "post"]) {
    const baseline = observePreTransitionPeer(alpha, phase, { peer: replacement });
    assert.equal(baseline, alpha);
    assert.equal(classifyObservedConnection({ peer: replacement }, null, baseline), "unknown");
    const evidence = { before: { peer: alpha }, after: { peer: replacement } };
    assert.equal(classifyObservedConnection({ peer: replacement }, evidence, baseline), "post");
    assert.equal(classifyObservedConnection({ peer: alpha }, evidence, baseline), "pre");
  }
});
