"""Drive an unmodified-protocol Firefox build with WebDriver/BiDi.

Requires Selenium 4.35.0 and geckodriver; the diagnostic browser itself needs no
Playwright/Juggler patch. Measures a single viewer of an already running publisher.
"""
import argparse
import json
import os
import re
import runpy
from pathlib import Path
import subprocess
import time

from selenium import webdriver
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.firefox.service import Service
import websocket


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('url')
    p.add_argument('output', type=Path)
    p.add_argument('--browser', required=True)
    p.add_argument('--driver', required=True)
    p.add_argument('--fps', type=int, default=30)
    p.add_argument('--duration', type=float, default=30)
    p.add_argument('--reloads', type=int, default=0)
    p.add_argument('--headed', action='store_true')
    p.add_argument('--require-gpu', action='store_true')
    p.add_argument('--require-window-protocol', choices=['x11', 'xwayland', 'wayland'])
    p.add_argument('--relay-protocol', choices=['udp', 'tcp', 'tls'])
    a = p.parse_args()
    if a.reloads < 0 or a.reloads > 5 or a.duration < 1 or a.duration > 300 or not 0 < a.fps <= 240:
        p.error('Reloads must be 0–5, duration 1–300 seconds, and FPS 1–240')
    if a.require_gpu and not a.headed:
        p.error('--require-gpu requires --headed')
    if a.require_window_protocol and not a.headed:
        p.error('--require-window-protocol requires --headed')
    a.output.mkdir(parents=True, exist_ok=True)
    helper = Path(__file__).with_name('receiver-browser-scripts.cjs')
    scripts = json.loads(subprocess.check_output(['node', str(helper)], text=True))
    options = Options()
    options.binary_location = a.browser
    if not a.headed:
        options.add_argument('-headless')
    options.set_capability('webSocketUrl', True)
    for key, value in {'media.autoplay.default': 0, 'media.autoplay.block-webaudio': False,
                       'media.gmp-gmpopenh264.enabled': True}.items():
        options.set_preference(key, value)
    env = os.environ.copy()
    env.update(MOZ_LOG='sync,webrtc_trace:2', MOZ_LOG_FILE=str((a.output / 'firefox-native.log').resolve()))
    if env.get('VDONINJA_FIREFOX_COMPOSITOR_TRACE') == '1':
        env['MOZ_LOG'] += ',VdoninjaCompositor:3'
    report = {'ok': False, 'headed': a.headed, 'requireGpu': a.require_gpu,
              'requiredWindowProtocol': a.require_window_protocol, 'expectedFps': a.fps, 'durationSeconds': a.duration,
              'browserExecutable': a.browser, 'url': a.url,
              'timingTrace': env.get('VDONINJA_FIREFOX_TIMING_TRACE'),
              'compositorTrace': env.get('VDONINJA_FIREFOX_COMPOSITOR_TRACE') == '1',
              'timingSamples': env.get('VDONINJA_FIREFOX_TIMING_SAMPLES', 'default'),
              'coverage': {'decodedPixels': False, 'rawTrackAudio': False},
              'expectedRelayProtocol': a.relay_protocol,
              'preserveIceConfiguration': env.get('VDONINJA_PRESERVE_VIEWER_ICE_CONFIGURATION') == '1',
              'rounds': []}
    driver = bidi = None
    report_path = a.output / 'report.json'
    try:
        driver = webdriver.Firefox(options=options, service=Service(executable_path=a.driver, env=env,
                                   log_output=str(a.output / 'geckodriver.log')))
        driver.set_script_timeout(60)
        driver.set_page_load_timeout(60)
        report['browserVersion'] = driver.capabilities['browserVersion']
        report['browserBuildId'] = driver.capabilities.get('moz:buildID')
        pid = driver.capabilities.get('moz:processID')
        if pid:
            proc = Path('/proc') / str(pid)
            report['browserProcessExecutable'] = str((proc / 'exe').resolve())
            report['browserXulLibraries'] = sorted({line.split()[-1] for line in
                (proc / 'maps').read_text().splitlines() if 'libxul.so' in line})
        if a.headed:
            driver.get('about:support')
            deadline = time.monotonic() + 20
            while True:
                graphics = driver.execute_script("""return Object.fromEntries(
                  ['graphics-features-tbody','graphics-gpu-1-tbody','graphics-decisions-tbody'].map(id=>
                    [id,document.getElementById(id)?.innerText||'']));""")
                if ('Compositing' in graphics['graphics-features-tbody']
                        and 'Active' in graphics['graphics-gpu-1-tbody']):
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError('Firefox graphics diagnostics did not initialize')
                time.sleep(.25)
            report['graphics'] = graphics
            features = graphics['graphics-features-tbody']
            gpu = graphics['graphics-gpu-1-tbody']
            protocol = re.search(r'Window Protocol\s+([^\s]+)', features)
            report['windowProtocol'] = protocol.group(1) if protocol else None
            if a.require_window_protocol and report['windowProtocol'] != a.require_window_protocol:
                raise RuntimeError('Firefox did not use the required window protocol')
            report['gpuVerified'] = bool(re.search(r'Compositing\s+WebRender\s*(?:\n|$)', features)
                and re.search(r'Active\s+Yes', gpu)
                and not re.search(r'llvmpipe|lavapipe|swiftshader|software', gpu, re.I))
            if a.require_gpu and not report['gpuVerified']:
                raise RuntimeError('Firefox did not verify active hardware WebRender')
        bidi = websocket.create_connection(driver.capabilities['webSocketUrl'], timeout=30, suppress_origin=True)
        pre = scripts['preload']
        diagnostic_preload = '(' + scripts['applicationDiagnostics'] + ')();'
        bidi.send(json.dumps({'id': 1, 'method': 'script.addPreloadScript', 'params': {
            'functionDeclaration': f"() => {{ {diagnostic_preload} ({pre['source']})({json.dumps(pre['arg'])}); }}"}}))
        while True:
            response = json.loads(bidi.recv())
            if response.get('id') == 1:
                if response.get('type') == 'error':
                    raise RuntimeError(response)
                break

        def evaluate(script):
            result = driver.execute_async_script(
                'const done=arguments[arguments.length-1]; Promise.resolve((' + script['source'] +
                ')(arguments[0])).then(value=>done({value}),error=>done({error:String(error.stack||error)}));', script['arg'])
            if 'error' in result:
                raise RuntimeError(result['error'])
            return result['value']

        def diagnostics():
            # Closed Firefox peers may throw even when reading descriptions.
            return json.loads(driver.execute_script("""const safe=f=>{
              try { return f(); } catch(e) { return {error:String(e)}; }
            }; return JSON.stringify({
              peers:(window.__pcList||[]).map(p=>({
                state:safe(()=>p.connectionState),ice:safe(()=>p.iceConnectionState),
                signaling:safe(()=>p.signalingState),
                local:safe(()=>p.localDescription?.toJSON()),
                remote:safe(()=>p.remoteDescription?.toJSON())})),
              negotiation:(window.__negotiation||[]).map(e=>safe(()=>JSON.parse(JSON.stringify(e)))),
              applicationErrors:window.__applicationErrors,
              applicationPeers:safe(()=>Object.entries(session.rpcs||{}).map(([id,p])=>({id,probeIndex:(window.__pcList||[]).indexOf(p),state:p.signalingState}))),
              capabilities:window.__timingCapabilities});"""))

        for round_index in range(a.reloads + 1):
            phase = {'ok': False, 'round': round_index, 'expectedFps': a.fps,
                     'samples': [], 'startupCandidatePairs': []}
            report['rounds'].append(phase)
            driver.get(a.url)
            deadline = time.monotonic() + 60
            while True:
                snapshot = evaluate(scripts['snapshot'])
                phase['startupLastSnapshot'] = snapshot
                for stats in snapshot.get('pcStats', []):
                    for pair in stats.get('selectedCandidatePairs', []):
                        if pair not in phase['startupCandidatePairs'] and len(phase['startupCandidatePairs']) < 100:
                            phase['startupCandidatePairs'].append(pair)
                if sum(s.get('framesDecoded', 0) or 0 for s in snapshot.get('pcStats', [])) >= a.fps:
                    break
                if time.monotonic() > deadline:
                    phase['startupDiagnostics'] = diagnostics()
                    driver.save_screenshot(str(a.output / f'round-{round_index}-startup-failure.png'))
                    raise RuntimeError('Firefox did not decode live video')
                time.sleep(.25)
            time.sleep(20)
            phase['audioSetup'] = evaluate(scripts['startAudio'])
            evaluate(scripts['start'])
            print(f'MEASURE round={round_index}', flush=True)
            deadline = time.monotonic() + a.duration
            while True:
                phase['samples'].append(evaluate(scripts['snapshot']))
                if time.monotonic() >= deadline:
                    break
                time.sleep(min(1, max(0, deadline - time.monotonic())))
            phase['capture'] = evaluate(scripts['stop'])
            phase['audio'] = evaluate(scripts['stopAudio'])
            phase['diagnostics'] = diagnostics()
            pairs = [p for s in phase['samples'][-1].get('pcStats', []) for p in s.get('selectedCandidatePairs', [])]
            phase['relayVerified'] = any(p.get('localCandidateType') == 'relay' or p.get('remoteCandidateType') == 'relay' for p in pairs)
            phase['selectedCandidatePairs'] = pairs
            phase['relayProtocolVerified'] = not a.relay_protocol or any(
                p.get('localCandidateType') == 'relay' and p.get('localRelayProtocol') == a.relay_protocol for p in pairs)
            phase['codecs'] = evaluate({'arg': None, 'source': '''async () => {
              const codecs=[];for(const pc of window.__pcList||[]) {const s=await pc.getStats();
              for(const r of s.values()) if(r.type==='inbound-rtp'&&r.kind==='video') codecs.push(s.get(r.codecId)?.mimeType);}
              return codecs;}'''})
            phase['codecVerified'] = bool(phase['codecs']) and all(c and c.lower() == 'video/h264' for c in phase['codecs'])
            phase_path = a.output / f'round-{round_index}.json'
            phase_path.write_text(json.dumps(phase, indent=2))
            subprocess.run(['node', str(helper), 'analyze', str(phase_path)], check=True)
            report['rounds'][-1] = json.loads(phase_path.read_text())
        report['ok'] = all(phase['ok'] for phase in report['rounds'])
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
        if driver and report['rounds']:
            try:
                report['rounds'][-1]['failureDiagnostics'] = diagnostics()
            except Exception as diagnostic_error:
                report['diagnosticError'] = str(diagnostic_error)
    finally:
        cleanup_errors = []
        if bidi:
            try:
                bidi.close()
            except Exception as error:
                cleanup_errors.append(repr(error))
        if driver:
            try:
                driver.execute_script('for (const pc of window.__pcList || []) pc.close();')
                time.sleep(.5)  # Drain receiver/log queues before content-process teardown.
            except Exception as error:
                cleanup_errors.append(repr(error))
            try:
                driver.quit()
            except Exception as error:
                cleanup_errors.append(repr(error))
        if cleanup_errors:
            report['cleanupErrors'] = cleanup_errors
            report['ok'] = False
        if report['compositorTrace']:
            analyze_native = runpy.run_path(str(Path(__file__).with_name('analyze-firefox-compositor.py')))['analyze']
            logs = [(path.name, path.read_text()) for path in a.output.glob('*.moz_log') if path.stat().st_size]
            for phase in report['rounds']:
                if 'capture' not in phase:
                    continue
                matches, errors = [], []
                for name, text in logs:
                    try:
                        matches.append(dict(analyze_native(text, phase['capture'], a.fps), sourceLog=name))
                    except (ValueError, KeyError, TypeError) as error:
                        errors.append(f'{name}: {type(error).__name__}: {error}')
                native = matches[0] if len(matches) == 1 else {
                    'ok': False, 'error': 'Require exactly one matching native compositor log', 'details': errors}
                phase['nativeCompositor'] = native
                phase['callbackAndStatsOk'] = phase.get('ok', False)
                phase['ok'] = phase['callbackAndStatsOk'] and native['ok']
                (a.output / f"round-{phase['round']}.json").write_text(json.dumps(phase, indent=2))
                print(json.dumps({'round': phase['round'], 'nativeCompositor': native['ok']}), flush=True)
            report['ok'] = report['ok'] and all(phase.get('ok', False) for phase in report['rounds'])
        report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps({'ok': report['ok'], 'output': str(a.output), 'error': report.get('error')}))
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
