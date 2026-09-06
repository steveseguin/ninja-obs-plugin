const test=require('node:test');
const assert=require('node:assert/strict');
const vm=require('node:vm');
const {rawAudioWorkerSource}=require('./tools/raw-audio-capture-worker.cjs');
function fixture(){
  let result;const sandbox={postMessage(message){result=message.result;}};
  vm.createContext(sandbox);vm.runInContext(rawAudioWorkerSource,sandbox);
  return {sandbox,result:()=>result};
}
function frame(timestamp,closed){
  return {numberOfFrames:480,sampleRate:48000,timestamp,duration:10000,
    copyTo(samples){samples.fill(0.25);},close(){closed.count++;}};
}
test('raw audio worker preserves samples and timestamp gaps and closes frames',async()=>{
  const {sandbox,result}=fixture(),closed={count:0};
  const readable=new ReadableStream({start(c){c.enqueue(frame(10000,closed));c.enqueue(frame(30000,closed));c.close();}});
  await sandbox.onmessage({data:{readable}});
  assert.equal(result().sampleCount,960);assert.equal(closed.count,2);
  assert.ok(result().chunks.every(chunk=>chunk.every(x=>x===0.25)));
  assert.equal(result().timestampGaps[0].timestampStep,20000);
});
test('raw audio worker cancellation exports the samples collected before stop',async()=>{
  const {sandbox,result}=fixture(),closed={count:0};
  const readable=new ReadableStream({start(c){c.enqueue(frame(10000,closed));}});
  const running=sandbox.onmessage({data:{readable}});
  await Promise.resolve();
  await sandbox.onmessage({data:'stop'});await running;
  assert.equal(result().sampleCount,480);assert.equal(closed.count,1);assert.equal(result().error,null);
});
