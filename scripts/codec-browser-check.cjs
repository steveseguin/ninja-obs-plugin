// Real VDO.Ninja viewers. Observe the installed Chrome and Playwright Firefox;
// retain raw SDP/stats, decoded pixels, and OBS resource/output counters.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium, firefox } = require('playwright');
const wait = ms => new Promise(r => setTimeout(r, ms));

async function checkCodecBrowsers(stream, options, directory, sender) {
  const results = [];
  const selectedBrowsers = (process.env.CODEC_BROWSERS || 'chrome,firefox').split(',');
  assert(selectedBrowsers.length && selectedBrowsers.every(n => ['chrome','firefox'].includes(n)));
  for (const name of selectedBrowsers) {
    let browser, context;
    if (name === 'chrome') {
      browser = await chromium.launch({
      executablePath: process.env.CODEC_CHROME_EXECUTABLE || path.join(process.env.LOCALAPPDATA, 'Google/Chrome/Application/chrome.exe'), headless: true,
      args: ['--autoplay-policy=no-user-gesture-required', '--mute-audio'],
      });
      context = await browser.newContext();
    } else {
      // Fresh test profile; copy only the installed Cisco codec binary/info,
      // never the user's browser profile, cookies or preferences.
      const profile = path.join(directory, 'firefox-profile'); fs.mkdirSync(profile, {recursive:true});
      const profiles = path.join(process.env.APPDATA, 'Mozilla/Firefox/Profiles');
      let gmpVersion;
      for (const entry of fs.readdirSync(profiles)) {
        const gmp = path.join(profiles, entry, 'gmp-gmpopenh264');
        if (!fs.existsSync(gmp)) continue;
        for (const version of fs.readdirSync(gmp)) {
          if (!fs.existsSync(path.join(gmp,version,'gmpopenh264.dll'))) continue;
          const target = path.join(profile, 'gmp-gmpopenh264', version); fs.mkdirSync(target,{recursive:true});
          for (const file of ['gmpopenh264.dll','gmpopenh264.info']) fs.copyFileSync(path.join(gmp,version,file),path.join(target,file));
          gmpVersion = version;
        }
        if (gmpVersion) break;
      }
      const cache = path.join(process.env.LOCALAPPDATA, 'ms-playwright');
      const newest = fs.readdirSync(cache).filter(n=>/^firefox-\d+$/.test(n)).sort((a,b)=>Number(b.split('-')[1])-Number(a.split('-')[1]))[0];
      const firefoxExecutable = process.env.CODEC_FIREFOX_EXECUTABLE || path.join(cache, newest, 'firefox/firefox.exe');
      context = await firefox.launchPersistentContext(profile, {headless: true, firefoxUserPrefs: {
        'media.autoplay.default': 0, 'media.autoplay.block-webaudio': false,
        'media.gmp-gmpopenh264.enabled': true, 'media.gmp-gmpopenh264.version': gmpVersion || '',
        // This disposable loss fixture deliberately uses a loopback TURN proxy.
        // Firefox blocks such servers by default (Mozilla bug 1973521).
        'media.peerconnection.ice.loopback': Boolean(options.ice),
      }, executablePath: firefoxExecutable});
      browser = context.browser();
    }
    const result = {name, version: browser.version(), phases: []}; results.push(result);
    try {
      await context.addInitScript(({ice}) => {
        window.codecPeers = [];
        const original = RTCPeerConnection;
        window.RTCPeerConnection = new Proxy(original, {construct(target, args) {
          if (ice) args[0] = {...args[0], iceServers: [ice], iceTransportPolicy: 'relay'};
          const pc = new target(...args); window.codecPeers.push(pc);
          if (ice) {
            const set = pc.setConfiguration.bind(pc);
            pc.setConfiguration = config => set({...config, iceServers: [ice], iceTransportPolicy: 'relay'});
          }
          return pc;
        }});
      }, {ice: options.ice || null});
      const page = await context.newPage();
      result.errors = []; page.on('pageerror', e => result.errors.push(String(e)));
      await page.goto(`https://vdo.ninja/?view=${stream}&password=false&autostart&cleanoutput`, {waitUntil: 'domcontentloaded'});
      result.capabilities = await page.evaluate(() => RTCRtpReceiver.getCapabilities('video'));
      const mime = 'video/' + (options.codec === 'HEVC' ? 'h265' : options.codec.toLowerCase());
      const compatible = result.capabilities.codecs.some(c => c.mimeType.toLowerCase() === mime &&
        !(options.expectProfile4 && !/profile-id=4(?:;|$)/.test(c.sdpFmtpLine || '')));
      if (options.codec === 'H264') assert(compatible, `${name} H.264 codec unavailable in test installation`);
      async function snapshot() {
        return page.evaluate(async () => {
          const peers = [];
          for (const pc of window.codecPeers) {
            if (pc.connectionState === 'closed') {
              peers.push({state: 'closed', localSdp: pc.localDescription?.sdp,
                remoteSdp: pc.remoteDescription?.sdp, reports: []}); continue;
            }
            const reports = await pc.getStats();
            peers.push({state: pc.connectionState, localSdp: pc.localDescription?.sdp, remoteSdp: pc.remoteDescription?.sdp,
              reports: [...reports.values()]});
          }
          const v = [...document.querySelectorAll('video')].find(v => v.videoWidth);
          let pixels;
          if (v) {
            const c = document.createElement('canvas'); c.width = 640; c.height = 360;
            const ctx = c.getContext('2d', {willReadFrequently: true}); ctx.drawImage(v, 0, 0, 640, 360);
            pixels = Array.from({length: 8}, (_, i) => [...ctx.getImageData(i*80+40, 50, 1, 1).data].slice(0,3));
          }
          return {time: Date.now(), peers, pixels};
        });
      }
      const findVideo = s => s.peers.flatMap(p => p.reports).find(r => r.type === 'inbound-rtp' && r.kind === 'video' && r.framesDecoded > 10);
      const deadline = Date.now() + (compatible ? 45000 : 10000);
      let first;
      do { await wait(500); first = await snapshot(); } while (!findVideo(first) && Date.now() < deadline);
      result.initialObservation = first;
      if (!compatible) {
        result.unsupported = true; result.observation = first;
        assert(!findVideo(first), 'Viewer unexpectedly decoded a capability it did not advertise');
        continue;
      }
      assert(findVideo(first), `${name} did not decode ${mime}`);
      for (const phase of ['steady', 'reloaded']) {
        if (phase === 'reloaded') {
          await page.reload({waitUntil: 'domcontentloaded'});
          const end = Date.now()+30000;
          do { await wait(500); first = await snapshot(); } while (!findVideo(first) && Date.now() < end);
          assert(findVideo(first), 'Viewer did not recover after reload');
        }
        await wait(2000); const before = await snapshot();
        const obsBefore = {stats: await sender.request('GetStats'), output: await sender.request('GetStreamStatus')};
        await wait(10000); const after = await snapshot();
        const obsAfter = {stats: await sender.request('GetStats'), output: await sender.request('GetStreamStatus')};
        const a = findVideo(before), b = findVideo(after); assert(a && b);
        const seconds = (b.timestamp-a.timestamp)/1000;
        const all = after.peers.flatMap(p => p.reports);
        const codec = all.find(r => r.id === b.codecId);
        const metrics = {fps: (b.framesDecoded-a.framesDecoded)/seconds,
          keyframeFraction: (b.keyFramesDecoded-a.keyFramesDecoded)/(b.framesDecoded-a.framesDecoded),
          kbps: (b.bytesReceived-a.bytesReceived)*8/seconds/1000,
          nack: (b.nackCount||0)-(a.nackCount||0), pli: (b.pliCount||0)-(a.pliCount||0),
          lost: (b.packetsLost||0)-(a.packetsLost||0), freezes: (b.freezeCount||0)-(a.freezeCount||0),
          decodeMsPerFrame: (b.totalDecodeTime-a.totalDecodeTime)*1000/(b.framesDecoded-a.framesDecoded)};
        result.phases.push({phase, metrics, before, after, obsBefore, obsAfter});
        assert.equal(codec?.mimeType.toLowerCase(), mime);
        if (options.ice) {
          const selected = all.find(r => r.type === 'candidate-pair' && r.nominated && r.state === 'succeeded');
          assert(selected && all.some(r => r.id === selected.localCandidateId && r.candidateType === 'relay'), 'Loss test bypassed TURN');
        }
        assert(metrics.fps >= (options.fps || 30)*(options.ice ? 2/3 : .9), JSON.stringify(metrics));
        assert(metrics.keyframeFraction < .15, 'Browser received an all-intra stream');
        const reference = [[24,180,40],[190,45,45],[45,70,190],[180,160,30],[40,170,180],[165,45,160],[32,32,32],[210,210,210]];
        assert(after.pixels, 'No decoded pixels');
        metrics.maxPatchError = Math.max(...after.pixels.flatMap((rgb,i)=>rgb.map((v,j)=>Math.abs(v-reference[i][j]))));
        assert(metrics.maxPatchError <= 10, `Browser colors shifted: ${metrics.maxPatchError}`);
        const audio = all.find(r => r.type === 'inbound-rtp' && r.kind === 'audio');
        assert(audio && audio.bytesReceived > 1000, 'Browser received no audio');
        const oldAudio = before.peers.flatMap(p => p.reports).find(r => r.id === audio.id);
        metrics.audioRms = Math.sqrt((audio.totalAudioEnergy-oldAudio.totalAudioEnergy)/
                                    (audio.totalSamplesDuration-oldAudio.totalSamplesDuration));
        assert(metrics.audioRms > .005, 'Browser decoded silent audio');
        metrics.audioCodec = all.find(r => r.id === audio.codecId)?.mimeType;
        assert(!after.peers.some(p => /a=rtpmap:\d+ ulpfec\//i.test(p.remoteSdp || '')), 'Unexpected ULPFEC advertisement');
      }
      await page.screenshot({path: path.join(directory, name+'.png')});
      result.ok = true;
    } catch (e) { result.error = String(e.stack || e); }
    finally { await browser.close(); fs.writeFileSync(path.join(directory, 'browsers.json'), JSON.stringify(results,null,2)); }
  }
  return results;
}
module.exports = {checkCodecBrowsers};
