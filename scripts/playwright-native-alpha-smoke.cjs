// Live, opt-in Linux/macOS interoperability check using synthetic frames only.
// Usage: node scripts/playwright-native-alpha-smoke.cjs /path/to/vp9-alpha-publisher [output-dir]
const { chromium } = require('playwright');
const { spawn } = require('node:child_process');
const { randomBytes } = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

async function withTimeout(promise, ms, description) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(description)), ms); }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

async function snapshot(page) {
  return page.evaluate(async () => {
    const result = [];
    for (const peer of window.__nativeAlphaPeers) {
      const stats = await peer.getStats();
      const tracks = [];
      stats.forEach(s => {
        if (s.type === 'inbound-rtp' && s.kind === 'video') {
          tracks.push({ mid: s.mid, framesDecoded: s.framesDecoded || 0,
            bytesReceived: s.bytesReceived || 0, codec: stats.get(s.codecId)?.mimeType });
        }
      });
      result.push({ state: peer.connectionState, tracks });
    }
    return result;
  });
}

function decodedTracks(peers) {
  for (const peer of peers) {
    const tracks = ['video', 'video-alpha'].map(mid => peer.tracks.find(t => t.mid === mid));
    if (peer.state === 'connected' && tracks.every(t => t && t.codec === 'video/VP9' && t.framesDecoded >= 20)) {
      return tracks;
    }
  }
  return null;
}

async function confirmPlayback(page, report, label) {
  let before;
  for (let attempt = 0; attempt < 30; attempt++) {
    await page.waitForTimeout(2000);
    const peers = await snapshot(page);
    report.samples.push({ label, peers });
    report.pageState = await page.evaluate(() => ({
      text: document.body.innerText.slice(0, 500),
    }));
    before = decodedTracks(peers);
    if (before) break;
  }
  if (!before) throw new Error(`${label}: both VP9 tracks did not decode`);
  await page.waitForTimeout(1500);
  const peers = await snapshot(page);
  report.samples.push({ label: `${label}-progress`, peers });
  const after = decodedTracks(peers);
  if (!after || !before.every(first => after.some(last => last.mid === first.mid && last.framesDecoded > first.framesDecoded + 10))) {
    throw new Error(`${label}: both VP9 tracks must continue decoding`);
  }
}

async function run() {
  if (!process.argv[2]) throw new Error('Supply the vp9-alpha-publisher executable path');
  if (process.platform === 'win32') throw new Error('This shutdown check requires POSIX SIGTERM; use the Windows alpha harness there');
  const output = path.resolve(process.argv[3] || 'artifacts/native-alpha-smoke');
  fs.mkdirSync(output, { recursive: true });
  const streamId = `NativeAlphaTest${randomBytes(8).toString('hex')}`;
  const password = process.env.VDO_TEST_PASSWORD || 'NativeAlphaSmoke42';
  const url = new URL(process.env.VDO_BASE_URL || 'https://vdo.ninja/alpha/');
  for (const [key, value] of Object.entries({ view: streamId, password, codec: 'vp9', autostart: '1', cleanoutput: '1' })) {
    url.searchParams.set(key, value);
  }
  const publisher = spawn(path.resolve(process.argv[2]), ['--stream-id', streamId, '--password', password]);
  const exited = new Promise(resolve => {
    publisher.once('exit', (code, signal) => resolve({ code, signal }));
    publisher.once('error', error => resolve({ error: String(error) }));
  });
  let publisherLog = '';
  publisher.stdout.on('data', data => { publisherLog += data; });
  publisher.stderr.on('data', data => { publisherLog += data; });
  const report = { baseUrl: process.env.VDO_BASE_URL || 'https://vdo.ninja/alpha/', streamId, samples: [], pageErrors: [] };
  let browser;
  try {
    browser = await chromium.launch({ headless: true, args: ['--autoplay-policy=no-user-gesture-required'] });
    const page = await browser.newPage();
    page.on('pageerror', error => report.pageErrors.push(String(error)));
    await page.addInitScript(() => {
      window.__nativeAlphaPeers = [];
      window.RTCPeerConnection = new Proxy(window.RTCPeerConnection, {
        construct(target, args) {
          const peer = new target(...args);
          window.__nativeAlphaPeers.push(peer);
          return peer;
        },
      });
    });
    // Start the viewer only after the publisher has sent its registration.
    for (let attempt = 0; !publisherLog.includes('Frame loop running'); attempt++) {
      if (attempt >= 100 || publisher.exitCode !== null || publisher.signalCode !== null) {
        throw new Error('Publisher failed to start');
      }
      await page.waitForTimeout(100);
    }
    await page.goto(url.href, { waitUntil: 'domcontentloaded', timeout: 60000 });
    await confirmPlayback(page, report, 'initial');
    await page.reload({ waitUntil: 'domcontentloaded', timeout: 60000 });
    await confirmPlayback(page, report, 'reload');
    // Stop with an active viewer: clearing peers under their mutex used to deadlock.
    publisher.kill('SIGTERM');
    report.publisherExit = await withTimeout(exited, 10000, 'Publisher hung during active-viewer shutdown');
    if (report.publisherExit.code !== 0 || !publisherLog.includes('Exiting')) {
      throw new Error(`Publisher did not shut down cleanly: ${JSON.stringify(report.publisherExit)}`);
    }
    report.passed = true;
    console.log('PASS: dual VP9 decode, continuous playback, viewer reload, and clean publisher shutdown');
  } catch (error) {
    report.failure = String(error);
    throw error;
  } finally {
    if (publisher.exitCode === null && publisher.signalCode === null) publisher.kill('SIGKILL');
    await withTimeout(exited, 5000, 'Publisher cleanup failed');
    if (browser) await browser.close();
    fs.writeFileSync(path.join(output, 'publisher.log'), publisherLog);
    fs.writeFileSync(path.join(output, 'browser.json'), JSON.stringify(report, null, 2));
  }
}

run().catch(error => { console.error(error); process.exitCode = 1; });
