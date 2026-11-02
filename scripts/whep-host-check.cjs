// Real OBS publishing to independent WHEP players. No VDO.Ninja player or client API.
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { chromium, firefox } = require('playwright');
const { ObsWebSocketClient } = require('./obs-websocket-vdoninja-source-check.cjs');
const [url, output, cyclesArg, brokerArg, thirdParty, remote] = process.argv.slice(2);
const cycles = Math.max(1, Number(cyclesArg || 2));
const broker = (brokerArg || 'https://whep.vdo.ninja').replace(/\/$/, '');
const playerIceServers = process.env.WHEP_PLAYER_ICE_SERVERS_JSON
  ? JSON.parse(process.env.WHEP_PLAYER_ICE_SERVERS_JSON) : null;
assert(playerIceServers === null || Array.isArray(playerIceServers), 'WHEP_PLAYER_ICE_SERVERS_JSON must be an array');
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(fn, label, timeout = 40000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { if (await fn()) return; await sleep(250); }
  throw Error('Timed out: ' + label);
}
(async () => {
  const client = new ObsWebSocketClient(url);
  const token = crypto.randomUUID();
  const report = { streamId: 'whepobs' + crypto.randomBytes(6).toString('hex'),
    endpoint: broker + '/' + token, cycles: [], players: 'Independent HTTP/RTCPeerConnection fixture',
    playerIceConfiguration: playerIceServers === null ? 'default STUN' :
      playerIceServers.length === 0 ? 'host candidates only' : 'custom ICE servers' };
  const browsers = [], pages = [];
  let server;
  try {
    const html = fs.readFileSync(path.join(__dirname, '../tests/tools/whep-player.html'));
    server = http.createServer((req, res) => {
      if (!req.url.startsWith('/whep/')) {
        res.writeHead(200, { 'Content-Type': 'text/html' }); res.end(html); return;
      }
      const target = new URL(broker);
      const upstream = http.request({ hostname: target.hostname, port: target.port,
        path: req.url.slice(5), method: req.method,
        headers: { ...req.headers, host: 'whep.vdo.ninja' } }, up => {
        const headers = { ...up.headers };
        if (headers.location) headers.location = '/whep' + new URL(headers.location, broker).pathname;
        res.writeHead(up.statusCode, headers); up.pipe(res);
      });
      upstream.on('error', () => { res.statusCode = 502; res.end(); }); req.pipe(upstream);
    });
    await new Promise(r => server.listen(0, '127.0.0.1', r));
    const fixtureUrl = 'http://127.0.0.1:' + server.address().port;
    const endpoint = broker.startsWith('http://127.0.0.1:') ? fixtureUrl + '/whep/' + token : report.endpoint;
    await client.connect();
    report.obs = await client.request('GetVersion');
    for (const input of (await client.request('GetInputList')).inputs)
      await client.request('RemoveInput', { inputName: input.inputName });
    await sleep(500); // OBS releases removed sources asynchronously.
    const colorName = 'WHEP color ' + token, audioName = 'WHEP audio ' + token;
    const scene = (await client.request('GetCurrentProgramScene')).currentProgramSceneName;
    await client.request('CreateInput', { sceneName: scene, inputName: colorName,
      inputKind: 'color_source_v3', inputSettings: { color: 0xff2a7fff, width: 1280, height: 720 }, sceneItemEnabled: true });
    await client.request('CreateInput', { sceneName: scene, inputName: audioName,
      inputKind: 'ffmpeg_source', inputSettings: { is_local_file: true,
        local_file: path.resolve(output, 'tone.wav'), looping: true, restart_on_activate: true }, sceneItemEnabled: true });
    await client.request('SetInputMute', { inputName: audioName, inputMuted: false });
    await client.request('SetStreamServiceSettings', { streamServiceType: 'vdoninja_service',
      streamServiceSettings: { stream_id: report.streamId, password: crypto.randomBytes(8).toString('hex'),
        whep_token: token, max_viewers: 3, enable_data_channel: true, auto_reconnect: true } });
    assert.equal((await client.request('GetStreamServiceSettings')).streamServiceSettings.whep_token, token);
    browsers.push(await chromium.launch({ headless: true, args: ['--autoplay-policy=no-user-gesture-required'] }));
    const profile = path.join(output, 'firefox-profile');
    let gmpVersion = '';
    if (process.platform === 'win32') {
      const profiles = path.join(process.env.APPDATA, 'Mozilla/Firefox/Profiles');
      for (const entry of fs.readdirSync(profiles)) {
        const gmp = path.join(profiles, entry, 'gmp-gmpopenh264');
        if (!fs.existsSync(gmp)) continue;
        for (const version of fs.readdirSync(gmp)) {
          if (!fs.existsSync(path.join(gmp, version, 'gmpopenh264.dll'))) continue;
          const target = path.join(profile, 'gmp-gmpopenh264', version);
          fs.mkdirSync(target, { recursive: true });
          for (const file of ['gmpopenh264.dll', 'gmpopenh264.info'])
            fs.copyFileSync(path.join(gmp, version, file), path.join(target, file));
          gmpVersion = version;
        }
        if (gmpVersion) break;
      }
      assert(gmpVersion, 'Firefox H.264 validation needs an installed OpenH264 module');
    }
    browsers.push(await firefox.launchPersistentContext(profile, { headless: true,
      viewport: { width: 1280, height: 800 }, firefoxUserPrefs: {
        'media.autoplay.default': 0, 'media.autoplay.block-webaudio': false,
        'media.gmp-gmpopenh264.enabled': true, 'media.gmp-gmpopenh264.version': gmpVersion,
        'media.peerconnection.ice.obfuscate_host_addresses': false } }));
    report.browsers = browsers.map(b => typeof b.version === 'function' ? b.version() : b.browser().version());
    const active = async () => (await client.request('GetStreamStatus')).outputActive;
    async function start() {
      const begin = Date.now(); await client.request('StartStream');
      await until(active, 'output starts'); await sleep(2500); return Date.now() - begin;
    }
    async function stop() {
      const begin = Date.now(); await client.request('StopStream');
      await until(async () => !await active(), 'output stops'); return Date.now() - begin;
    }
    async function newPlayer(browserIndex, trickle = false, codec = 'H264') {
      const page = await browsers[browserIndex].newPage({ viewport: { width: 1280, height: 800 } }); pages.push(page);
      if (playerIceServers !== null)
        await page.addInitScript(servers => { window.playerIceServers = servers; }, playerIceServers);
      await page.goto(fixtureUrl); await page.locator('header').click();
      await page.evaluate(args => startPlayback(...args), [endpoint, trickle, codec]);
      await until(async () => (await page.evaluate(() => playbackStats())).frames >= 20, 'decoded WHEP frames');
      return page;
    }
    async function verify(page, label) {
      const before = await page.evaluate(() => playbackStats()); await sleep(2000);
      const after = await page.evaluate(() => playbackStats());
      assert(after.frames > before.frames + 20, label + ': video advances');
      assert(after.audioPackets > before.audioPackets + 20, label + ': audio advances');
      assert(after.rms > 0.01 && after.peakHz > 410 && after.peakHz < 470, label + ': decoded 440Hz tone ' + JSON.stringify(after));
      assert(after.pixel[0] > 220 && after.pixel[1] > 100 && after.pixel[1] < 150 && after.pixel[2] < 70,
        label + ': expected orange video pixels ' + after.pixel);
      assert.equal(after.videoCodec.toLowerCase(), 'video/h264');
      assert.equal(after.audioCodec.toLowerCase(), 'audio/opus');
      await page.screenshot({ path: path.join(output, label + '.png') });
      return { ...after, framesDelta: after.frames - before.frames,
        requests: await page.evaluate(() => requests), screenshot: label + '.png' };
    }
    for (let cycle = 1; cycle <= cycles; ++cycle) {
      const entry = { cycle, startMs: await start() }; report.cycles.push(entry);
      // Exercise trickle first, then simultaneous joins with complete offers.
      // The existing server stores one ETag per stream, so interleaved PATCHes
      // after another player's POST can fail its HTTP precondition check.
      const [chrome, fox] = cycle === 1
        ? [await newPlayer(0, true), await newPlayer(1, true)]
        : await Promise.all([newPlayer(0), newPlayer(1)]);
      entry.chrome = await verify(chrome, `cycle-${cycle}-chrome`);
      entry.firefox = await verify(fox, `cycle-${cycle}-firefox`);
      if (cycle === 1) {
        const third = await newPlayer(0);
        entry.third = await verify(third, 'third-viewer');
        await assert.rejects(() => newPlayer(0), /rejected|POST returned/, 'fourth viewer exceeds limit');
        entry.viewerLimitEnforced = true;
        await chrome.evaluate(() => stopPlayback()); await chrome.close(); await sleep(1500);
        entry.survivor = await verify(fox, 'surviving-viewer');
        const replacement = await newPlayer(0);
        entry.replacement = await verify(replacement, 'replacement-viewer');
        await third.evaluate(() => stopPlayback()); await third.close();
        await replacement.evaluate(() => stopPlayback()); await replacement.close();
      }
      entry.stopMs = await stop(); assert(entry.stopMs < 20000, 'OBS stop stalls');
      await until(async () => (await fox.evaluate(() => playbackStats())).state !== 'connected', 'player drops after output stop', 20000);
      entry.playerDroppedAfterStop = true;
      for (const page of pages.splice(0)) if (!page.isClosed()) await page.close();
      if (cycle === 1) {
        const begin = Date.now(); await client.request('StartStream'); await sleep(300);
        await client.request('StopStream'); await until(async () => !await active(), 'immediate stop');
        report.immediateStopMs = Date.now() - begin; assert(report.immediateStopMs < 20000); await sleep(1000);
      }
    }
    if (thirdParty === 'eyevinn') {
      await start();
      report.thirdParty = await require('./whep-eyevinn-check.cjs').checkEyevinn(browsers, endpoint, output);
      await stop();
    }
    if (remote === 'browserstack') {
      await start();
      report.browserstack = await require('./whep-browserstack-check.cjs').checkBrowserStack(endpoint, output);
      await stop();
    }
    report.ok = true;
  } catch (error) { report.error = String(error.stack || error); throw error; }
  finally {
    try { if ((await client.request('GetStreamStatus')).outputActive) await client.request('StopStream'); } catch (_) {}
    for (const browser of browsers) await browser.close();
    if (server) await new Promise(r => server.close(r));
    fs.writeFileSync(path.join(output, 'whep-workflow.json'), JSON.stringify(report, null, 2));
    await client.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
