// Drive an isolated portable OBS instance through its real source/recording API.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { ObsWebSocketClient } = require('./obs-websocket-vdoninja-source-check.cjs');
const [url, stream, output] = process.argv.slice(2);
const wait = ms => new Promise(r => setTimeout(r, ms));
(async () => {
  const client = new ObsWebSocketClient(url);
  const report = { recordings: [] };
  try {
    await client.connect();
    report.version = await client.request('GetVersion');
    const { inputs } = await client.request('GetInputList');
    for (const input of inputs) await client.request('RemoveInput', { inputName: input.inputName });
    const scene = (await client.request('GetCurrentProgramScene')).currentProgramSceneName;
    const settings = { stream_id: stream, password: 'false', use_native_receiver: true,
      enable_data_channel: true, auto_reconnect: true, width: 640, height: 360 };
    async function connect() {
      await client.request('CreateInput', { sceneName: scene, inputName: 'Six independent channels',
        inputKind: 'vdoninja_source', inputSettings: settings, sceneItemEnabled: true });
      await wait(14000);
    }
    await connect();
    for (const cycle of ['initial', 'reconnected']) {
      await client.request('StartRecord');
      await wait(14000);
      const result = await client.request('StopRecord');
      assert(fs.existsSync(result.outputPath), 'OBS must finalize a real file');
      report.recordings.push({ cycle, path: result.outputPath });
      if (cycle === 'initial') {
        await client.request('RemoveInput', { inputName: 'Six independent channels' });
        await wait(1500);
        await connect();
      }
    }
    await client.request('RemoveInput', { inputName: 'Six independent channels' });
    report.ok = true;
  } catch (error) {
    report.error = String(error.stack || error); throw error;
  } finally {
    fs.writeFileSync(path.join(output, 'obs-workflow.json'), JSON.stringify(report, null, 2));
    await client.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
