const test = require('node:test');
const assert = require('node:assert/strict');
const { selectMeasuredPcm } = require('./tools/audio-sink-coverage.cjs');
const pcm = Buffer.alloc(40);
for(let i=0;i<20;i++) pcm.writeInt16LE(i,i*2);
test('wall-clock window clips both boundary packets without discarding interior samples',()=>{
  const r=selectMeasuredPcm(pcm,[{pts_time:1,size:20},{pts_time:1.01,size:20}],1000,1005,1015);
  assert.deepEqual(r.measured,pcm.subarray(10,30));
  assert.equal(r.timestampCoverageOk,true);
});
test('packet timestamp gap fails coverage even when concatenated PCM has enough samples',()=>{
  const r=selectMeasuredPcm(pcm,[{pts_time:1,size:20},{pts_time:1.015,size:20}],1000,1000,1025);
  assert.equal(r.measured.length,40);
  assert.equal(r.timestampCoverageOk,false);
  assert.equal(r.packetGapsMs.length,1);
  assert.ok(Math.abs(r.packetGapsMs[0]-5)<1e-9);
});
test('unmapped PCM bytes cannot satisfy sink coverage',()=>{
  assert.throws(()=>selectMeasuredPcm(pcm,[{pts_time:1,size:20}],1000,1000,1010),/coverage differs/);
});
