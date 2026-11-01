// Basic HTTP/WebRTC WHEP receiver on remote BrowserStack browsers; no Local tunnel.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const { chromium, firefox } = require('playwright');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function checkBrowserStack(endpoint, output) {
  const fixtures = process.env.VDO_BROWSERSTACK_FIXTURES ||
    'C:/Users/Steve/code/obsninja/tests/playwright/browserstack';
  const env = require(path.join(fixtures, 'browserstack-env'));
  const { SafariWebDriverFactory } = require(path.join(fixtures, 'safari-full-chunked'));
  env.requireBrowserStackEnv(); // Existing loader reads chunkcast/.secrets without copying it.
  const sanitize = value => {
    let text = String(value);
    for (const secret of [process.env.BROWSERSTACK_USERNAME, process.env.BROWSERSTACK_ACCESS_KEY]) {
      if (secret) for (const encoded of [secret, encodeURIComponent(secret)]) text = text.split(encoded).join('[redacted]');
    }
    return text.replace(/wss:\/\/cdp\.browserstack\.com\/playwright\?caps=\S+/g, '[BrowserStack connection]');
  };
  const html = fs.readFileSync(path.join(__dirname, '../tests/tools/whep-player.html'), 'utf8');
  const report = { date: new Date().toISOString(), browserStackLocal: false,
    playerStun: ['stun:stun.l.google.com:19302', 'stun:stun.cloudflare.com:3478'],
    player: 'tests/tools/whep-player.html', fixtureSha256: crypto.createHash('sha256').update(html).digest('hex'),
    loading: 'Fixture installed through automation into an isolated HTTPS document; WHEP requests go directly to the public endpoint.',
    profiles: [] };
  fs.writeFileSync(path.join(output, 'browserstack-player.html'), html);
  const save = () => fs.writeFileSync(path.join(output, 'browserstack-results.json'), JSON.stringify(report, null, 2));
  const profiles = (process.env.WHEP_BROWSERSTACK_PROFILES || 'win-chrome,android,ios').split(',');
  for (const profile of profiles) {
    const row = { profile, samples: [] }; report.profiles.push(row); save();
    let factory, browser, page;
    try {
      console.log('BrowserStack starting ' + profile);
      if (['android', 'ios', 'win-firefox-native', 'mac-safari'].includes(profile)) {
        const options = { local: false, localIdentifier: undefined, networkLogs: false,
          projectName: 'OBS WHEP', buildName: 'whep-v1.1.84-remote', sessionName: 'Basic WHEP ' + profile };
        if (profile === 'android') Object.assign(options, { deviceName: 'Google Pixel 9', osVersion: '16.0' });
        let browserName = profile === 'android' ? 'chrome' : 'safari';
        const capabilities = {};
        if (profile === 'win-firefox-native' || profile === 'mac-safari') {
          Object.assign(options, { realMobile: false, deviceName: undefined,
            os: profile === 'mac-safari' ? 'OS X' : 'Windows',
            osVersion: profile === 'mac-safari' ? 'Sequoia' : '11', browserVersion: 'latest' });
          capabilities['appium:nativeWebTap'] = undefined;
          if (profile === 'win-firefox-native') {
            browserName = 'firefox';
            capabilities['moz:firefoxOptions'] = { prefs: { 'media.gmp-gmpopenh264.enabled': true,
              'media.peerconnection.video.h264_enabled': true } };
          }
        }
        factory = new SafariWebDriverFactory('', { browserName, options, capabilities });
        page = (await factory.newPage()).page;
        row.automationRun = page.sessionId;
      } else {
        const caps = env.buildCaps(profile, 'Basic WHEP ' + profile, 'whep-v1.1.84-remote', {
          project: 'OBS WHEP', 'browserstack.networkLogs': 'false', 'browserstack.local': 'false' });
        browser = await (profile === 'win-firefox' ? firefox : chromium).connect(env.browserStackWs(caps));
        page = await browser.newPage();
        page.evaluateAsync = page.evaluate.bind(page);
        page.click = selector => page.locator(selector).click();
        const details = await page.evaluate(() => {}, 'browserstack_executor: {"action":"getSessionDetails"}');
        row.automationRun = typeof details === 'string' ? JSON.parse(details).hashed_id : details?.hashed_id;
      }
      save();
      const shot = async label => {
        const name = 'browserstack-' + profile + '-' + label + '.png';
        fs.writeFileSync(path.join(output, name), await page.screenshot());
        return name;
      };
      // Our own public origin supplies a secure document, not the receiver implementation.
      await page.goto('https://steveseguin.github.io/ninja-obs-plugin/whep.html');
      await page.evaluate(source => { document.open(); document.write(source); document.close(); }, html);
      row.environment = await page.evaluate(() => ({ userAgent: navigator.userAgent, secure: isSecureContext }));
      assert(row.environment.secure, 'A secure WebRTC document is required');
      await page.evaluate(value => document.querySelector('#endpoint').value = value, endpoint);
      row.readyScreenshot = await shot('ready');
      const read = () => page.evaluateAsync(async () => {
        if (window.playbackError) throw Error(window.playbackError);
        return playbackStats();
      });
      const play = async label => {
        await page.click('#play');
        const deadline = Date.now() + 60000;
        let first;
        while (Date.now() < deadline) {
          first = await read();
          if (first.state === 'connected' && first.frames > 20) break;
          await sleep(1000);
        }
        assert.equal(first.state, 'connected', label + ': connected');
        assert(first.frames > 20, label + ': decoded video');
        await page.click('#audio'); await sleep(1200);
        const before = await read(); await sleep(10000); const after = await read();
        row.samples.push({ label, before, after }); save();
        assert(after.frames > before.frames + 30 && after.currentTime > before.currentTime + 5, 'Video advances');
        assert(after.audioPackets > before.audioPackets + 100, 'Audio packets advance');
        assert(after.rms > .01 && after.peakHz > 410 && after.peakHz < 470, 'Decoded 440 Hz audio');
        assert(!after.muted && !after.paused && after.audioState === 'running', 'Playback enabled by user gesture');
        assert.equal(after.width, 1280); assert.equal(after.height, 720);
        assert(after.pixel[0] > 220 && after.pixel[1] > 100 && after.pixel[1] < 150 && after.pixel[2] < 70,
          'Expected decoded orange video');
        assert.equal(after.videoCodec.toLowerCase(), 'video/h264');
        assert.equal(after.audioCodec.toLowerCase(), 'audio/opus');
        assert(after.route.local && after.route.remote, 'Selected ICE route must be recorded');
        row.samples.at(-1).screenshot = await shot(label);
      };
      await play('playing');
      await page.click('#stop'); await sleep(1500);
      assert.equal((await read()).state, 'closed');
      row.stoppedScreenshot = await shot('stopped');
      await play('rejoined');
      await page.click('#stop'); await sleep(1000);
      assert.equal((await read()).state, 'closed');
      row.requests = await page.evaluate(() => window.requests);
      row.ok = true;
      console.log('BrowserStack passed ' + profile);
    } catch (error) {
      row.ok = false; row.error = sanitize(error.message || error);
      console.log('BrowserStack failed ' + profile + ': ' + row.error);
      if (page) {
        row.requests = await page.evaluate(() => window.requests).catch(() => null);
        row.failureState = await page.evaluateAsync(() => playbackStats()).catch(() => null);
        try { fs.writeFileSync(path.join(output, 'browserstack-' + profile + '-failure.png'), await page.screenshot()); } catch (_) {}
      }
    } finally {
      if (page) await page.evaluateAsync(() => stopPlayback()).catch(() => {});
      if (factory) await factory.close();
      if (browser) await browser.close();
      save();
    }
  }
  report.ok = report.profiles.every(row => row.ok); save();
  assert(report.ok, 'One or more BrowserStack profiles failed; inspect browserstack-results.json');
  return report;
}
module.exports = { checkBrowserStack };
