#!/usr/bin/env python3
"""Install the bundle and exercise real OBS Flatpak under Xvfb.

Requires python3-websocket, xdotool and ImageMagick on the Linux host.
Uses a new, isolated OBS configuration; never opens an existing scene collection.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import uuid

import websocket


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    bundle = args.bundle.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    app = 'com.obsproject.Studio'
    extension = app + '.Plugin.VDONinja'
    if app in run('flatpak', 'ps', '--columns=application').splitlines():
        raise RuntimeError('Close OBS Flatpak before testing its plugin package')
    installed = subprocess.run(['flatpak', 'info', '--user', extension], capture_output=True)
    if installed.returncode == 0:
        # Flatpak 1.14 rejects reinstalling an identical bundle even with --reinstall.
        # Remove only the extension, preserving all OBS profiles and scene data.
        run('flatpak', 'uninstall', '--user', '-y', '--noninteractive', extension)
    run('flatpak', 'install', '--user', '-y', '--noninteractive', str(bundle))
    config = output / 'config/obs-studio'
    config.mkdir(parents=True)
    (config / 'user.ini').write_text('[General]\nFirstRun=true\nConfirmOnExit=false\n'
                                   '[BasicWindow]\nSetupWizard=false\n', encoding='utf-8')
    profile = config / 'basic/profiles/Flatpak'
    profile.mkdir(parents=True)
    (profile / 'basic.ini').write_text('[General]\nName=Flatpak\n'
        '[Video]\nBaseCX=1280\nBaseCY=720\nOutputCX=1280\nOutputCY=720\nFPSType=0\nFPSCommon=30\n',
        encoding='utf-8')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    password = uuid.uuid4().hex
    wsconfig = config / 'plugin_config/obs-websocket/config.json'
    wsconfig.parent.mkdir(parents=True)
    wsconfig.write_text(json.dumps({'server_enabled': True, 'server_port': port,
        'auth_required': True, 'server_password': password, 'first_load': False, 'alerts_enabled': False}))
    report = {'bundleSha256': hashlib.sha256(bundle.read_bytes()).hexdigest(),
              'obsCommit': run('flatpak', 'info', '--show-commit', app),
              'extensionCommit': run('flatpak', 'info', '--user', '--show-commit', extension)}
    client = None
    with (output / 'obs.log').open('w') as log:
        process = subprocess.Popen(['flatpak', 'run', '--socket=x11', '--nosocket=wayland',
            '--env=QT_QPA_PLATFORM=xcb',
            '--env=LIBGL_ALWAYS_SOFTWARE=1', '--command=sh', app, '-c',
            'export XDG_CONFIG_HOME="$1"; shift; exec obs "$@"', 'flatpak-validation',
            str(config.parent), '--multi', '--profile', 'Flatpak', '--disable-missing-files-check'],
            stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(120):
                if process.poll() is not None:
                    raise RuntimeError('OBS exited; see obs.log')
                try:
                    client = websocket.create_connection(f'ws://127.0.0.1:{port}', timeout=3)
                    break
                except (OSError, websocket.WebSocketException):
                    time.sleep(.5)
            if not client:
                raise RuntimeError('OBS WebSocket did not start; see obs.log')
            hello = json.loads(client.recv())['d']
            auth = hello['authentication']
            secret = base64.b64encode(hashlib.sha256((password + auth['salt']).encode()).digest()).decode()
            response = base64.b64encode(hashlib.sha256((secret + auth['challenge']).encode()).digest()).decode()
            client.send(json.dumps({'op': 1, 'd': {'rpcVersion': 1, 'authentication': response,
                                                  'eventSubscriptions': 0}}))
            assert json.loads(client.recv())['op'] == 2

            def request(name, data=None):
                deadline = time.monotonic() + 15
                while True:
                    request_id = uuid.uuid4().hex
                    client.send(json.dumps({'op': 6, 'd': {'requestType': name, 'requestId': request_id,
                                                           'requestData': data or {}}}))
                    reply = json.loads(client.recv())['d']
                    assert reply['requestId'] == request_id, reply
                    # WebSocket can accept connections before OBS finishes startup.
                    if reply['requestStatus']['code'] == 207 and time.monotonic() < deadline:
                        time.sleep(.25)
                        continue
                    assert reply['requestStatus']['result'], reply
                    return reply.get('responseData', {})

            report['version'] = request('GetVersion')
            report['inputKinds'] = request('GetInputKindList')['inputKinds']
            assert 'vdoninja_source' in report['inputKinds'], 'VDO.Ninja source not registered'
            scene = 'Flatpak validation'
            request('CreateScene', {'sceneName': scene})
            request('SetCurrentProgramScene', {'sceneName': scene})
            # Native receiver creation/destruction exercises the installed plugin, not a test double.
            for cycle in range(3):
                name = f'VDO.Ninja receiver {cycle}'
                request('CreateInput', {'sceneName': scene, 'inputName': name, 'inputKind': 'vdoninja_source',
                    'inputSettings': {'use_native_receiver': True, 'stream_id': '', 'password': 'false'},
                    'sceneItemEnabled': True})
                request('RemoveInput', {'inputName': name})
            report['receiverCreateDestroyCycles'] = 3
            time.sleep(3)
            # Verify the module loaded through the extension mount, not a host plugin path.
            matches = []
            for proc in Path('/proc').iterdir():
                if not proc.name.isdigit():
                    continue
                try:
                    if str(config.parent).encode() not in (proc / 'environ').read_bytes():
                        continue
                    maps = (proc / 'maps').read_text()
                    if '/app/plugins/VDONinja/lib/obs-plugins/obs-vdoninja.so' in maps:
                        matches.append(int(proc.name))
                except (OSError, ProcessLookupError):
                    pass
            assert len(matches) == 1, f'Expected one OBS loading our extension: {matches}'
            report['loadedModule'] = '/app/plugins/VDONinja/lib/obs-plugins/obs-vdoninja.so'
            client.close()
            client = None
            status = Path(f'/proc/{matches[0]}/status').read_text()
            namespace_pid = next(line.split()[-1] for line in status.splitlines() if line.startswith('NSpid:'))
            windows = run('xdotool', 'search', '--onlyvisible', '--pid', namespace_pid).splitlines()
            assert windows, 'OBS window not found'
            run('xdotool', 'windowfocus', windows[0])
            run('xdotool', 'key', 'alt+t')
            time.sleep(.5)
            run('import', '-window', 'root', str(output / 'obs-tools-menu.png'))
            run('xdotool', 'key', 'Up', 'Return')
            time.sleep(1)
            run('import', '-window', 'root', str(output / 'obs-flatpak.png'))
            # Ask Qt to close normally, verifying plugin shutdown too.
            # Qt may destroy the window before xdotool sends the final key-up.
            # The authoritative result is OBS's exit status below.
            subprocess.run(['xdotool', 'key', '--window', windows[0], 'ctrl+q'], capture_output=True)
            process.wait(timeout=30)
            assert process.returncode == 0, f'OBS exit: {process.returncode}'
            report['cleanShutdown'] = True
            (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps(report, indent=2))
        finally:
            if client:
                client.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            # The temporary WebSocket password is not part of retained evidence.
            if wsconfig.exists():
                wsconfig.unlink()


if __name__ == '__main__':
    main()
