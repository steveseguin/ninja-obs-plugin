// Remote device qualification of the real OBS Guest Return output.
// Uses the existing private BrowserStack fixture; never saves account/relay secrets.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const { firefox } = require('playwright');
const fixtures = process.env.VDO_BROWSERSTACK_FIXTURES || 'C:/Users/Steve/code/obsninja/tests/playwright/browserstack';
const env = require(path.join(fixtures, 'browserstack-env'));
const { SafariWebDriverFactory } = require(path.join(fixtures, 'safari-full-chunked'));
env.requireBrowserStackEnv();
const output = path.resolve(process.argv[2]);
const state = JSON.parse(fs.readFileSync(path.join(output, 'state.json')));
const profiles = (process.env.RETURN_PROFILES || 'firefox,ios,android').split(',');
const report = { date: new Date().toISOString(), forcedPublisherRelay: true, profiles: [] };
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
function ui(action, ...args) {
  try { return execFileSync('python', ['scripts/guest-return-room-ui.py', output, action, ...args], { encoding: 'utf8', windowsHide: true, stdio:['ignore','pipe','pipe'] }); }
  catch { throw new Error('OBS UI action failed: '+action+' (arguments omitted to protect credentials)'); }
}
function save() { fs.writeFileSync(path.join(output, 'remote-results.json'), JSON.stringify(report, null, 2)); }
async function snapshot() {
  const result = { userAgent: navigator.userAgent, stream: window.session && session.streamID, peers: [], videos: [] };
  if (!window.session) return result;
  for (const [id, pc] of Object.entries(session.rpcs || {})) {
    const stats = await pc.getStats(), rows = Array.from(stats.values());
    const transport = rows.find(x => x.type === 'transport' && x.selectedCandidatePairId);
    const pair = transport ? stats.get(transport.selectedCandidatePairId) : rows.find(x => x.type === 'candidate-pair' && x.state === 'succeeded' && (x.selected || x.nominated));
    const local = pair && stats.get(pair.localCandidateId), remote = pair && stats.get(pair.remoteCandidateId);
    result.peers.push({ id, stream: pc.streamID, state: pc.connectionState || pc.iceConnectionState,
      localCandidate: local && local.candidateType, remoteCandidate: remote && remote.candidateType,
      protocol: local && local.protocol, relayProtocol: local && local.relayProtocol,
      incoming: rows.filter(x => x.type === 'inbound-rtp').map(x => ({kind:x.kind || x.mediaType,
        bytes:x.bytesReceived,frames:x.framesDecoded,energy:x.totalAudioEnergy,samples:x.totalSamplesReceived})) });
  }
  result.videos = Array.from(document.querySelectorAll('video')).filter(v => v.srcObject).map(v => ({
    width:v.videoWidth,height:v.videoHeight,playing:!v.paused,time:v.currentTime,muted:v.muted,
    visible:!!(v.getBoundingClientRect().width && v.getBoundingClientRect().height)}));
  result.audioState = session.audioCtx && session.audioCtx.state;
  return result;
}
async function main() {
  if (!process.env.RETURN_SKIP_SETUP) {
    const urls = process.env.RETURN_RELAY_FILE ? JSON.parse(fs.readFileSync(process.env.RETURN_RELAY_FILE,'utf8')) : JSON.parse(process.env.CHUNKCAST_TURN_RTC_CONFIG_JSON || 'null');
    const servers = Array.isArray(urls) ? urls : urls && urls.iceServers;
    let entry = servers && servers.find(s => (Array.isArray(s.urls) ? s.urls : [s.urls]).some(u => /^turns?:/.test(u)));
    if (!entry) entry = {urls:process.env.CHUNKCAST_TURN_URLS,username:process.env.CHUNKCAST_TURN_USER,credential:process.env.CHUNKCAST_TURN_PASSWORD};
    let turnUrl = Array.isArray(entry.urls) ? entry.urls.find(u => /^turn:/.test(u)) : entry.urls;
    assert(turnUrl && entry.username && entry.credential, 'Configured external TURN credentials are required');
    const transport = process.env.RETURN_TURN_TCP === '1' ? 'tcp' : 'udp';
    turnUrl = turnUrl.replace(/\?transport=(udp|tcp)/, '?transport='+transport);
    if (!turnUrl.includes('?transport=')) turnUrl += '?transport='+transport;
    report.publisherRelayTransport = transport;
    if(!process.env.RETURN_ADVANCED_OPEN)ui('click', 'GuestReturnAdvanced');
    ui('set', JSON.stringify({GuestReturnStreamID:state.stream,GuestReturnRoomID:state.room,
      GuestReturnPassword:state.password,GuestReturnMaxViewers:10,
      GuestReturnICEServers:turnUrl+'|'+entry.username+'|'+entry.credential,GuestReturnForceTURN:true}));
    ui('click','GuestReturnCopyInvite'); ui('clipboard','invite.txt'); ui('click','GuestReturnStart');
    await wait(4000);
  }
  const invite = fs.readFileSync(path.join(output,'invite.txt'),'utf8').trim();
  for (const profile of profiles) {
    const row = {profile, phases:[]}; report.profiles.push(row); save();
    let factory, browser, page;
    try {
      if (profile === 'firefox') {
        // Use an isolated profile with the installed Cisco codec, never user cookies.
        const profilePath = path.join(output,'firefox-codec-profile');
        const profilesRoot = path.join(process.env.APPDATA,'Mozilla/Firefox/Profiles');
        let version;
        for(const name of fs.readdirSync(profilesRoot)) {
          const source=path.join(profilesRoot,name,'gmp-gmpopenh264');
          if(!fs.existsSync(source))continue;
          for(const candidate of fs.readdirSync(source)) {
            if(!fs.existsSync(path.join(source,candidate,'gmpopenh264.dll')))continue;
            const dest=path.join(profilePath,'gmp-gmpopenh264',candidate);fs.mkdirSync(dest,{recursive:true});
            for(const file of ['gmpopenh264.dll','gmpopenh264.info'])fs.copyFileSync(path.join(source,candidate,file),path.join(dest,file));
            version=candidate;break;
          }
          if(version)break;
        }
        assert(version,'Install Firefox OpenH264 before qualifying H.264');
        browser = await firefox.launchPersistentContext(profilePath,{headless:true,viewport:{width:1280,height:900},firefoxUserPrefs:{'media.navigator.streams.fake':true,'media.navigator.permission.disabled':true,'media.autoplay.default':0,'media.autoplay.block-webaudio':false,'media.gmp-gmpopenh264.enabled':true,'media.gmp-gmpopenh264.version':version}});
        page = await browser.newPage();
        page.evaluateAsync = page.evaluate.bind(page);
      } else {
        const options = {local:false,localIdentifier:undefined,sessionName:'OBS Guest Return room '+profile,buildName:'guest-return-20261031'};
        if(profile === 'android') Object.assign(options,{deviceName:'Google Pixel 9',osVersion:'16.0'});
        factory = new SafariWebDriverFactory('',{browserName:profile === 'android'?'chrome':'safari',options});
        page = (await factory.newPage()).page; row.session = page.sessionId;
      }
      const shot = async name => {
        fs.writeFileSync(path.join(output,'screenshots',profile+'-'+name+'-full.png'), await page.screenshot());
      };
      const click = async selector => profile === 'firefox' ? page.locator(selector).click({force:true}) : page.click(selector);
      await page.goto(invite+'&label='+profile);
      await wait(4000); await shot('01-invitation');
      const selector = await page.evaluate(() => {
        const el = document.getElementById('add_camera');
        if(!el) return null; el.setAttribute('data-review-join','true'); return '[data-review-join="true"]';
      });
      assert(selector,'Guest camera join action must be present');
      await click(selector); await wait(2500); await shot('02-permission');
      if(profile !== 'firefox') row.permissionsAccepted = await page.allowMediaPermission().catch(() => 0);
      await wait(2500); await shot('03-devices');
      await click('#gowebcam');
      const deadline = Date.now()+90000;
      while(Date.now()<deadline) {
        const current = await page.evaluateAsync(snapshot);
        const returning = current.peers.find(p=>p.stream===state.stream);
        if(returning && returning.incoming.some(x=>x.kind==='video'&&x.frames>10))break;
        await wait(3000);
      }
      row.phases.push({name:'joined',state:await page.evaluateAsync(snapshot)}); await shot('04-program'); save();
      await wait(6000);
      row.phases.push({name:'flowing',state:await page.evaluateAsync(snapshot)});
      const first = row.phases[0].state.peers.find(p=>p.stream===state.stream);
      const latest = row.phases[1].state.peers.find(p=>p.stream===state.stream);
      assert(first && latest,'Return publisher must be in the guest room');
      assert.equal(latest.remoteCandidate,'relay','Plugin return must arrive from the external relay');
      const media = (p,k) => p.incoming.find(x=>x.kind===k);
      assert(media(latest,'video').frames > media(first,'video').frames+10,'Video frames must advance');
      assert(media(latest,'audio').bytes > media(first,'audio').bytes,'Audio bytes must advance');
      assert(row.phases[1].state.videos.some(v=>v.width===1280&&v.height===720&&v.playing&&v.visible),'Program must visibly play');
      row.passed = true; console.log(profile+' passed: room program, audio and external relay');
    } catch(error) {
      row.passed=false; row.error=String(error.message); console.log(profile+' failed: '+row.error);
      if(page) { row.failedState=await page.evaluateAsync(snapshot).catch(()=>null); try{fs.writeFileSync(path.join(output,'screenshots',profile+'-failure-full.png'),await page.screenshot());}catch{} }
    } finally { if(factory)await factory.close(); if(browser)await browser.close(); save(); }
  }
  if(report.profiles.some(p=>!p.passed))process.exitCode=1;
}
main().catch(error=>{report.error=String(error.message);save();console.error(report.error);process.exitCode=1;});
