const test = require('node:test');
const assert = require('node:assert/strict');
const { verifyExpectedEncoderMode } = require('./tools/obs-encoder-validation.cjs');

function clientWithMode(mode) {
  return { async request(type, data) {
    assert.equal(type, 'GetProfileParameter');
    assert.deepEqual(data, { parameterCategory: 'Output', parameterName: 'Mode' });
    return { parameterValue: mode };
  } };
}

test('rejects checking an inactive encoder configuration in either mode', async () => {
  await assert.rejects(verifyExpectedEncoderMode(clientWithMode('Simple'), '', 'obs_x264'), /Mode=Advanced.*observed Simple/);
  await assert.rejects(verifyExpectedEncoderMode(clientWithMode('Advanced'), 'x264', ''), /Mode=Simple.*observed Advanced/);
});

test('accepts encoder expectations for the active output mode', async () => {
  await verifyExpectedEncoderMode(clientWithMode('Simple'), 'x264', '');
  await verifyExpectedEncoderMode(clientWithMode('Advanced'), '', 'obs_x264');
});

test('rejects conflicting expectations before querying OBS', async () => {
  await assert.rejects(verifyExpectedEncoderMode(null, 'x264', 'obs_x264'), /not both/);
});

test('preserves runs without an encoder expectation', async () => {
  await verifyExpectedEncoderMode(null, '', '');
});
