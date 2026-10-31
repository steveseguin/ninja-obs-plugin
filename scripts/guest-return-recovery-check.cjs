// Run after guest-return-20-check.cjs, leaving its controller and guests open.
// Verify return restart without rejoining, then the lower-upload preset.
const fs=require('node:fs'), path=require('node:path'), assert=require('node:assert/strict');
const {execFileSync}=require('node:child_process');
const output=path.resolve(process.argv[2]);
const state=JSON.parse(fs.readFileSync(path.join(output,'state.json')));
const report={syntheticGuests:20,phases:[]};
const wait=ms=>new Promise(r=>setTimeout(r,ms));
function ui(action,...args){return execFileSync('python',['scripts/guest-return-room-ui.py',output,action,...args],{encoding:'utf8',windowsHide:true})}
async function command(action,args={}){
  const response=await fetch('http://127.0.0.1:19356',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action,args}),signal:AbortSignal.timeout(300000)});
  const data=await response.json();assert(response.ok,JSON.stringify(data));return data;
}
async function check(name,width,height){
  await wait(15000);
  const before=await command('probe',{name:name+'-before',fast:true});
  await wait(15000);
  const after=await command('probe',{name:name+'-after',fast:true});
  assert.equal(after.length,20);
  const guests=after.map((g,i)=>{
    const a=g.peers.find(p=>p.stream===state.stream), b=before[i].peers.find(p=>p.stream===state.stream);
    assert(a&&b,'Missing return '+g.name);assert.equal(a.state,'connected');
    assert.equal(a.width,width);assert.equal(a.height,height);
    const media=(p,k)=>p.rtp.find(t=>t.kind===k);
    const frames=media(a,'video').frames-media(b,'video').frames;
    const audioBytes=media(a,'audio').bytes-media(b,'audio').bytes;
    assert(frames>100,'Frozen '+g.name);assert(audioBytes>0,'Audio stopped '+g.name);
    return {name:g.name,frames,audioBytes,width,height};
  });
  report.phases.push({name,guests});
  fs.writeFileSync(path.join(output,'final-checks.json'),JSON.stringify(report,null,2));
  console.log(name+': all 20 guests received moving video and advancing audio.');
}
(async()=>{
  ui('click','GuestReturnStart');await check('restart-with-guests-still-joined',960,540);
  await command('snap',{name:'33-return-restarted',guest:0});
  ui('click','GuestReturnStop');ui('combo','GuestReturnSize','Up to 360p');
  ui('set',JSON.stringify({GuestReturnBitrate:750}));
  ui('capture','30-lower-upload-settings','VDONinjaGuestReturn');
  ui('click','GuestReturnStart');await check('lower-upload-750kbps-360p',640,360);
  await command('snap',{name:'31-lower-upload-room'});
  ui('click','GuestReturnStop');ui('combo','GuestReturnSize','Up to 540p');
  ui('set',JSON.stringify({GuestReturnBitrate:1500}));
  ui('capture','32-packaged-settings-final','VDONinjaGuestReturn');
  report.passed=true;
  fs.writeFileSync(path.join(output,'final-checks.json'),JSON.stringify(report,null,2));
})().catch(e=>{report.error=e.stack;fs.writeFileSync(path.join(output,'final-checks.json'),JSON.stringify(report,null,2));console.error(e);process.exitCode=1});
