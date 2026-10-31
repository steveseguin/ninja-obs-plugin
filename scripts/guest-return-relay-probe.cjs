// Discover a current VDO.Ninja session relay and verify allocation before testing it.
// Credentials remain only in the ignored run directory; never print them.
const { chromium } = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const output = path.resolve(process.argv[2]);
(async()=>{
  const browser = await chromium.launch({channel:'chrome',args:['--use-fake-ui-for-media-stream','--use-fake-device-for-media-stream']});
  try {
    const context = await browser.newContext({permissions:['camera','microphone']});
    const pub = await context.newPage(), view = await context.newPage();
    const id='relayprobe'+Date.now();
    await pub.goto('https://vdo.ninja/?push='+id+'&password=false&webcam&autostart');
    await view.goto('https://vdo.ninja/?view='+id+'&password=false');
    await view.waitForFunction(()=>Object.values(session.rpcs||{}).some(pc=>pc.connectionState==='connected'),{},{timeout:60000});
    const config = await view.evaluate(()=>Object.values(session.rpcs).find(pc=>pc.connectionState==='connected').getConfiguration());
    const servers = config.iceServers.filter(s=>(Array.isArray(s.urls)?s.urls:[s.urls]).some(url=>/^turn:/.test(url)));
    if(!servers.length)throw new Error('No current session TURN server');
    const candidates = await view.evaluate(async servers=>{
      const pc=new RTCPeerConnection({iceServers:servers,iceTransportPolicy:'relay'}), candidates=[];
      pc.onicecandidate=e=>{if(e.candidate)candidates.push({type:e.candidate.type,protocol:e.candidate.protocol})};
      pc.createDataChannel('allocation');await pc.setLocalDescription(await pc.createOffer());
      await new Promise(r=>setTimeout(r,30000));pc.close();return candidates;
    },servers);
    fs.writeFileSync(path.join(output,'relay-allocation.json'),JSON.stringify({candidates,serverCount:servers.length},null,2));
    if(!candidates.some(c=>c.type==='relay'))throw new Error('Current session relay did not allocate');
    fs.writeFileSync(path.join(output,'private-relay.json'),JSON.stringify({iceServers:servers}));
    console.log('Current VDO.Ninja session relay allocated successfully; credentials saved only in ignored run directory.');
  }finally{await browser.close()}
})().catch(e=>{console.error(e.message);process.exitCode=1});
