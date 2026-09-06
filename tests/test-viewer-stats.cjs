const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const { collectViewerSnapshot } = require('../scripts/obs-websocket-vdoninja-publish-check.cjs');
async function snapshot(pair) {
  const stats = new Map([
    ['transport', { id:'transport', type:'transport', selectedCandidatePairId:'pair' }],
    ['pair', { id:'pair', type:'candidate-pair', state:'succeeded', localCandidateId:'local',
      remoteCandidateId:'remote', ...pair }],
    ['local', { id:'local', type:'local-candidate', candidateType:'relay', protocol:'udp' }],
    ['remote', { id:'remote', type:'remote-candidate', candidateType:'relay', protocol:'udp' }],
  ]);
  return collectViewerSnapshot({ evaluate: fn => vm.runInNewContext(`(${fn.toString()})()`, {
    window:{ __pcList:[{ connectionState:'connected', getStats:async () => stats }] },
    document:{ querySelectorAll:() => [], title:'test', body:null }, location:{ href:'http://localhost/' },
  }) });
}
test('transport-selected relay is recognized without optional pair flags', async () => {
  const result = await snapshot({ selected:false, nominated:false });
  assert.equal(result.pcStats[0].selectedCandidatePairs.length,1);
  assert.equal(result.pcStats[0].selectedCandidatePairs[0].localCandidateType,'relay');
});
test('failed pair cannot establish relay coverage even when referenced by transport', async () => {
  const result = await snapshot({ state:'failed', nominated:true });
  assert.equal(result.pcStats[0].selectedCandidatePairs.length,0);
});
