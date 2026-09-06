// Drain the native audio processor on its transferred worker queue. Preserve the
// browser's default queue capacity; main-thread stalls must not lose probe data.
function installRawAudioWorker() {
  let reader, active = true;
  onmessage = async ({data}) => {
    if (data === 'stop') {
      active = false;
      if (reader) await reader.cancel().catch(()=>{});
      return;
    }
    reader = data.readable.getReader();
    const result = {chunks:[],sampleCount:0,sampleRate:0,firstTimestamp:null,lastTimestamp:null,
      lastDuration:null,maxTimestampStep:0,nonForwardTimestamps:0,timestampGaps:[],error:null};
    try {
      while (active) {
        const {value,done} = await reader.read();
        if (done) break;
        try {
          const samples = new Float32Array(value.numberOfFrames);
          value.copyTo(samples,{planeIndex:0,format:'f32-planar'});
          const chunkStartSample = result.sampleCount;
          result.chunks.push(samples);result.sampleCount += samples.length;result.sampleRate=value.sampleRate;
          if(result.firstTimestamp === null) result.firstTimestamp=value.timestamp;
          else {
            const step=value.timestamp-result.lastTimestamp;
            result.maxTimestampStep=Math.max(result.maxTimestampStep,step);
            if(step<=0)result.nonForwardTimestamps++;
            if(step>Math.max(12000,Number(result.lastDuration||value.duration||0)+2000)&&result.timestampGaps.length<50)
              result.timestampGaps.push({chunkStartSample,captureTimeSeconds:chunkStartSample/result.sampleRate,
                previousTimestamp:result.lastTimestamp,timestamp:value.timestamp,timestampStep:step,
                previousDuration:result.lastDuration,receiveWallTimeMs:Date.now()});
          }
          result.lastTimestamp=value.timestamp;result.lastDuration=value.duration;
        } finally {value.close();}
      }
    } catch(error){result.error=String(error.stack||error);}
    postMessage({result},result.chunks.map(chunk=>chunk.buffer));
  };
}
const rawAudioWorkerSource = `(${installRawAudioWorker.toString()})()`;
module.exports = {rawAudioWorkerSource};
