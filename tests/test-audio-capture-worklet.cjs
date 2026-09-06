const test=require('node:test');
const assert=require('node:assert/strict');
const vm=require('node:vm');
const {pcmCaptureWorkletSource}=require('./tools/audio-capture-worklet.cjs');
function fixture(){
  const messages=[];let Processor;
  class Base {constructor(){this.port={postMessage(message){messages.push(message);}};}}
  vm.runInNewContext(pcmCaptureWorkletSource,{AudioWorkletProcessor:Base,registerProcessor(name,ctor){Processor=ctor;}});
  return {processor:new Processor(),messages};
}
test('PCM worklet preserves exact samples across full and partial chunks',()=>{
  const {processor,messages}=fixture();const expected=[];
  for(let n=0;n<33;n++){
    const input=Float32Array.from({length:128},(_,i)=>Math.sin((n*128+i)/20));expected.push(...input);
    assert.equal(processor.process([[input]]),true);
  }
  processor.port.onmessage({data:'flush'});
  assert.deepEqual(messages.filter(m=>m.samples).flatMap(m=>Array.from(m.samples)),expected);
  assert.equal(messages.at(-1).done,true);assert.equal(processor.process([]),false);
});
test('PCM worklet retains missing input as silence rather than hiding an audio dropout',()=>{
  const {processor,messages}=fixture();processor.process([]);processor.port.onmessage({data:'flush'});
  assert.equal(messages[0].samples.length,128);assert.ok(messages[0].samples.every(x=>x===0));
});
