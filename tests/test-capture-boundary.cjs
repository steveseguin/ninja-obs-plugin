const assert = require("node:assert/strict");
const { test } = require("node:test");
const vm = require("node:vm");
const { freezeContinuityCapture } = require("./tools/capture-boundary.cjs");

test("both probes stop before either reader cancellation can delay artifact export", async () => {
  let finishVideo, finishAudio;
  const video = { active: true,
    processorDone: new Promise(resolve => { finishVideo = resolve; }) };
  const audio = { processor: { onaudioprocess() {} }, rawTrack: { active: true,
    loop: new Promise(resolve => { finishAudio = resolve; }) } };
  let cancellations = 0;
  const checkStopped = () => {
    assert.equal(video.active, false);
    assert.equal(audio.rawTrack.active, false);
    assert.equal(audio.processor.onaudioprocess, null);
    cancellations++;
    return Promise.resolve();
  };
  video.processorReader = { cancel: checkStopped };
  audio.rawTrack.reader = { cancel: checkStopped };
  let finished = false;
  const boundary = vm.runInNewContext(`(${freezeContinuityCapture})()`, {
    window: { __vdoninjaPresentationCapture: video, __vdoninjaDecodedAudioCapture: audio },
  }).then(() => { finished = true; });
  assert.equal(cancellations, 2);
  finishVideo();
  await Promise.resolve();
  assert.equal(finished, false, "audio must finish before records are serialized");
  finishAudio();
  await boundary;
  assert.equal(finished, true);
});

test("disabled capture probes require no cleanup", async () => {
  await vm.runInNewContext(`(${freezeContinuityCapture})()`, { window: {} });
});

test("worker and worklet capture freeze before video export without a main-thread audio reader", async () => {
  let finishPcm, finishRaw;
  const video={active:true,processorDone:Promise.resolve()};
  const raw={active:true,loop:new Promise(resolve=>{finishRaw=resolve;})};
  let requested=false;
  const audio={processor:{},rawTrack:raw,freeze(){
    assert.equal(video.active,false);assert.equal(raw.active,false);requested=true;
    return new Promise(resolve=>{finishPcm=resolve;});
  }};
  video.processorReader={cancel(){assert.equal(requested,true);return Promise.resolve();}};
  let finished=false;
  const pending=vm.runInNewContext(`(${freezeContinuityCapture})()`,{
    window:{__vdoninjaPresentationCapture:video,__vdoninjaDecodedAudioCapture:audio},
  }).then(()=>{finished=true;});
  finishPcm();await Promise.resolve();assert.equal(finished,false);
  finishRaw();await pending;assert.equal(finished,true);
});
