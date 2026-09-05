// Opt-in recording/control test. Use an isolated OBS profile with WebSocket enabled.
// Usage: node scripts/obs-linux-e2e-record.cjs file MEDIA_FILE | native STREAM_ID | clock LABEL
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const childProcess = require('child_process');
const { ObsWebSocketClient } = require('./obs-websocket-vdoninja-source-check.cjs');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function main() {
  const [mode, input] = process.argv.slice(2);
  if (!['file', 'native', 'clock'].includes(mode) || !input) throw new Error('Specify file MEDIA_FILE, native STREAM_ID or clock LABEL');
  const durationMs = Number(process.env.E2E_DURATION_MS || 30000);
  const width = Number(process.env.E2E_WIDTH || 1280);
  const height = Number(process.env.E2E_HEIGHT || 720);
  const fps = Number(process.env.E2E_FPS || 30);
  if (!Number.isFinite(durationMs) || durationMs <= 0 ||
      ![width, height, fps].every(value => Number.isInteger(value) && value > 0)) {
    throw new Error('Duration must be positive and finite; dimensions and FPS must be positive integers');
  }
  const output = path.resolve(process.env.E2E_OUTPUT_DIR || `artifacts/linux-e2e/${mode}-record`);
  fs.mkdirSync(output, { recursive: true });
  const client = new ObsWebSocketClient(process.env.OBS_WEBSOCKET_URL || 'ws://127.0.0.1:4455');
  const scene = `Linux E2E ${Date.now()}`;
  const name = `${scene} input`;
  const report = { mode, input, width, height, fps, samples: [] };
  const waitForRecordingStop = async () => {
    for (let attempt = 0; attempt < 100; attempt++) {
      if (!(await client.request('GetRecordStatus')).outputActive) return;
      await sleep(100);
    }
    throw new Error('OBS recording did not stop within ten seconds');
  };
  let priorScene, priorVideo, priorDirectory, created = false, inputCreated = false, recording = false;
  try {
    await client.connect();
    if ((await client.request('GetStreamStatus')).outputActive || (await client.request('GetRecordStatus')).outputActive) {
      throw new Error('OBS must be idle before starting this test');
    }
    report.obsVersion = await client.request('GetVersion');
    priorScene = (await client.request('GetCurrentProgramScene')).currentProgramSceneName;
    priorVideo = await client.request('GetVideoSettings');
    priorDirectory = (await client.request('GetRecordDirectory')).recordDirectory;
    await client.request('SetRecordDirectory', { recordDirectory: output });
    await client.request('SetVideoSettings', { baseWidth: width, baseHeight: height, outputWidth: width,
      outputHeight: height, fpsNumerator: fps, fpsDenominator: 1 });
    await client.request('CreateScene', { sceneName: scene });
    created = true;
    await client.request('CreateInput', { sceneName: scene, inputName: name,
      inputKind: mode === 'file' ? 'ffmpeg_source' : mode === 'clock' ? 'vdoninja_clock_fixture' : 'vdoninja_source',
      inputSettings: mode === 'clock' ? {} : mode === 'file'
        ? { is_local_file: true, local_file: path.resolve(input), looping: false, restart_on_activate: true }
        : { stream_id: input, password: process.env.VDONINJA_PASSWORD || 'false', use_native_receiver: true,
          auto_reconnect: true, width, height }, sceneItemEnabled: true });
    inputCreated = true;
    await client.request('SetCurrentProgramScene', { sceneName: scene });
    await sleep(10000);
    await client.request('StartRecord');
    recording = true;
    const deadline = Date.now() + durationMs;
    while (Date.now() < deadline) {
      report.samples.push({ timestampMs: Date.now(), stats: await client.request('GetStats') });
      await sleep(Math.min(1000, Math.max(0, deadline - Date.now())));
    }
    report.recording = await client.request('StopRecord');
    await waitForRecordingStop();
    recording = false;
    if (mode === 'clock' || process.env.E2E_REQUIRE_VISUAL_SEQUENCE === '1') {
      const analysis = childProcess.spawnSync(process.execPath,
        [path.join(__dirname, 'analyze-publish-sequence-video.cjs'), report.recording.outputPath], { encoding: 'utf8' });
      if (analysis.error) throw analysis.error;
      report.visualSequenceAnalysis = JSON.parse(analysis.stdout);
      if (analysis.status !== 0 || !report.visualSequenceAnalysis.ok) {
        throw new Error('Recorded frame-counter validation failed');
      }
    }
    if (mode === 'native') {
      // Retire the current receiver and prove moving video returns after restoring its stream.
      await client.request('SetInputSettings', { inputName: name, inputSettings: { stream_id: `${input}_offline` } });
      await sleep(2000);
      await client.request('SetInputSettings', { inputName: name, inputSettings: { stream_id: input } });
      await sleep(10000);
      const hashes = [];
      for (let i = 0; i < 2; i++) {
        const screenshot = await client.request('GetSourceScreenshot', { sourceName: name, imageFormat: 'png',
          imageWidth: 640, imageHeight: 360 });
        const bytes = Buffer.from(screenshot.imageData.split(',')[1], 'base64');
        if (bytes.length < 5000) throw new Error('Recovered receiver image is unexpectedly small');
        fs.writeFileSync(path.join(output, `recovered-${i}.png`), bytes);
        hashes.push(crypto.createHash('sha256').update(bytes).digest('hex'));
        await sleep(2000);
      }
      if (hashes[0] === hashes[1]) throw new Error('Receiver did not resume moving video');
      report.recovery = { movingVideo: true, hashes };
    }
    report.passed = true;
  } catch (error) {
    report.passed = false;
    report.failure = String(error);
    throw error;
  } finally {
    // Record cleanup failures rather than allowing a passing report to hide them.
    try {
      if (recording) {
        await client.request('StopRecord');
        await waitForRecordingStop();
      }
      if (priorScene) await client.request('SetCurrentProgramScene', { sceneName: priorScene });
      if (inputCreated) await client.request('RemoveInput', { inputName: name });
      if (created) await client.request('RemoveScene', { sceneName: scene });
      if (priorVideo) await client.request('SetVideoSettings', priorVideo);
      if (priorDirectory) await client.request('SetRecordDirectory', { recordDirectory: priorDirectory });
    } catch (error) {
      report.passed = false;
      report.cleanupFailure = String(error);
      process.exitCode = 1;
    }
    await client.close();
    fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(report, null, 2));
  }
  if (report.passed) console.log(`PASS: ${mode} recording${mode === 'native' ? ' and receiver recovery' : ''}; ${output}`);
}
main().catch(error => { console.error(error); process.exitCode = 1; });
