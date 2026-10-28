// Real OBS publisher -> native OBS receiver, with independently recorded output.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { ObsWebSocketClient } = require('./obs-websocket-vdoninja-source-check.cjs');
const [senderUrl, receiverUrl, stream, fixture, directory] = process.argv.slice(2);
const options = JSON.parse(fs.readFileSync(path.join(directory, 'options.json')));
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(fn, label, timeout = 45000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { if (await fn()) return; await wait(250); }
  throw new Error('Timed out: ' + label);
}
(async () => {
  const sender = new ObsWebSocketClient(senderUrl), receiver = new ObsWebSocketClient(receiverUrl);
  const report = { screenshots: [], recordings: [] };
  try {
    await sender.connect(); await receiver.connect();
    report.videoSettings = await sender.request('GetVideoSettings');
    assert.equal(report.videoSettings.fpsNumerator/report.videoSettings.fpsDenominator, options.fps || 30);
    assert.equal(report.videoSettings.outputWidth, options.width || 640);
    assert.equal(report.videoSettings.outputHeight, options.height || 360);
    for (const client of [sender, receiver]) {
      for (const input of (await client.request('GetInputList')).inputs)
        await client.request('RemoveInput', { inputName: input.inputName });
    }
    const scene = (await sender.request('GetCurrentProgramScene')).currentProgramSceneName;
    const receivedScene = (await receiver.request('GetCurrentProgramScene')).currentProgramSceneName;
    await sender.request('CreateInput', { sceneName: scene, inputName: 'Codec reference', inputKind: 'ffmpeg_source',
      inputSettings: { is_local_file: true, local_file: fixture, looping: true, restart_on_activate: true,
        hw_decode: false, clear_on_media_end: false }, sceneItemEnabled: true });
    await sender.request('SetStreamServiceSettings', { streamServiceType: 'vdoninja_service',
      streamServiceSettings: { stream_id: stream, password: 'false', enable_data_channel: true,
        room_id: '', auto_reconnect: true, max_viewers: 4, bitrate: 6000, ...options.service } });
    report.service = await sender.request('GetStreamServiceSettings');
    const sourceName = 'Codec native receive';
    async function attach() {
      await receiver.request('CreateInput', { sceneName: receivedScene, inputName: sourceName,
        inputKind: 'vdoninja_source', inputSettings: { stream_id: stream, password: 'false',
          use_native_receiver: true, width: options.width || 640, height: options.height || 360, auto_reconnect: true, enable_data_channel: true },
        sceneItemEnabled: true });
    }
    await sender.request('StartStream');
    await until(async () => (await sender.request('GetStreamStatus')).outputActive, 'publisher active');
    await sender.request('StartRecord');
    for (const phase of options.browserOnly ? [] : ['initial', 'viewer-reconnected']) {
      await attach(); await wait(15000);
      await receiver.request('StartRecord');
      for (let sample = 0; sample < 3; ++sample) {
        await wait(2000);
        const shot = await receiver.request('GetSourceScreenshot', { sourceName,
          imageFormat: 'png', imageWidth: 640, imageHeight: 360 });
        const file = path.join(directory, phase + '-' + sample + '.png');
        fs.writeFileSync(file, Buffer.from(shot.imageData.split(',')[1], 'base64'));
        report.screenshots.push(file);
      }
      const recording = await receiver.request('StopRecord');
      assert(fs.existsSync(recording.outputPath)); report.recordings.push(recording.outputPath);
      await receiver.request('RemoveInput', { inputName: sourceName });
      await wait(1000);
    }
    if (options.browserCheck) {
      const { checkCodecBrowsers } = require('./codec-browser-check.cjs');
      report.browsers = await checkCodecBrowsers(stream, options, directory, sender);
    }
    if (options.checkSharedColorGuard) {
      assert(options.service.preserve_source_color_format, 'Guard case requires source-format opt-in');
      await sender.request('StopStream');
      await until(async () => !(await sender.request('GetStreamStatus')).outputActive, 'stop while recording');
      const service = await sender.request('GetStreamServiceSettings');
      await sender.request('SetStreamServiceSettings', {...service,
        streamServiceSettings: {...service.streamServiceSettings, preserve_source_color_format: false}});
      const logs = path.join(directory, 'sender/config/obs-studio/logs');
      const log = fs.readdirSync(logs).map(n=>path.join(logs,n))
        .sort((a,b)=>fs.statSync(b).mtimeMs-fs.statSync(a).mtimeMs)[0];
      const offset = fs.readFileSync(log, 'utf8').length;
      // StartStream acknowledges the queued frontend request, even when the
      // output subsequently refuses to start. Observe the actual rejection.
      try { await sender.request('StartStream'); } catch {}
      await until(async () => fs.readFileSync(log, 'utf8').slice(offset)
        .includes('Shared recording encoder was started without VDO.Ninja compatibility settings'), 'shared color rejection');
      assert(!(await sender.request('GetStreamStatus')).outputActive,
        'Nonstandard shared encoder published without color opt-in');
      assert((await sender.request('GetRecordStatus')).outputActive, 'Guard disturbed the active recording');
      await sender.request('SetStreamServiceSettings', service);
      await sender.request('StartStream');
      await until(async () => (await sender.request('GetStreamStatus')).outputActive, 'opted-in shared encoder');
      report.sharedColorGuard = {rejectedWithoutOptIn: true, recordingPreserved: true, restartedWithOptIn: true};
    }
    report.senderRecording = (await sender.request('StopRecord')).outputPath;
    // StopRecord acknowledges the request before the shared encoder is released.
    await until(async () => !(await sender.request('GetRecordStatus')).outputActive, 'shared recording stopped');
    await sender.request('StopStream');
    await until(async () => !(await sender.request('GetStreamStatus')).outputActive, 'publisher stopped');
    // Start the same sender again, including codec/header and RTP state reset.
    await sender.request('StartStream');
    await until(async () => (await sender.request('GetStreamStatus')).outputActive, 'publisher restarted');
    if (!options.browserOnly) {
      await attach(); await wait(15000);
      const shot = await receiver.request('GetSourceScreenshot', { sourceName, imageFormat: 'png', imageWidth: 640, imageHeight: 360 });
      const file = path.join(directory, 'publisher-restarted.png');
      fs.writeFileSync(file, Buffer.from(shot.imageData.split(',')[1], 'base64')); report.screenshots.push(file);
      await receiver.request('RemoveInput', { inputName: sourceName });
    }
    await sender.request('StopStream');
    await until(async () => !(await sender.request('GetStreamStatus')).outputActive, 'final stop');
    report.ok = true;
  } catch (error) {
    report.error = String(error.stack || error); throw error;
  } finally {
    fs.writeFileSync(path.join(directory, 'workflow.json'), JSON.stringify(report, null, 2));
    await sender.close(); await receiver.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
