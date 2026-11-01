// Exercise the actual third-party hosted application. Instrumentation observes
// peer stats; it does not replace fetch, SDP, ICE, or the player's WHEP adapter.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const playerUrl = 'https://webrtc.player.eyevinn.technology/';

async function stats(page) {
  return page.evaluate(async () => {
    const video = document.querySelector('video');
    const pc = [...window.__observedPeers].reverse().find(p => p.connectionState === 'connected');
    const result = { state: pc?.connectionState || 'not connected',
      frames: video?.getVideoPlaybackQuality().totalVideoFrames || 0,
      width: video?.videoWidth, height: video?.videoHeight, time: video?.currentTime,
      paused: video?.paused, visibility: document.visibilityState };
    if (!pc || !video?.videoWidth) return result;
    const values = await pc.getStats();
    values.forEach(v => {
      if (v.type === 'inbound-rtp' && v.kind === 'audio') result.audioPackets = v.packetsReceived;
      if (v.type === 'inbound-rtp' && v.kind === 'video') result.decodedFrames = v.framesDecoded;
      if (v.type === 'inbound-rtp' && v.codecId) result[v.kind + 'Codec'] = values.get(v.codecId)?.mimeType;
    });
    const canvas = document.createElement('canvas'); canvas.width = canvas.height = 1;
    const ctx = canvas.getContext('2d'); ctx.drawImage(video, 0, 0, 1, 1);
    result.pixel = Array.from(ctx.getImageData(0, 0, 1, 1).data);
    if (!window.__audioProbe || window.__audioProbe.stream !== video.srcObject) {
      if (window.__audioProbe) await window.__audioProbe.context.close();
      const context = new AudioContext();
      const source = context.createMediaStreamSource(video.srcObject);
      const analyser = context.createAnalyser(); analyser.fftSize = 4096;
      const mute = context.createGain(); mute.gain.value = 0;
      source.connect(analyser); analyser.connect(mute); mute.connect(context.destination);
      window.__audioProbe = { context, source, analyser, stream: video.srcObject };
    }
    const { context, analyser } = window.__audioProbe; await context.resume();
    const samples = new Float32Array(analyser.fftSize); analyser.getFloatTimeDomainData(samples);
    result.rms = Math.sqrt(samples.reduce((a, b) => a + b * b, 0) / samples.length);
    const spectrum = new Float32Array(analyser.frequencyBinCount); analyser.getFloatFrequencyData(spectrum);
    let peak = 1; for (let i = 2; i < spectrum.length; i++) if (spectrum[i] > spectrum[peak]) peak = i;
    result.peakHz = peak * context.sampleRate / analyser.fftSize;
    return result;
  });
}

async function verify(page, output, label) {
  const end = Date.now() + 45000;
  let before;
  while (Date.now() < end) {
    before = await stats(page);
    if (before.state === 'connected' && before.width === 1280 && before.decodedFrames > 20) break;
    await sleep(250);
  }
  assert.equal(before.state, 'connected', label + ': connection');
  await sleep(2200);
  const after = await stats(page);
  fs.writeFileSync(path.join(output, label + '-stats.json'), JSON.stringify({ before, after }, null, 2));
  assert(after.decodedFrames > before.decodedFrames + 20 && after.time > before.time + 1,
    label + ': decoded video and playback progression');
  assert(after.audioPackets > before.audioPackets + 20, label + ': audio packets');
  assert(after.rms > .01 && after.peakHz > 410 && after.peakHz < 470, label + ': decoded 440Hz tone');
  assert(after.pixel[0] > 220 && after.pixel[1] > 100 && after.pixel[1] < 150 && after.pixel[2] < 70,
    label + ': decoded orange pixels');
  assert.equal(after.videoCodec.toLowerCase(), 'video/h264');
  assert.equal(after.audioCodec.toLowerCase(), 'audio/opus');
  await page.screenshot({ path: path.join(output, label + '.png') });
  return { ...after, decodedFramesDelta: after.decodedFrames - before.decodedFrames,
    playbackSecondsDelta: after.time - before.time, screenshot: label + '.png' };
}

async function checkEyevinn(browsers, endpoint, output) {
  const report = { player: 'Eyevinn WebRTC Player', url: playerUrl, adapter: 'whep', requests: [], assets: [], samples: {} };
  const pages = [], pending = [];
  try {
    for (const [i, browser] of browsers.entries()) {
      const page = await browser.newPage({ viewport: { width: 1280, height: 900 } }); pages.push(page);
      page.on('console', msg => { if (msg.type() === 'error') (report.consoleErrors ||= []).push(msg.text()); });
      page.on('response', response => {
        if (response.url().startsWith('https://whep.vdo.ninja/'))
          report.requests.push({ browser: i, method: response.request().method(), status: response.status() });
        if (response.request().resourceType() === 'script') pending.push((async () => {
          try { report.assets.push({ url: response.url(), sha256: crypto.createHash('sha256').update(await response.body()).digest('hex') }); }
          catch (_) { /* Navigation can cancel an unrelated script load. */ }
        })());
      });
      await page.addInitScript(() => {
        window.__observedPeers = [];
        const Original = window.RTCPeerConnection;
        window.RTCPeerConnection = class extends Original {
          constructor(...args) { super(...args); window.__observedPeers.push(this); }
        };
      });
      await page.goto(playerUrl);
      await page.getByRole('button', { name: 'WHEP', exact: true }).click();
      await page.locator('#channelUrl').fill(endpoint);
      await page.screenshot({ path: path.join(output, `eyevinn-${i}-ready.png`) });
      await page.locator('#play').click();
      report.samples[i === 0 ? 'chromium' : 'firefox'] = await verify(page, output, `eyevinn-${i}-playing`);
    }
    await pages[0].locator('#stop').click();
    await sleep(1500);
    assert.equal(await pages[0].evaluate(() => window.__observedPeers.every(p => p.connectionState === 'closed')), true);
    report.samples.survivor = await verify(pages[1], output, 'eyevinn-survivor');
    await pages[0].locator('#play').click();
    report.samples.rejoined = await verify(pages[0], output, 'eyevinn-rejoined');
    for (const page of pages) await page.locator('#stop').click();
    report.ok = true;
    return report;
  } catch (error) {
    report.error = String(error.stack || error);
    for (const [i, page] of pages.entries()) await page.screenshot({ path: path.join(output, `eyevinn-${i}-failure.png`) }).catch(() => {});
    throw error;
  } finally {
    for (const page of pages) await page.close();
    await Promise.allSettled(pending);
    fs.writeFileSync(path.join(output, 'eyevinn-results.json'), JSON.stringify(report, null, 2));
  }
}
module.exports = { checkEyevinn };
