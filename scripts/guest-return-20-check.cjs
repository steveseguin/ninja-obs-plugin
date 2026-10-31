// Twenty synthetic camera/microphone guests in a real VDO.Ninja room.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {execFileSync}=require('node:child_process');
const output=path.resolve(process.argv[2]);
const state=JSON.parse(fs.readFileSync(path.join(output,'state.json')));
const report={guestCount:20,syntheticDevices:true,publicDestination:'local RTMP recorder',phases:[]};
const wait=ms=>new Promise(r=>setTimeout(r,ms));
function ui(action,...args){return execFileSync('python',['scripts/guest-return-room-ui.py',output,action,...args],{encoding:'utf8',windowsHide:true})}
async function command(action,args={}) {
  const res=await fetch('http://127.0.0.1:19356',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action,args}),signal:AbortSignal.timeout(300000)});
  const data=await res.json();if(!res.ok)throw new Error(JSON.stringify(data));return data;
}
function save(){fs.writeFileSync(path.join(output,'twenty-results.json'),JSON.stringify(report,null,2))}
async function main(){
  if(!process.env.RETURN_TWENTY_RESUME) {
  if(/GuestReturnStop[^\n]*True/.test(ui('status')))ui('click','GuestReturnStop');
  ui('resize','600','1100');
  ui('set',JSON.stringify({GuestReturnICEServers:'turn:127.0.0.1:9|invalid|invalid',GuestReturnForceTURN:true}));
  ui('click','GuestReturnStart');await wait(30000);
  report.unreachableRelay=await command('probe',{name:'unreachable-relay',fast:true,director:true});
  assert(report.unreachableRelay[0].peers.filter(p=>p.stream===state.stream).every(p=>p.state!=='connected'),'Unreachable relay must not fall back to a direct route');
  ui('click','GuestReturnStop');
  ui('set',JSON.stringify({GuestReturnBitrate:1500,GuestReturnMaxViewers:22,GuestReturnForceTURN:false,GuestReturnICEServers:''}));
  ui('combo','GuestReturnSize','Up to 540p');ui('combo','GuestReturnFrameRate','OBS frame rate / 2');
  ui('click','GuestReturnAdvanced');
  ui('resize','600','1100');
  ui('capture','24-twenty-settings','VDONinjaGuestReturn');
  ui('click','GuestReturnCopyInvite');ui('clipboard','invite.txt');
  ui('click','GuestReturnStart');
  await command('stream');
  for(let i=0;i<20;i++) {
    await command('join',{auto:true});await command('ingest',{guest:i,grid:true});
    console.log('Joined and ingested guest '+(i+1)+'/20');
  }
  } else {
    if(/GuestReturnStop[^\n]*True/.test(ui('status')))ui('click','GuestReturnStop');
    ui('combo','GuestReturnSize','Up to 540p');ui('combo','GuestReturnFrameRate','OBS frame rate / 2');
    ui('capture','24-twenty-settings','VDONinjaGuestReturn');ui('click','GuestReturnStart');
  }
  await wait(15000);
  report.before=await command('probe',{name:'twenty-before',fast:true});
  report.statsBefore=await command('request',{type:'GetStats'});
  report.sampleStart=Date.now();await wait(30000);
  report.after=await command('probe',{name:'twenty-after',fast:true});report.sampleSeconds=(Date.now()-report.sampleStart)/1000;
  report.statsAfter=await command('request',{type:'GetStats'});report.main=await command('request',{type:'GetStreamStatus'});
  save();assert.equal(report.after.length,20);assert(report.main.outputActive);
  for(let i=0;i<20;i++) {
    const before=report.before[i].peers.find(p=>p.stream===state.stream),after=report.after[i].peers.find(p=>p.stream===state.stream);
    assert(before&&after,'Guest '+i+' missing return');assert.equal(after.width,960);assert.equal(after.height,540);
    const media=(p,k)=>p.rtp.find(x=>x.kind===k);assert(media(after,'video').frames>media(before,'video').frames+150,'Frozen guest '+i);
    assert(media(after,'audio').bytes>media(before,'audio').bytes,'Silent transport for guest '+i);
    report.phases.push({guest:i+1,frames:media(after,'video').frames-media(before,'video').frames,
      fps:(media(after,'video').frames-media(before,'video').frames)/report.sampleSeconds,
      otherGuestVideoFrames:report.after[i].peers.filter(p=>p.stream!==state.stream).reduce((sum,p)=>sum+p.rtp.filter(t=>t.kind==='video').reduce((n,t)=>n+(t.frames||0),0),0)});
  }
  await command('snap',{name:'25-twenty-room'});ui('capture','26-twenty-live','VDONinjaGuestReturn');
  let rejoinButtons=await command('reload',{guest:19});
  if(rejoinButtons.some(b=>b.text.includes('Join Room with Camera')))
    rejoinButtons=await command('click',{guest:19,selector:'text=Join Room with Camera',name:'27-twentieth-rejoin-devices'});
  if(rejoinButtons.some(b=>b.id==='gowebcam'))
    await command('click',{guest:19,selector:'#gowebcam',name:'28-twentieth-rejoined'});
  await wait(10000);report.rejoined=await command('probe',{name:'twenty-rejoined',fast:true});
  assert(report.rejoined[19].peers.some(p=>p.stream===state.stream&&p.rtp.some(t=>t.kind==='video'&&t.frames>10)));
  await command('snap',{name:'28-twentieth-rejoined',guest:19});
  ui('click','GuestReturnStop');await wait(1500);report.mainAfterReturnStop=await command('request',{type:'GetStreamStatus'});
  assert(report.mainAfterReturnStop.outputActive);ui('capture','29-twenty-return-stopped','VDONinjaGuestReturn');
  await command('request',{type:'StopStream'});report.passed=true;save();console.log('20-guest room passed media, reduced return, rejoin and independent public-output checks.');
}
main().catch(e=>{report.error=e.message;save();console.error(e.message);process.exitCode=1});
