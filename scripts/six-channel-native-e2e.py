"""Packaged publisher -> native portable OBS -> six-channel recordings.

The capture buffer is synthetic; OBS reception, mixing and recording are real.
Never substitutes this result for physical six-input hardware validation.
"""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time
import uuid
from urllib.request import Request, urlopen

import numpy as np
import psutil


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--publisher', type=Path, required=True)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--game-capture', type=Path, required=True)
    parser.add_argument('--obs-template', type=Path, required=True)
    parser.add_argument('--obs-review', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--stereo', action='store_true')
    parser.add_argument('--publisher-channels', type=int, choices=(1, 2, 6), default=6)
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    review = args.obs_review.resolve()
    assert review != args.obs_template.resolve()
    if not review.exists():
        template = args.obs_template.resolve()
        def copy_review(source, destination):
            # Runtime binaries are immutable; only configuration is copied.
            if Path(source).is_relative_to(template / 'config'):
                return shutil.copy2(source, destination)
            os.link(source, destination)
            return destination
        shutil.copytree(template, review, copy_function=copy_review)
    assert not any(p.name().lower() == 'obs64.exe' and Path(p.exe()).is_relative_to(review)
                   for p in psutil.process_iter() if p.name().lower() == 'obs64.exe'), 'Review OBS already running'
    target_dll = review / 'obs-plugins/64bit/obs-vdoninja.dll'
    if target_dll.exists(): target_dll.unlink()  # Break the review link before replacement.
    shutil.copy2(args.plugin, target_dll)
    profile = review / 'config/obs-studio/basic/profiles/Untitled/basic.ini'
    config = configparser.ConfigParser(interpolation=None, strict=False); config.optionxform = str
    config.read(profile, encoding='utf-8-sig')
    updates = {
        'Audio': {'SampleRate': '48000', 'ChannelSetup': 'Stereo' if args.stereo else '5.1'},
        'Output': {'Mode': 'Advanced', 'FilenameFormatting': '%CCYY-%MM-%DD %hh-%mm-%ss'},
        'Video': {'BaseCX': '640', 'BaseCY': '360', 'OutputCX': '640', 'OutputCY': '360', 'FPSCommon': '30'},
        'AdvOut': {'RecType': 'Standard', 'RecFilePath': output.as_posix(), 'RecFormat2': 'mkv', 'RecTracks': '1',
                   'RecEncoder': 'obs_x264', 'RecAudioEncoder': 'ffmpeg_aac', 'Track1Bitrate': '384',
                   'RecUseRescale': 'false', 'RecSplitFile': 'false'},
    }
    for section, values in updates.items():
        if not config.has_section(section): config.add_section(section)
        for key, value in values.items(): config[section][key] = value
    with profile.open('w', encoding='utf-8') as f: config.write(f, space_around_delimiters=False)
    (profile.parent / 'recordEncoder.json').write_text(json.dumps({'rate_control': 'CRF', 'crf': 23, 'preset': 'ultrafast'}))
    with socket.socket() as s: s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
    password = uuid.uuid4().hex
    wsconfig = review / 'config/obs-studio/plugin_config/obs-websocket/config.json'
    wsconfig.parent.mkdir(parents=True, exist_ok=True)
    wsconfig.write_text(json.dumps({'server_enabled': True, 'server_port': port, 'auth_required': True,
                                   'server_password': password, 'first_load': False, 'alerts_enabled': False}))
    stream = 'sixobs' + uuid.uuid4().hex
    capture = args.game_capture.resolve(); exe = args.publisher.resolve()
    assert args.publisher_channels == 6 or args.stereo, 'Mono/stereo regression uses a stereo OBS profile'
    report = {'syntheticCapture': True, 'instrumentedCapture': args.publisher_channels == 6,
              'publisherChannels': args.publisher_channels, 'physicalInputTested': False, 'stereoObsProfile': args.stereo,
              'publisherSha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'pluginSha256': hashlib.sha256(target_dll.read_bytes()).hexdigest(), 'recordings': []}
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
    discovery = output / 'control.json'
    def quit_publisher():
        data = json.loads(discovery.read_text())
        req = Request(data['base_url'] + '/commands', data=b'{"command":"quit"}',
                      headers={'Authorization': 'Bearer ' + data['token'], 'Content-Type': 'application/json'})
        with urlopen(req, timeout=5) as r: r.read()
    try:
        launch(['python', capture / 'native-qt/e2e/six-channel-source.py'], 'source')
        until(lambda: 'SIX_CHANNEL_SOURCE_READY' in (output / 'source.log').read_text())
        publisher_command = ([capture / 'native-qt/.cache/desktop-ui-python/Scripts/python.exe',
            capture / 'native-qt/e2e/six-channel-instrumented-launch.py', exe]
            if args.publisher_channels == 6 else [exe])
        publisher = launch(publisher_command + [
            '--headless', '--source=window', '--window=Game Capture Six Channel Source',
            '--audio-source=selected-window', '--audio-channels=' + str(args.publisher_channels),
            '--audio-bitrate-kbps=384', '--no-audio-red',
            '--resolution=640x360', '--fps=30', '--video-encoder=software', '--stream=' + stream,
            '--password=false', '--ice-mode=host', '--duration-ms=180000', '--local-control',
            '--local-control-discovery=' + str(discovery)], 'publisher', env=dict(os.environ,
                LOCALAPPDATA=str(output), GAME_CAPTURE_SUPPRESS_FIREWALL_WARNING='1'))
        until(lambda: discovery.exists() and (args.publisher_channels != 6 or
            'synthetic-six-channel-capture' in (output / 'publisher.log').read_text()))
        obs = launch([review / 'bin/64bit/obs64.exe', '--portable', '--multi', '--minimize-to-tray',
                      '--disable-updater', '--disable-missing-files-check'], 'obs', cwd=review / 'bin/64bit')
        def obs_ready():
            assert obs.poll() is None, 'OBS exited'
            try:
                with socket.create_connection(('127.0.0.1', port), .2): return True
            except OSError: return False
        until(obs_ready)
        modules = [m.path for m in psutil.Process(obs.pid).memory_maps() if Path(m.path).name.lower() == 'obs-vdoninja.dll']
        assert len(modules) == 1 and Path(modules[0]).resolve() == target_dll.resolve(), modules
        report['loadedModule'] = modules[0]
        node = subprocess.run(['node', str(Path(__file__).with_name('six-channel-native-record.cjs')),
            'ws://127.0.0.1:' + str(port), stream, str(output)], env=dict(os.environ, OBS_WEBSOCKET_PASSWORD=password),
            capture_output=True, text=True, timeout=100)
        (output / 'driver.log').write_text(node.stdout + node.stderr)
        assert node.returncode == 0, node.stderr
        workflow = json.loads((output / 'obs-workflow.json').read_text())
        ffmpeg = exe.parent / 'ffmpeg/bin/ffmpeg.exe'
        frequencies = [300, 500, 700, 90, 1100, 1300]
        for recording in workflow['recordings']:
            media = Path(recording['path'])
            probe = subprocess.run([str(ffmpeg), '-hide_banner', '-i', str(media)], capture_output=True)
            metadata = probe.stderr.decode('utf-8', errors='replace')
            (output / (recording['cycle'] + '-metadata.txt')).write_text(metadata)
            assert ('stereo' if args.stereo else '5.1') in metadata, metadata
            decoded = subprocess.run([str(ffmpeg), '-v', 'error', '-ss', '4', '-i', str(media), '-t', '5',
                '-map', '0:a:0', '-f', 'f32le', '-acodec', 'pcm_f32le', '-'], capture_output=True, check=True)
            samples = np.frombuffer(decoded.stdout, dtype='<f4').reshape(-1, 2 if args.stereo else 6)
            assert len(samples) > 48000 * 4
            t = np.arange(len(samples)) / 48000
            # Hann window tolerates resampling drift and codec priming.
            window = np.hanning(len(samples)); matrix = []
            spectrum = abs(np.fft.rfft(samples * window[:, None], axis=0)) * 2 / window.sum()
            bins = np.fft.rfftfreq(len(samples), 1/48000)
            for channel in range(samples.shape[1]):
                matrix.append([float(spectrum[abs(bins-frequency) < 3, channel].max()) for frequency in frequencies])
            if args.publisher_channels != 6:
                # Actual Windows loopback -> ordinary mono/stereo Opus. The
                # system playback endpoint may already mix the six test tones.
                for row in matrix: assert max(row) > .003, matrix
            elif args.stereo:
                # OBS's standard 5.1 -> stereo mix includes front/center/surround
                # speakers; LFE is deliberately omitted, not folded into L/R.
                for tone in (0, 1, 2, 4, 5): assert max(row[tone] for row in matrix) > .003, matrix
                assert max(row[3] for row in matrix) < .001, matrix
            else:
                for channel, row in enumerate(matrix):
                    assert row[channel] > .01, matrix
                    assert max(v for i, v in enumerate(row) if i != channel) < row[channel] * .1, matrix
            report['recordings'].append(dict(recording, frequencyAmplitudes=matrix, frames=len(samples)))
        quit_publisher(); assert publisher.wait(timeout=20) == 0
        # WM_CLOSE lets OBS finalize its own state and teardown the plugin.
        import ctypes
        callback = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        def close(hwnd, _):
            pid = ctypes.c_ulong(); ctypes.windll.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == obs.pid: ctypes.windll.user32.PostMessageW(hwnd, 0x0010, 0, 0)
            return True
        ctypes.windll.user32.EnumWindows(callback(close), 0)
        assert obs.wait(timeout=25) == 0
        report['cleanExit'] = True; report['ok'] = True
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
