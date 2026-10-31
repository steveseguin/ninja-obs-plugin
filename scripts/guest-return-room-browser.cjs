// Review driver for a real browser director/guest room and the isolated OBS output.
// Media devices are synthetic; signaling, WebRTC, OBS encoding and playback are real.
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { chromium } = require('playwright');
const { ObsWebSocketClient } = require('./obs-websocket-vdoninja-source-check.cjs');
const output = path.resolve(process.argv[2]);
const state = JSON.parse(fs.readFileSync(path.join(output, 'state.json')));
process.env.OBS_WEBSOCKET_PASSWORD = state.websocketPassword;
const client = new ObsWebSocketClient(state.websocket);
const guests = [], browsers = [];
const report = { steps: [], guests: [], checks: {} };
const wait = ms => new Promise(r => setTimeout(r, ms));
let director, scene, receiver, connected = false;
function save() { fs.writeFileSync(path.join(output, 'room-results.json'), JSON.stringify(report, null, 2)); }
function tone(name, frequency) {
  const rate = 48000, samples = rate * 20, buffer = Buffer.alloc(44 + samples * 2);
  buffer.write('RIFF'); buffer.writeUInt32LE(buffer.length-8, 4); buffer.write('WAVEfmt ', 8);
  buffer.writeUInt32LE(16, 16); buffer.writeUInt16LE(1, 20); buffer.writeUInt16LE(1, 22);
  buffer.writeUInt32LE(rate, 24); buffer.writeUInt32LE(rate*2, 28); buffer.writeUInt16LE(2, 32);
  buffer.writeUInt16LE(16, 34); buffer.write('data', 36); buffer.writeUInt32LE(samples*2, 40);
  for (let i=0; i<samples; ++i) buffer.writeInt16LE(Math.round(4500*Math.sin(2*Math.PI*frequency*i/rate)), 44+i*2);
  const file = path.join(output, name+'.wav'); fs.writeFileSync(file, buffer); return file;
}
async function shot(page, name) {
  await page.screenshot({path:path.join(output,'screenshots',name+'-full.png'), fullPage:true});
  report.steps.push({name, url:page.url(), title:await page.title()}); save();
}
async function browser(frequency) {
  const args = ['--use-fake-ui-for-media-stream','--use-fake-device-for-media-stream',
    '--autoplay-policy=no-user-gesture-required','--disable-background-timer-throttling'];
  if(frequency) args.push('--use-file-for-fake-audio-capture='+tone('guest-'+frequency,frequency));
  const b = await chromium.launch({channel:'chrome',headless:true,args}); browsers.push(b);
  const context = await b.newContext({permissions:['camera','microphone'],viewport:{width:1440,height:1000}});
  return context.newPage();
}
async function ui(page) {
  return page.locator('button,input[type=button],input[type=submit]').evaluateAll(es=>es.filter(e=>e.offsetWidth&&e.offsetHeight)
    .map(e=>({id:e.id,text:e.innerText||e.value})));
}
async function spectrum(page, fast = false) {
  return page.evaluate(async(fast)=>{
    const result=[];
    for(const [uuid,pc] of Object.entries(session.rpcs||{})) {
      const video=pc.videoElement;
      const item={uuid,stream:pc.streamID,state:pc.connectionState, width:video&&video.videoWidth,
        height:video&&video.videoHeight, currentTime:video&&video.currentTime};
      try {
        const stats=await pc.getStats(); item.rtp=[];
        stats.forEach(s=>{if(s.type==='inbound-rtp')item.rtp.push({kind:s.kind,bytes:s.bytesReceived,frames:s.framesDecoded,energy:s.totalAudioEnergy});});
      }catch(e){item.statsError=String(e);}
      if(!fast&&video&&video.srcObject&&video.srcObject.getAudioTracks().length){
        const context=new AudioContext(); await context.resume();
        const source=context.createMediaStreamSource(video.srcObject), analyser=context.createAnalyser();
        analyser.fftSize=32768;source.connect(analyser);
        await new Promise(r=>setTimeout(r,800));
        const values=new Float32Array(analyser.frequencyBinCount);analyser.getFloatFrequencyData(values);
        item.tones={};for(const hz of [300,600,900,1200,1500]) {
          const bin=Math.round(hz*analyser.fftSize/context.sampleRate);
          item.tones[hz]=Math.max(...values.slice(bin-2,bin+3));
        }
        source.disconnect(); await context.close();
      }
      result.push(item);
    }
    return {stream:session.streamID,broadcast:session.broadcast, peers:result};
  }, fast);
}
async function command(action, args) {
  if(action==='init') {
    if (!connected) { await client.connect(); connected = true; }
    report.obs=await client.request('GetVersion');
    scene=(await client.request('GetCurrentProgramScene')).currentProgramSceneName;
    for(const input of (await client.request('GetInputList')).inputs) await client.request('RemoveInput',{inputName:input.inputName});
    await wait(1500); // OBS releases removed source names asynchronously.
    await client.request('CreateInput',{sceneName:scene,inputName:'Program background',inputKind:'color_source_v3',
      inputSettings:{color:0xff30231b,width:1280,height:720},sceneItemEnabled:true});
    await client.request('CreateInput',{sceneName:scene,inputName:'Program title',inputKind:'text_gdiplus_v3',
      inputSettings:{text:'CREATOR ROUNDTABLE | OBS PROGRAM',font:{face:'Arial',size:40,flags:1},color:0xffe6e6e6},sceneItemEnabled:true});
    for(const [name,hz] of [['Host microphone',1500],['Shared clip',1200]]) {
      await client.request('CreateInput',{sceneName:scene,inputName:name,inputKind:'ffmpeg_source',
        inputSettings:{local_file:tone(name,hz),is_local_file:true,looping:true},sceneItemEnabled:true});
      await client.request('SetInputAudioTracks',{inputName:name,inputAudioTracks:{1:true,2:true,3:false,4:false,5:false,6:false}});
    }
    director=await browser();
    await director.goto('https://vdo.ninja/?director='+state.room+'&password='+state.password,{waitUntil:'domcontentloaded'});
    await wait(6000);await shot(director,'03-director-room');
    report.browser=await director.context().browser().version();save();return ui(director);
  }
  if(action==='join') {
    const index=guests.length, name=['Alice','Ben','Casey','Dana'][index] || 'Guest '+String(index+1).padStart(2,'0'), frequency=[300,600,900,1800][index] || 300+30*index;
    const page=await browser(frequency);
    const invite=fs.readFileSync(path.join(output,'invite.txt'),'utf8').trim();
    await page.goto(invite+'&label='+encodeURIComponent(name)+(args.auto?'&webcam&autostart&width=320&height=180&fps=10&bitrate=200':''),{waitUntil:'domcontentloaded'});await wait(5000);
    guests.push({name,page,frequency});await shot(page,`04-${name}-invitation`);return ui(page);
  }
  if(action==='click') {
    const page=args.guest===undefined?director:guests[args.guest].page;
    // VDO.Ninja's START button pulses continuously. It remains visible and
    // clickable, but cannot satisfy Playwright's animation-stability check.
    await page.locator(args.selector).click({force:true});await wait(4000);
    await shot(page,args.name);return ui(page);
  }
  if(action==='ingest') {
    const guest=guests[args.guest];guest.stream=await guest.page.evaluate(()=>session.streamID);
    assert(guest.stream);
    const url='https://vdo.ninja/?view='+encodeURIComponent(guest.stream)+'&room='+state.room+'&password='+state.password+'&solo&cleanoutput';
    const input=await client.request('CreateInput',{sceneName:scene,inputName:guest.name,inputKind:'browser_source',
      inputSettings:{url,width:640,height:360,reroute_audio:true,shutdown:false},sceneItemEnabled:true});
    await client.request('SetSceneItemTransform',{sceneName:scene,sceneItemId:input.sceneItemId,
      sceneItemTransform:args.grid?{positionX:(args.guest%5)*256,positionY:50+Math.floor(args.guest/5)*166,scaleX:.39,scaleY:.39}:
        {positionX:args.guest===1?640:0,positionY:args.guest===2?400:70,scaleX:.9,scaleY:.9}});
    await client.request('SetInputAudioTracks',{inputName:guest.name,inputAudioTracks:{1:true,2:false,3:false,4:false,5:false,6:false}});
    report.guests.push({name:guest.name,stream:guest.stream,frequency:guest.frequency});save();return {name:guest.name,stream:guest.stream};
  }
  if(action==='snap') {
    if(Number.isInteger(args.guest)) {
      const guest=guests[args.guest];
      await shot(guest.page,args.name+'-'+guest.name);return {ok:true};
    }
    await shot(director,args.name+'-director');
    for(const guest of guests) await shot(guest.page,args.name+'-'+guest.name);
    return {ok:true};
  }
  if(action==='probe') {
    const probes=[];for(const g of guests)probes.push({name:g.name,...await spectrum(g.page,args.fast)});
    if(args.director)probes.unshift({name:'Director',...await spectrum(director,args.fast)});
    report.checks[args.name]=probes;save();return probes;
  }
  if(action==='reload') {
    await guests[args.guest].page.reload({waitUntil:'domcontentloaded'});await wait(5000);
    return ui(guests[args.guest].page);
  }
  if(action==='request')return client.request(args.type,args.data||{});
  if(action==='stream') {
    const ffmpeg=path.resolve('build-plugin-release-validation/deps/obs-studio/.deps/obs-deps-2026-07-15-x64/bin/ffmpeg.exe');
    const log=fs.openSync(path.join(output,'rtmp-receiver.log'),'w');
    receiver=spawn(ffmpeg,['-y','-listen','1','-i','rtmp://127.0.0.1:19355/live/room-review','-c','copy',path.join(output,'public-stream.mkv')],
      {stdio:['ignore',log,log],windowsHide:true});
    await wait(1000);await client.request('StartStream');await wait(5000);
    report.mainStream=await client.request('GetStreamStatus');save();return report.mainStream;
  }
  if(action==='close') {
    for(const b of browsers)await b.close();
    try {
      if((await client.request('GetStreamStatus')).outputActive)await client.request('StopStream');
    } catch(e) { report.cleanupNote=String(e); } // OBS may already have closed for persistence checks.
    await wait(2000);client.close();save();setTimeout(()=>process.exit(0),500);return {ok:true};
  }
  throw Error('Unknown action '+action);
}
http.createServer(async(req,res)=>{
  let data='';for await(const part of req)data+=part;
  try {const request=JSON.parse(data||'{}'); const result=await command(request.action,request.args||{});
    res.setHeader('Content-Type','application/json');res.end(JSON.stringify(result));}
  catch(e){report.lastError=e.stack;save();res.statusCode=500;res.end(JSON.stringify({error:e.stack}));}
}).listen(19356,'127.0.0.1',()=>console.log('Review controller ready on 19356'));
