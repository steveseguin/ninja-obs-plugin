"""Create an isolated portable OBS for the room-return walkthrough.

Only configuration is copied; immutable runtime files are hard linked. The plugin
link is broken before installing the reviewed DLL. No existing OBS is touched.
"""
import configparser
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time
import uuid

import psutil

root = Path(__file__).resolve().parent.parent
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=False)
template = root / '_obs-portable'
review = output / 'obs'

def copy_file(source, destination):
    if Path(source).relative_to(template).parts[0] in ('config', 'data'):
        return shutil.copy2(source, destination)
    os.link(source, destination)
    return destination

shutil.copytree(template, review, copy_function=copy_file)
plugin = (Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else
          root / 'build-plugin-release-validation/RelWithDebInfo/obs-vdoninja.dll')
target = review / 'obs-plugins/64bit/obs-vdoninja.dll'
assert target.resolve().is_relative_to(output) and target != plugin
target.unlink()
shutil.copy2(plugin, target)
shutil.copytree(root / 'data', review / 'data/obs-plugins/obs-vdoninja', dirs_exist_ok=True)

def update_ini(path, updates):
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    config.read(path, encoding='utf-8-sig')
    for section, values in updates.items():
        if not config.has_section(section): config.add_section(section)
        for key, value in values.items(): config[section][key] = str(value)
    with path.open('w', encoding='utf-8') as handle:
        config.write(handle, space_around_delimiters=False)

profile = review / 'config/obs-studio/basic/profiles/Untitled/basic.ini'
update_ini(profile, {
    'Output': {'Mode': 'Advanced'},
    'Audio': {'SampleRate': '48000', 'ChannelSetup': 'Stereo'},
    'Video': {'BaseCX': 1280, 'BaseCY': 720, 'OutputCX': 1280, 'OutputCY': 720,
              'FPSType': 0, 'FPSCommon': 30, 'ColorFormat': 'NV12', 'ColorSpace': '709', 'ColorRange': 'Partial'},
    'AdvOut': {'Encoder': 'obs_x264', 'AudioEncoder': 'ffmpeg_aac', 'TrackIndex': 1,
               'RecType': 'Standard', 'RecEncoder': 'obs_x264', 'RecAudioEncoder': 'ffmpeg_aac',
               'RecFilePath': output.as_posix(), 'RecFormat2': 'mkv', 'RecTracks': 1},
    'VDONinjaGuestReturn': {'StreamID': '', 'Password': '', 'RoomID': '', 'AudioTrack': 2,
                           'VideoEncoder': 'obs_x264', 'VideoBitrate': 2500, 'MaxViewers': 10},
})
update_ini(review / 'config/obs-studio/user.ini', {'General': {'ConfirmOnExit': 'false'},
           'BasicWindow': {'SysTrayWhenStarted': 'false', 'PreviewProgramMode': 'false'}})
(profile.parent / 'streamEncoder.json').write_text(json.dumps({'rate_control': 'CBR', 'bitrate': 2500,
    'keyint_sec': 2, 'bf': 0, 'preset': 'veryfast'}))
(profile.parent / 'recordEncoder.json').write_text(json.dumps({'rate_control': 'CRF', 'crf': 18, 'preset': 'ultrafast'}))
(profile.parent / 'service.json').write_text(json.dumps({'type': 'rtmp_custom', 'settings': {
    'server': 'rtmp://127.0.0.1:19355/live', 'key': 'room-review', 'use_auth': False}}))
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
password = uuid.uuid4().hex
wsconfig = review / 'config/obs-studio/plugin_config/obs-websocket/config.json'
wsconfig.parent.mkdir(parents=True, exist_ok=True)
wsconfig.write_text(json.dumps({'server_enabled': True, 'server_port': port, 'auth_required': True,
    'server_password': password, 'first_load': False, 'alerts_enabled': False}))
log = (output / 'obs-process.log').open('w')
obs = subprocess.Popen([str(review / 'bin/64bit/obs64.exe'), '--portable', '--multi', '--disable-updater',
    '--disable-missing-files-check', '--disable-shutdown-check'], cwd=review / 'bin/64bit',
    stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
state = {'pid': obs.pid, 'websocket': 'ws://127.0.0.1:' + str(port), 'websocketPassword': password,
    'room': 'returnreview' + uuid.uuid4().hex[:10], 'password': 'RoomGuide2026', 'stream': 'ProgramReturn' + uuid.uuid4().hex[:8],
    'output': str(output), 'plugin': str(target), 'pluginSha256': hashlib.sha256(target.read_bytes()).hexdigest()}
(output / 'state.json').write_text(json.dumps(state, indent=2))
for _ in range(120):
    assert obs.poll() is None, 'OBS exited'
    try:
        with socket.create_connection(('127.0.0.1', port), .2): break
    except OSError: time.sleep(.5)
else: raise RuntimeError('OBS did not start')
modules = [m.path for m in psutil.Process(obs.pid).memory_maps() if Path(m.path).name.lower() == 'obs-vdoninja.dll']
assert len(modules) == 1 and Path(modules[0]).resolve() == target.resolve(), modules
state['loadedModule'] = modules[0]
(output / 'state.json').write_text(json.dumps(state, indent=2))
print(json.dumps({'pid': obs.pid, 'loadedModule': modules[0], 'output': str(output)}))
