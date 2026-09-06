// Capture synthetic browser video markers and that browser's actual audio sink.
// Usage: node scripts/browser-av-sync-capture.cjs file|url TARGET OUTPUT
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { spawn, execFileSync } = require('node:child_process');
const { chromium, firefox } = require('playwright');
const { installProbes } = require('../tests/tools/rtc-timing-probes.cjs');
const { startPresentationCapture, stopPresentationCapture, collectViewerSnapshot } = require('./obs-websocket-vdoninja-publish-check.cjs');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const pactl = (...args) => execFileSync('pactl', args, { encoding:'utf8' }).trim();
async function main() {
  const [mode, target, outputArg] = process.argv.slice(2);
  if (!['file','url'].includes(mode) || !target || !outputArg) throw new Error('Specify file|url TARGET OUTPUT');
  const output = path.resolve(outputArg); fs.mkdirSync(output, { recursive:true });
  const sink = `ninja-av-${process.pid}`;
  const defaultBefore = pactl('get-default-sink');
  const moduleId = pactl('load-module','module-null-sink',`sink_name=${sink}`,'rate=48000','channels=1','channel_map=mono');
  const report = { mode, target, sink, defaultBefore, ok:false,
    clockBasis:'FFmpeg PulseAudio wall-clock timestamps, latency compensated; browser compositor timestamps.',
    scope:'Software playout at a dedicated virtual sink, not physical monitor/speaker latency.' };
  let browser, recorder, server, recorderExit;
  const log = fs.openSync(path.join(output,'audio-recorder.log'),'w');
  try {
    if (pactl('get-default-sink') === sink) {
      pactl('set-default-sink',defaultBefore); throw new Error('Virtual sink unexpectedly became the default');
    }
    recorder = spawn('ffmpeg',['-nostdin','-v','warning','-y','-copyts','-f','pulse',
      '-sample_rate','48000','-channels','1','-fragment_size','960','-i',`${sink}.monitor`,
      '-c:a','copy','-f','nut',path.join(output,'audio.nut')], { stdio:['ignore','ignore',log] });
    recorderExit = new Promise(resolve => recorder.once('exit',resolve));
    let url = target;
    if (mode === 'file') {
      const source = path.resolve(target), size = fs.statSync(source).size;
      server = http.createServer((req,res) => {
        if (req.url !== '/fixture.mp4') { res.setHeader('Content-Type','text/html');
          res.end('<!doctype html><video autoplay playsinline src="/fixture.mp4" style="width:100%"></video>'); return; }
        const range = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || '');
        const start = range ? Number(range[1]) : 0, end = range && range[2] ? Math.min(size-1,Number(range[2])) : size-1;
        res.writeHead(range ? 206 : 200, { 'Content-Type':'video/mp4','Accept-Ranges':'bytes',
          'Content-Length':end-start+1, ...(range ? { 'Content-Range':`bytes ${start}-${end}/${size}` } : {}) });
        fs.createReadStream(source,{start,end}).pipe(res);
      });
      await new Promise(resolve => server.listen(0,'127.0.0.1',resolve));
      url = `http://127.0.0.1:${server.address().port}/`;
    }
    const type = process.env.VDONINJA_VIEWER_BROWSER || 'chromium';
    if (!['chromium','firefox'].includes(type)) throw new Error('Unknown browser');
    const trials = process.env.VDONINJA_CHROMIUM_FIELD_TRIALS || '';
    if (trials && type !== 'chromium') throw new Error('Chromium field trials require Chromium');
    const executable = process.env.VDONINJA_BROWSER_EXECUTABLE;
    browser = await ({chromium,firefox}[type]).launch({
      ...(executable ? { executablePath:executable } : {}),
      env:{...process.env,PULSE_SINK:sink,
        ...(executable && type==='chromium' ? {CHROME_LOG_FILE:path.join(output,'chromium-process.log')} : {})},
      ignoreDefaultArgs:['--mute-audio'],
      ...(type === 'chromium' ? { args:['--autoplay-policy=no-user-gesture-required',
        ...(executable ? ['--enable-logging'] : []),
        ...(trials ? [`--force-fieldtrials=${trials}`] : [])] } : { firefoxUserPrefs:{'media.autoplay.default':0} }),
    });
    Object.assign(report,{browser:type,browserVersion:browser.version(),executable:executable||null,fieldTrials:trials});
    const context = await browser.newContext();
    if (mode === 'url') {
      const ice = JSON.parse(process.env.VDONINJA_VIEWER_ICE_SERVERS_JSON || 'null');
      if (!Array.isArray(ice) || !ice.length) throw new Error('Explicit private relay is required');
      await installProbes(context,ice,null);
    }
    const page = await context.newPage(); await page.goto(url,{waitUntil:'domcontentloaded'});
    await page.mouse.click(320,180);
    await page.waitForFunction(() => Array.from(document.querySelectorAll('video')).some(v => v.videoWidth>0),{},{timeout:60000});
    await page.evaluate(async () => { for (const v of document.querySelectorAll('video,audio')) {
      v.muted=false;v.volume=1;await v.play(); } });
    await sleep(20000);
    const sinks = JSON.parse(pactl('-f','json','list','sinks'));
    const sinkId = sinks.find(s => s.name === sink)?.index;
    report.sinkInputs = JSON.parse(pactl('-f','json','list','sink-inputs')).filter(s => s.sink === sinkId)
      .map(s => ({ index:s.index,sink:s.sink,binary:s.properties?.['application.process.binary'],
        processId:s.properties?.['application.process.id'],mute:s.mute }));
    if (!report.sinkInputs.length) throw new Error('Browser did not route audio to its dedicated sink');
    await startPresentationCapture(page,false,'counter-complement',{capturePresentedMarkers:true,allowFileVideo:mode==='file'});
    await sleep(30000);
    report.capture = await stopPresentationCapture(page);
    if (mode === 'url') {
      const snapshot = await collectViewerSnapshot(page);
      report.selectedCandidatePairs = (snapshot.pcStats || []).flatMap(s => s.selectedCandidatePairs || []);
      if (!report.selectedCandidatePairs.some(p => p.localCandidateType === 'relay' || p.remoteCandidateType === 'relay')) {
        throw new Error('Selected private relay transport was not observed');
      }
      report.codecs = await page.evaluate(async () => {
        const result=[];
        for (const pc of window.__pcList || []) { const stats=await pc.getStats();
          for (const s of stats.values()) if(s.type==='inbound-rtp' && s.kind==='video') result.push(stats.get(s.codecId)?.mimeType); }
        return result;
      });
      if (!report.codecs.length || report.codecs.some(c => c?.toLowerCase() !== 'video/h264')) throw new Error('H264 was not negotiated');
    }
    report.ok=true; // Capture success only; the separate matcher determines A/V alignment.
  } catch(error) { report.error=String(error.stack||error); }
  finally {
    if(browser) await browser.close();
    if(recorder && recorder.exitCode===null) recorder.kill('SIGINT');
    if(recorderExit) report.recorderExit=await recorderExit;
    if(server) await new Promise(resolve => server.close(resolve));
    fs.closeSync(log);
    pactl('unload-module',moduleId);report.defaultAfter=pactl('get-default-sink');
    fs.writeFileSync(path.join(output,'av-capture.json'),JSON.stringify(report,null,2));
  }
  console.log(JSON.stringify({ok:report.ok,output,error:report.error}));
  if(!report.ok) process.exitCode=1;
}
main().catch(e => { console.error(e);process.exitCode=1; });
