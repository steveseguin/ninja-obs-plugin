// Minimal audio-only source scheduling control: no OBS, encoder, RTP or relay.
// Usage: VDONINJA_BROWSER_EXECUTABLE=... node scripts/browser-audio-fifo-repro.cjs OUTPUT DELAY_MS
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const {execFileSync} = require('node:child_process');
const {chromium} = require('playwright');
const {startDecodedAudioCapture,stopDecodedAudioCapture} = require('./obs-websocket-vdoninja-publish-check.cjs');
const {analyzePcm16Le,createPcm16Wav} = require('../tests/tools/audio-continuity-analysis.cjs');
const sleep = ms => new Promise(resolve=>setTimeout(resolve,ms));
async function main() {
  const output = path.resolve(process.argv[2]);
  const delayMs = Number(process.argv[3]);
  if (!Number.isFinite(delayMs) || delayMs<0 || delayMs>20) throw new Error('Require delay from 0 to 20 ms');
  const durationMs=Number(process.env.VDONINJA_AUDIO_REPRO_DURATION_MS||60000);
  const stallMs=Number(process.env.VDONINJA_AUDIO_MAIN_THREAD_STALL_MS||0);
  if(!Number.isFinite(durationMs)||durationMs<5000||durationMs>120000||!Number.isFinite(stallMs)||stallMs<0||stallMs>1000)
    throw new Error('Invalid reproduction duration or main-thread stall');
  fs.mkdirSync(output,{recursive:true});
  const pactl = (...args)=>execFileSync('pactl',args,{encoding:'utf8'}).trim();
  const sink = `ninja-fifo-${process.pid}`,defaultBefore=pactl('get-default-sink');
  const moduleId=pactl('load-module','module-null-sink',`sink_name=${sink}`,'rate=48000','channels=1','channel_map=mono');
  const server=http.createServer((req,res)=>{res.setHeader('Content-Type','text/html');res.end('<!doctype html><video muted autoplay></video>');});
  let browser;
  const report={ok:false,delayMs,stepAtSourceMs:30000,durationMs,stallMs,
    scope:'Generated continuous 997 Hz audio with a scheduled source-delivery phase change; no networking or OBS.'};
  try {
    if(pactl('get-default-sink')!==defaultBefore){pactl('set-default-sink',defaultBefore);throw new Error('Default sink changed unexpectedly');}
    report.clockAnchor={epochMs:Date.now(),monotonicUs:Number(process.hrtime.bigint()/1000n)};
    await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
    browser=await chromium.launch({executablePath:process.env.VDONINJA_BROWSER_EXECUTABLE,
      ignoreDefaultArgs:['--mute-audio'],
      env:{...process.env,PULSE_SINK:sink,CHROME_LOG_FILE:path.join(output,'chromium-process.log')},
      args:['--autoplay-policy=no-user-gesture-required','--enable-logging','--force-fieldtrials=WebRTC-AudioFifoTrace/Enabled/']});
    report.browserVersion=browser.version();
    const page=await browser.newPage();await page.goto(`http://127.0.0.1:${server.address().port}`);
    report.sourceSetup=await page.evaluate(async ({delayMs,durationMs})=>{
      if(typeof MediaStreamTrackGenerator!=='function')throw new Error('Audio track generator is unavailable');
      const generator=new MediaStreamTrackGenerator({kind:'audio'});
      document.querySelector('video').srcObject=new MediaStream([generator]);
      const workerSource=`onmessage=async({data:{writable,delayMs,durationMs}})=>{
        const writer=writable.getWriter(),start=performance.now()+100;
        let last=null;const diagnostics={startEpochMs:performance.timeOrigin+start,blocks:0,maxLatenessMs:0,maxDeliveryGapMs:0,lateDeliveries:[],nearStep:[]};
        const totalBlocks=Math.ceil((durationMs+3000)/10);
        for(let i=0;i<totalBlocks;i++){
          const deadline=start+i*10+(i>=3000?delayMs:0);
          await new Promise(resolve=>setTimeout(resolve,Math.max(0,deadline-performance.now())));
          const now=performance.now();
          diagnostics.maxLatenessMs=Math.max(diagnostics.maxLatenessMs,now-deadline);
          if(last!==null)diagnostics.maxDeliveryGapMs=Math.max(diagnostics.maxDeliveryGapMs,now-last);
          if(i>=2995&&i<=3005)diagnostics.nearStep.push({block:i,deadline,now,gap:last===null?null:now-last});
          if(last!==null&&now-last>11&&diagnostics.lateDeliveries.length<200)
            diagnostics.lateDeliveries.push({block:i,epochMs:performance.timeOrigin+now,gapMs:now-last,latenessMs:now-deadline});
          last=now;
          const samples=new Float32Array(480);
          for(let j=0;j<480;j++)samples[j]=0.125*Math.sin(2*Math.PI*997*(i*480+j)/48000);
          const frame=new AudioData({format:'f32-planar',sampleRate:48000,numberOfFrames:480,numberOfChannels:1,timestamp:i*10000,data:samples});
          await writer.write(frame);frame.close();diagnostics.blocks++;
          if(i%100===0||i===totalBlocks-1)postMessage(diagnostics);
        }
        await writer.close();
      };`;
      const worker=new Worker(URL.createObjectURL(new Blob([workerSource],{type:'text/javascript'})));
      window.__sourceWorker=worker;window.__generator=generator;
      worker.onmessage=e=>{window.__sourceDiagnostics=e.data;};
      worker.onerror=e=>{window.__sourceError=e.message;};
      worker.postMessage({writable:generator.writable,delayMs,durationMs},[generator.writable]);
      return {generator:generator.kind,sampleRate:48000,blockFrames:480};
    },{delayMs,durationMs});
    await page.waitForFunction(()=>window.__sourceDiagnostics?.blocks>0||window.__sourceError,{},{timeout:5000});
    const sourceError=await page.evaluate(()=>window.__sourceError);
    if(sourceError)throw new Error(sourceError);
    await sleep(1000);
    report.captureStartEpochMs=Date.now();
    report.audioSetup=await startDecodedAudioCapture(page);
    const deadline=Date.now()+report.durationMs;
    if(stallMs){
      await sleep(3000);
      await page.evaluate(ms=>{const end=performance.now()+ms;while(performance.now()<end){}},stallMs);
    }
    await sleep(Math.max(0,deadline-Date.now()));
    const capture=await stopDecodedAudioCapture(page);
    report.sourceDiagnostics=await page.evaluate(()=>({schedule:window.__sourceDiagnostics,error:window.__sourceError}));
    report.taps={};
    for(const [name,tap] of [['webAudio',capture],['rawTrack',capture.rawTrack]]){
      if(!tap)continue;
      const pcm=Buffer.from(tap.pcmBase64,'base64');
      fs.writeFileSync(path.join(output,`${name}.wav`),createPcm16Wav(pcm,tap.sampleRate));
      delete tap.pcmBase64;tap.analysis=analyzePcm16Le(pcm,{sampleRate:tap.sampleRate,toneHz:997});
      report.taps[name]=tap;
    }
    report.ok=Boolean(report.taps.rawTrack?.analysis.ok&&report.taps.webAudio.analysis.ok&&!report.sourceDiagnostics.error);
    await page.evaluate(()=>{window.__sourceWorker.terminate();window.__generator.stop();});
  } catch(error){report.error=String(error.stack||error);}
  finally {
    if(browser)await browser.close();
    await new Promise(resolve=>server.close(resolve));
    pactl('unload-module',moduleId);report.defaultSinkRestored=pactl('get-default-sink')===defaultBefore;
    report.ok=report.ok&&report.defaultSinkRestored;
    fs.writeFileSync(path.join(output,'report.json'),JSON.stringify(report,null,2));
  }
  console.log(JSON.stringify({ok:report.ok,error:report.error,raw:report.taps?.rawTrack?.analysis.ok,web:report.taps?.webAudio?.analysis.ok}));
  if(report.error)process.exitCode=1; // Measurement failures stay in the report.
}
main().catch(error=>{console.error(error);process.exitCode=1;});
