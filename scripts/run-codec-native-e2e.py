"""Two isolated portable OBS instances: publish, receive, compare colors and motion.

Requires Python numpy/Pillow/psutil and a complete portable OBS template. All
processes and configuration belong to this run; regular OBS is not modified.
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
import threading
import uuid

import numpy as np
from PIL import Image
import psutil

PATCHES = [(24, 180, 40), (190, 45, 45), (45, 70, 190), (180, 160, 30),
           (40, 170, 180), (165, 45, 160), (32, 32, 32), (210, 210, 210)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--encoder', required=True, help='OBS video encoder ID')
    parser.add_argument('--codec', choices=['H264', 'HEVC', 'AV1'], required=True)
    parser.add_argument('--color-format', choices=['NV12', 'I444', 'P010'], default='NV12')
    parser.add_argument('--preserve-color-format', action='store_true')
    parser.add_argument('--check-shared-color-guard', action='store_true')
    parser.add_argument('--color-space', choices=['601', '709'], default='709')
    parser.add_argument('--color-range', choices=['Partial', 'Full'], default='Partial')
    parser.add_argument('--browser-check', action='store_true')
    parser.add_argument('--browser-only', action='store_true', help='Skip native receiver checks for isolated browser/baseline comparisons')
    parser.add_argument('--browser-ice', default='null', help='Optional isolated TURN configuration as JSON')
    parser.add_argument('--service-options', default='{}', help='Additional service settings as JSON')
    parser.add_argument('--encoder-options', default='{}', help='Additional encoder settings as JSON')
    parser.add_argument('--size', choices=['640x360', '1280x720', '1920x1080'], default='640x360')
    parser.add_argument('--fps', choices=[30, 60], type=int, default=30)
    parser.add_argument('--stress', action='store_true', help='Add moving full-resolution texture to exercise encoder load')
    parser.add_argument('--plugin', type=Path, default=Path('build-plugin-release-validation/RelWithDebInfo/obs-vdoninja.dll'))
    parser.add_argument('--obs-template', type=Path, default=Path('_obs-portable'))
    parser.add_argument('--ffmpeg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.browser_only and not args.browser_check:
        parser.error('--browser-only requires --browser-check')
    if args.check_shared_color_guard and not args.preserve_color_format:
        parser.error('--check-shared-color-guard requires --preserve-color-format')
    width, height = map(int, args.size.split('x'))
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    template = args.obs_template.resolve(); plugin = args.plugin.resolve(); ffmpeg = args.ffmpeg.resolve()
    report = {'codec': args.codec, 'encoder': args.encoder, 'colorFormat': args.color_format,
              'size': args.size, 'fps': args.fps, 'stress': args.stress,
              'pluginSha256': hashlib.sha256(plugin.read_bytes()).hexdigest()}
    children, handles = [], []
    monitor_stop = threading.Event()
    monitor = None

    def launch(command, name, cwd=None, env=None):
        handle = (output / (name + '.log')).open('w'); handles.append(handle)
        child = subprocess.Popen([str(x) for x in command], cwd=cwd, env=env, stdout=handle,
                                 stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        children.append(child); return child

    def close_obs(process):
        callback = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        def close(hwnd, _):
            pid = ctypes.c_ulong(); ctypes.windll.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == process.pid: ctypes.windll.user32.PostMessageW(hwnd, 0x0010, 0, 0)
            return True
        ctypes.windll.user32.EnumWindows(callback(close), 0)
        assert process.wait(timeout=30) == 0, 'OBS did not shut down cleanly'

    try:
        fixture = output / 'reference.mkv'
        with (output / 'fixture.log').open('w') as log:
            encode = subprocess.Popen([str(ffmpeg), '-v', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                '-s', args.size, '-r', str(args.fps), '-i', '-', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
                '-t', '8', '-c:v', 'ffv1', '-pix_fmt', 'bgr0', '-c:a', 'pcm_s16le', '-ac', '2', str(fixture)],
                stdin=subprocess.PIPE, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(encode)
            random = np.random.default_rng(42)
            texture = random.integers(0,256,(height-220*height//360,width,3),dtype=np.uint8)
            for index in range(8 * args.fps):
                frame = np.empty((360, 640, 3), dtype=np.uint8); frame[:] = PATCHES[0]
                for patch, rgb in enumerate(PATCHES): frame[:100, patch*80:(patch+1)*80] = rgb
                frame[120:180, 64:576] = np.repeat(np.arange(256, dtype=np.uint8), 2)[None, :, None]
                frame[190:210, 64:576:2] = PATCHES[0]
                frame[190:210, 65:576:2] = PATCHES[5]
                x = (index * 7) % 560; frame[220:320, x:x+80] = (235, 225, 215)
                frame = np.repeat(np.repeat(frame, height//360, axis=0), width//640, axis=1)
                if args.stress:
                    frame[220*height//360:] = np.roll(texture, (index*3,index*7), axis=(0,1))
                encode.stdin.write(frame.tobytes())
            encode.stdin.close(); assert encode.wait(timeout=30) == 0
        password = uuid.uuid4().hex
        instances = []
        for role in ['sender', 'receiver']:
            review = output / role
            def copy_file(source, destination):
                relative = Path(source).relative_to(template)
                if relative.parts[0] == 'config' or relative.parts[0] == 'data':
                    return shutil.copy2(source, destination)
                os.link(source, destination); return destination
            shutil.copytree(template, review, copy_function=copy_file)
            target = review / 'obs-plugins/64bit/obs-vdoninja.dll'
            assert target.is_relative_to(output) and target != plugin
            if target.exists(): target.unlink()  # Break the template hard link before installing.
            shutil.copy2(plugin, target)
            shutil.copytree(Path('data'), review / 'data/obs-plugins/obs-vdoninja', dirs_exist_ok=True)
            profile = review / 'config/obs-studio/basic/profiles/Untitled/basic.ini'
            config = configparser.ConfigParser(interpolation=None, strict=False); config.optionxform = str
            config.read(profile, encoding='utf-8-sig')
            updates = {'Output': {'Mode': 'Advanced'},
                'Audio': {'SampleRate': '48000', 'ChannelSetup': 'Stereo'},
                'VDONinja': {'StreamID': '', 'RoomID': '', 'Password': 'false', 'AutoAddFeeds': 'false'},
                'Video': {'BaseCX': str(width), 'BaseCY': str(height), 'OutputCX': str(width), 'OutputCY': str(height),
                          'FPSType': '0', 'FPSCommon': str(args.fps),
                          'ColorFormat': args.color_format if role == 'sender' else 'NV12',
                          'ColorSpace': args.color_space, 'ColorRange': args.color_range},
                'AdvOut': {'Encoder': args.encoder, 'AudioEncoder': 'ffmpeg_opus', 'TrackIndex': '1',
                    'RecType': 'Standard', 'RecEncoder': 'none' if role == 'sender' else 'obs_x264', 'RecAudioEncoder': 'ffmpeg_aac',
                    'RecFilePath': (output / role).as_posix(), 'RecFormat2': 'mkv', 'RecTracks': '1',
                    'RecUseRescale': 'false', 'RecSplitFile': 'false'}}
            for section, values in updates.items():
                if not config.has_section(section): config.add_section(section)
                for key, value in values.items(): config[section][key] = value
            with profile.open('w', encoding='utf-8') as handle: config.write(handle, space_around_delimiters=False)
            (profile.parent / 'service.json').write_text(json.dumps({'type': 'vdoninja_service',
                'settings': {'stream_id': 'codec-idle-' + uuid.uuid4().hex, 'password': 'false', 'room_id': ''}}))
            (profile.parent / 'streamEncoder.json').write_text(json.dumps({'rate_control': 'CBR', 'bitrate': 6000,
                'keyint_sec': 0, 'bf': 0, 'bframes': 0, 'preset': 'p4' if 'nvenc' in args.encoder else 'veryfast',
                'cpu_used': 8, 'profile': 'main' if args.codec != 'H264' else 'high', **json.loads(args.encoder_options)}))
            (profile.parent / 'recordEncoder.json').write_text(json.dumps({'rate_control': 'CRF', 'crf': 10, 'preset': 'ultrafast'}))
            with socket.socket() as sock: sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
            ws = review / 'config/obs-studio/plugin_config/obs-websocket/config.json'; ws.parent.mkdir(parents=True, exist_ok=True)
            ws.write_text(json.dumps({'server_enabled': True, 'server_port': port, 'auth_required': True,
                                     'server_password': password, 'first_load': False, 'alerts_enabled': False}))
            process = launch([review / 'bin/64bit/obs64.exe', '--portable', '--multi', '--minimize-to-tray',
                '--disable-updater', '--disable-missing-files-check', '--disable-shutdown-check'], role, cwd=review / 'bin/64bit')
            end = time.monotonic() + 60
            while True:
                assert process.poll() is None, f'{role} exited'
                try:
                    with socket.create_connection(('127.0.0.1', port), .2): break
                except OSError:
                    assert time.monotonic() < end, f'{role} startup timed out'
                    time.sleep(.2)
            modules = [m.path for m in psutil.Process(process.pid).memory_maps() if Path(m.path).name.lower() == 'obs-vdoninja.dll']
            assert len(modules) == 1 and Path(modules[0]).resolve() == target.resolve(), modules
            instances.append((process, review, port)); report[role + 'LoadedModule'] = modules[0]
        options = {'codec': args.codec, 'browserCheck': args.browser_check,
                   'browserOnly': args.browser_only,
                   'checkSharedColorGuard': args.check_shared_color_guard,
                   'width': width, 'height': height, 'fps': args.fps,
                   'ice': json.loads(args.browser_ice),
                   'expectProfile4': args.codec == 'HEVC' and args.color_format == 'I444' and args.preserve_color_format,
                   'service': {'preserve_source_color_format': args.preserve_color_format, **json.loads(args.service_options)}}
        (output / 'options.json').write_text(json.dumps(options))
        def sample_resources():
            processes = [(review.name, psutil.Process(process.pid)) for process, review, _ in instances]
            with (output / 'resources.jsonl').open('w') as samples:
                while not monitor_stop.wait(1):
                    for role, process in processes:
                        try:
                            samples.write(json.dumps({'time': time.time(), 'role': role, 'cpuOneCore': process.cpu_percent(),
                                'rss': process.memory_info().rss, 'logicalCpus': psutil.cpu_count()})+'\n')
                        except psutil.Error: pass
        monitor = threading.Thread(target=sample_resources, daemon=True); monitor.start()
        driver = launch(['node', Path(__file__).with_name('codec-native-check.cjs').resolve(),
            'ws://127.0.0.1:' + str(instances[0][2]), 'ws://127.0.0.1:' + str(instances[1][2]),
            'codec' + uuid.uuid4().hex, fixture, output], 'driver', env=dict(os.environ, OBS_WEBSOCKET_PASSWORD=password))
        assert driver.wait(timeout=600) == 0, 'OBS workflow failed; see driver.log'
        monitor_stop.set(); monitor.join()
        workflow = json.loads((output / 'workflow.json').read_text()); report['workflow'] = workflow
        report['colorChecks'] = []
        for filename in workflow['screenshots']:
            pixels = np.asarray(Image.open(filename).convert('RGB')).astype(int)
            errors = []
            for patch, rgb in enumerate(PATCHES):
                measured = np.median(pixels[20:80, patch*80+20:patch*80+60], axis=(0,1))
                errors.append(int(np.max(np.abs(measured - rgb))))
            levels = len(np.unique(pixels[140:160, 64:576, 1]))
            report['colorChecks'].append({'file': filename, 'patchErrors': errors, 'gradientLevels': levels})
            assert max(errors) <= 8, f'Color error {errors} in {filename}'
            assert levels >= 180, f'Gradient lost precision: {levels} levels'
            if args.color_format == 'I444' and args.preserve_color_format:
                chroma_errors = [int(np.max(np.abs(np.median(pixels[195:205, 64+i:576:2], axis=(0,1))-rgb)))
                                 for i, rgb in enumerate([PATCHES[0], PATCHES[5]])]
                report['colorChecks'][-1]['chromaStripeErrors'] = chroma_errors
                assert max(chroma_errors) <= 12, f'Fine chroma detail lost: {chroma_errors}'
        for phase in ([] if args.browser_only else ['initial', 'viewer-reconnected']):
            shots = [np.asarray(Image.open(f)) for f in workflow['screenshots'] if Path(f).name.startswith(phase)]
            assert any(np.mean(np.abs(shots[0][220:320].astype(float)-s[220:320])) > 5 for s in shots[1:]), 'Frozen received video'
        for recording in workflow['recordings']:
            decoded = subprocess.run([str(ffmpeg), '-v', 'error', '-i', recording, '-map', '0:a:0',
                '-f', 'f32le', '-ac', '1', '-'], capture_output=True, check=True)
            audio = np.frombuffer(decoded.stdout, dtype='<f4')
            assert len(audio) > 48000 * 4 and np.sqrt(np.mean(audio**2)) > .01, 'Silent or short received audio'
        # This recording shares the live streaming encoder, so its GOP and
        # compressed sizes are the publisher's actual stream, not a re-encode.
        probe_exe = ffmpeg.with_name('ffprobe.exe')
        recording = Path(workflow['senderRecording']).resolve()
        probe_command = [str(probe_exe)] if probe_exe.exists() else ['wsl', '--exec', 'ffprobe']
        probe_recording = str(recording) if probe_exe.exists() else '/mnt/'+recording.drive[0].lower()+recording.as_posix()[2:]
        probe = subprocess.run([*probe_command, '-v', 'error', '-select_streams', 'v:0',
            '-show_frames', '-show_streams', '-show_entries', 'frame=key_frame,pict_type,pkt_size,best_effort_timestamp_time:stream=codec_name,pix_fmt,color_range,color_space,width,height,avg_frame_rate',
            '-of', 'json', probe_recording], capture_output=True, check=True)
        encoded = json.loads(probe.stdout); frames = encoded['frames']
        keys = [i for i,f in enumerate(frames) if f['key_frame']]
        duration = float(frames[-1]['best_effort_timestamp_time'])-float(frames[0]['best_effort_timestamp_time'])
        report['encoded'] = {'stream': encoded['streams'][0], 'frames': len(frames), 'keyframes': len(keys),
            'keyframeFraction': len(keys)/len(frames), 'maxGopFrames': max([b-a for a,b in zip(keys,keys[1:])] or [0]),
            'bFrames': sum(f.get('pict_type') == 'B' for f in frames),
            'videoKbps': sum(int(f['pkt_size']) for f in frames)*8/duration/1000}
        assert len(frames) > 100 and report['encoded']['keyframeFraction'] < .15, 'Publisher became all-intra'
        packet_probe = subprocess.run([*probe_command, '-v', 'error', '-select_streams', 'v:0', '-show_packets',
            '-show_entries', 'packet=pts_time,dts_time', '-of', 'json', probe_recording], capture_output=True, check=True)
        packets = json.loads(packet_probe.stdout)['packets']
        # Intel HEVC GPB is labelled B but references only past pictures. Check
        # actual decode/presentation order instead of rejecting its slice label.
        report['encoded']['reorderedPackets'] = sum(p.get('pts_time') != p.get('dts_time') for p in packets)
        report['encoded']['nonforwardPts'] = sum(float(b['pts_time']) <= float(a['pts_time']) for a,b in zip(packets,packets[1:]))
        assert not report['encoded']['reorderedPackets'] and not report['encoded']['nonforwardPts'], 'Publisher reordered video'
        assert report['encoded']['maxGopFrames'] <= 2*args.fps+5, 'Recovery GOP exceeds two seconds'
        assert encoded['streams'][0]['width'] == width and encoded['streams'][0]['height'] == height
        target = json.loads(args.encoder_options).get('bitrate', 6000)
        if json.loads(args.encoder_options).get('rate_control', 'CBR') == 'CBR':
            assert report['encoded']['videoKbps'] < target * 1.5, 'Encoded bitrate exceeded CBR budget'
        if not args.preserve_color_format:
            assert report['encoded']['stream']['pix_fmt'] in ['yuv420p', 'yuvj420p'], 'Nonstandard chroma/depth without opt-in'
        for process, review, _ in instances:
            close_obs(process)
            latest = max((review / 'config/obs-studio/logs').glob('*.txt'), key=lambda p: p.stat().st_mtime)
            shutil.copy2(latest, output / (review.name + '-obs.log'))
        sender_log = (output / 'sender-obs.log').read_text(errors='replace')
        receiver_log = (output / 'receiver-obs.log').read_text(errors='replace')
        assert report['encoded']['stream']['codec_name'] == {'H264':'h264','HEVC':'hevc','AV1':'av1'}[args.codec]
        receive_name = 'H.264' if args.codec == 'H264' else args.codec
        if not args.browser_only:
            assert f'Native receiver selected {receive_name}' in receiver_log
        assert 'Could not packetize a complete video frame' not in sender_log
        if args.codec == 'HEVC' and args.color_format == 'I444' and args.preserve_color_format:
            assert 'profile-id=4;' in sender_log, 'HEVC encoder silently reduced I444 to 4:2:0'
        assert all(b.get('ok') or b.get('unsupported') for b in workflow.get('browsers', [])), 'Browser compatibility check failed; see browsers.json'
        report['ok'] = True
    except BaseException as error:
        report['error'] = str(error); raise
    finally:
        monitor_stop.set()
        if monitor: monitor.join(timeout=3)
        for process in children:
            if process.poll() is None:
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True)
                process.wait(timeout=15)
        for handle in handles: handle.close()
        (output / 'results.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__': main()
