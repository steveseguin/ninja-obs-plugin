"""Real OBS -> unchanged WHIP/WHEP server -> independent Chrome and Firefox players.

The loopback broker uses the existing whip_private checkout without editing it.
--broker hosted tests the deployed whep.vdo.ninja endpoint. No server deployment is performed.
"""
import argparse
import configparser
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time
import uuid
import math
import struct
import wave

import psutil


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--install-prefix', type=Path, default=Path('install-obs32'),
                        help='cmake --install prefix holding obs-plugins/64bit and data/obs-plugins')
    parser.add_argument('--obs-template', type=Path, default=Path('_obs-portable'))
    parser.add_argument('--obs-review', type=Path, default=Path('_obs-portable-whep-review'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cycles', type=int, default=2)
    parser.add_argument('--broker', choices=['local', 'hosted'], default='local')
    parser.add_argument('--broker-source', type=Path, default=Path('../whip_private'),
                        help='whip_private checkout whose tests/server-fixture.cjs starts the loopback broker')
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    with wave.open(str(output / 'tone.wav'), 'wb') as tone:
        tone.setnchannels(2); tone.setsampwidth(2); tone.setframerate(48000)
        tone.writeframes(b''.join(struct.pack('<hh', *([int(8000 * math.sin(2 * math.pi * 440 * i / 48000))] * 2))
                                  for i in range(48000 * 4)))
    prefix = args.install_prefix.resolve()
    plugin = prefix / 'obs-plugins/64bit/obs-vdoninja.dll'
    data = prefix / 'data/obs-plugins/obs-vdoninja'
    assert plugin.is_file() and (data / 'locale/en-US.ini').is_file(), f'Install the plugin into {prefix} first'
    review = args.obs_review.resolve()
    template = args.obs_template.resolve()
    assert review != template and review.is_relative_to(Path.cwd().resolve()), "Review must be inside this checkout"
    if not review.exists():
        def copy_review(source, destination):
            # Runtime binaries are immutable; only configuration is copied.
            if Path(source).is_relative_to(template / 'config'):
                return shutil.copy2(source, destination)
            os.link(source, destination)
            return destination
        shutil.copytree(template, review, copy_function=copy_review)
    assert not any(Path(p.exe()).is_relative_to(review) for p in psutil.process_iter(['name'])
                   if (p.info['name'] or '').lower() == 'obs64.exe'), 'Review OBS already running'
    target_dll = review / 'obs-plugins/64bit/obs-vdoninja.dll'
    if target_dll.exists(): target_dll.unlink()  # Break the review link before replacement.
    shutil.copy2(plugin, target_dll)
    user_dll = review / 'config/obs-studio/plugins/obs-vdoninja/bin/64bit/obs-vdoninja.dll'
    if user_dll.exists(): user_dll.unlink()
    target_data = review / 'data/obs-plugins/obs-vdoninja'
    if target_data.exists(): shutil.rmtree(target_data)
    shutil.copytree(data, target_data)

    profile = review / 'config/obs-studio/basic/profiles/Untitled/basic.ini'
    config = configparser.ConfigParser(interpolation=None, strict=False); config.optionxform = str
    config.read(profile, encoding='utf-8-sig')
    updates = {
        'Output': {'Mode': 'Simple'},
        'SimpleOutput': {'StreamEncoder': 'x264', 'Preset': 'veryfast', 'VBitrate': '2500',
                         'StreamAudioEncoder': 'opus', 'ABitrate': '128'},
        'Video': {'BaseCX': '1280', 'BaseCY': '720', 'OutputCX': '1280', 'OutputCY': '720', 'FPSCommon': '30'},
    }
    for section, values in updates.items():
        if not config.has_section(section): config.add_section(section)
        for key, value in values.items(): config[section][key] = value
    with profile.open('w', encoding='utf-8') as f: config.write(f, space_around_delimiters=False)

    with socket.socket() as s: s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
    password = uuid.uuid4().hex
    wsconfig = review / 'config/obs-studio/plugin_config/obs-websocket/config.json'
    wsconfig.parent.mkdir(parents=True, exist_ok=True)
    wsconfig.write_text(json.dumps({'server_enabled': True, 'server_port': port, 'auth_required': True,
                                   'server_password': password, 'first_load': False, 'alerts_enabled': False}))
    report = {'pluginSha256': hashlib.sha256(target_dll.read_bytes()).hexdigest(), 'plugin': str(target_dll),
              'obsExe': str(review / 'bin/64bit/obs64.exe'), 'cycles': args.cycles, 'broker': args.broker}
    children, logs = [], []
    def launch(command, name, cwd=None, env=None):
        log = (output / (name + '.log')).open('w'); logs.append(log)
        p = subprocess.Popen([str(x) for x in command], cwd=cwd, env=env, stdout=log,
                             stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        children.append(p); return p
    def until(predicate, timeout=40):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if predicate(): return
            time.sleep(.2)
        raise AssertionError('Workflow timed out')
    logs_dir = review / 'config/obs-studio/logs'
    before = set(logs_dir.glob('*.txt')) if logs_dir.exists() else set()
    obs = None
    obs_env = dict(os.environ)
    broker_base = 'https://whep.vdo.ninja'
    try:
        if args.broker == 'local':
            fixture = (args.broker_source / 'tests/server-fixture.cjs').resolve()
            assert fixture.is_file(), f'Missing broker fixture {fixture}'
            report['brokerSource'] = str(fixture.parent.parent / 'whip.js')
            report['brokerSha256'] = hashlib.sha256((fixture.parent.parent / 'whip.js').read_bytes()).hexdigest()
            broker = launch(['node', fixture], 'broker', cwd=fixture.parent.parent)
            def broker_port():
                assert broker.poll() is None, 'Broker exited'
                return 'TEST_PORT ' in (output / 'broker.log').read_text()
            until(broker_port, 20)
            port_line = next(l for l in (output / 'broker.log').read_text().splitlines() if l.startswith('TEST_PORT '))
            broker_base = 'http://127.0.0.1:' + port_line.split()[1]
            obs_env['VDONINJA_WHEP_ENDPOINT'] = broker_base.replace('http://', 'ws://')
        report['brokerBase'] = broker_base
        obs = launch([review / 'bin/64bit/obs64.exe', '--portable', '--multi', '--minimize-to-tray',
                      '--disable-updater', '--disable-missing-files-check', '--disable-shutdown-check', '--profile', 'Untitled'],
                     'obs', cwd=review / 'bin/64bit', env=obs_env)
        def obs_ready():
            assert obs.poll() is None, 'OBS exited'
            try:
                with socket.create_connection(('127.0.0.1', port), .2): return True
            except OSError: return False
        until(obs_ready, 60)
        modules = [m.path for m in psutil.Process(obs.pid).memory_maps() if Path(m.path).name.lower() == 'obs-vdoninja.dll']
        assert len(modules) == 1 and Path(modules[0]).resolve() == target_dll.resolve(), modules
        report['loadedModule'] = modules[0]
        node = subprocess.run(['node', str(Path(__file__).with_name('whep-host-check.cjs')),
            'ws://127.0.0.1:' + str(port), str(output), str(args.cycles), broker_base],
            env=dict(os.environ, OBS_WEBSOCKET_PASSWORD=password), capture_output=True, text=True, timeout=600)
        (output / 'driver.log').write_text(node.stdout + node.stderr)
        workflow_path = output / 'whep-workflow.json'
        report['workflow'] = json.loads(workflow_path.read_text()) if workflow_path.exists() else None
        assert node.returncode == 0, node.stderr[-4000:]
        # WM_CLOSE lets OBS finalize its own state and tear down the plugin.
        callback = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        def close(hwnd, _):
            pid = ctypes.c_ulong(); ctypes.windll.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == obs.pid: ctypes.windll.user32.PostMessageW(hwnd, 0x0010, 0, 0)
            return True
        ctypes.windll.user32.EnumWindows(callback(close), 0)
        report['obsExitCode'] = obs.wait(timeout=30)
        report['cleanExit'] = report['obsExitCode'] == 0
        new_logs = sorted(set(logs_dir.glob('*.txt')) - before, key=lambda p: p.stat().st_mtime)
        assert new_logs, 'OBS wrote no log'
        text = new_logs[-1].read_text(encoding='utf-8', errors='replace')
        shutil.copy2(new_logs[-1], output / 'obs-log.txt')
        markers = {
            'whepHostingConnected': text.count('WHEP link hosting connected to ' + broker_base.replace('http://', 'ws://')
                                              .replace('https://whep.vdo.ninja', 'wss://whep.vdo.ninja')),
            'outputStopped': text.count('VDO.Ninja output stopped'),
            'whepOfferRejected': text.count('WHEP offer rejected'),
            'whepHostConnectionEnded': text.count('WHEP host connection ended'),
            'startThreadCrashed': text.count('start thread crashed'),
            'warnings': sum(1 for line in text.splitlines() if '[obs-vdoninja]' in line and 'warning' in line.lower()),
        }
        report['obsLogMarkers'] = markers
        # Two watched cycles plus one immediate stop each start the output and the broker link.
        assert markers['whepHostingConnected'] >= args.cycles, markers
        assert markers['outputStopped'] >= args.cycles + 1, markers
        assert markers['startThreadCrashed'] == 0, markers
        assert report['cleanExit'], report['obsExitCode']
        report['ok'] = True
    except BaseException as error:
        report['error'] = str(error); raise
    finally:
        for child in children:
            if child.poll() is None:
                subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'], capture_output=True)
            child.wait(timeout=15)
        for log in logs: log.close()
        (output / 'results.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__': main()
