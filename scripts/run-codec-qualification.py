"""Finite codec/options matrix. Each case runs real isolated portable OBS.

Keep cases sequential so their CPU/GPU measurements do not compete. Raw failed
runs remain in place; no unsupported browser is counted as a playback pass.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--ffmpeg', required=True)
p.add_argument('--only', default='')
p.add_argument('--browser-ice', default='null')
a = p.parse_args()
cases = [
    ('h264-1080p60-stress', 'obs_nvenc_h264_tex', 'H264', ['--size','1920x1080','--fps','60','--stress'], {}, {'bitrate':12000}),
    ('av1-720p60-stress', 'av1_texture_amf', 'AV1', ['--size','1280x720','--fps','60','--stress','--color-space','601'], {}, {'bitrate':8000}),
    ('hevc-default', 'obs_nvenc_hevc_tex', 'HEVC', [], {}, {}),
    ('av1-default', 'av1_texture_amf', 'AV1', [], {}, {}),
    ('av1-601-low-red', 'av1_texture_amf', 'AV1', ['--color-space','601'], {'video_protection_mode':1,'audio_red':True}, {}),
    ('hevc-444-default-off', 'obs_nvenc_hevc_tex', 'HEVC', ['--color-format','I444'], {}, {}),
    ('hevc-444-opt-in', 'obs_nvenc_hevc_tex', 'HEVC', ['--color-format','I444','--preserve-color-format','--check-shared-color-guard'], {}, {}),
    ('hevc-10bit-opt-in', 'obs_nvenc_hevc_tex', 'HEVC', ['--color-format','P010','--preserve-color-format'], {}, {}),
    ('av1-low-red', 'av1_texture_amf', 'AV1', [], {'video_protection_mode':1,'audio_red':True}, {}),
    ('hevc-medium-adaptive', 'obs_qsv11_hevc', 'HEVC', [], {'video_protection_mode':2,'adaptive_bitrate':True}, {}),
    ('h264-high-red-adaptive', 'obs_nvenc_h264_tex', 'H264', [], {'video_protection_mode':3,'audio_red':True,'adaptive_bitrate':True}, {}),
    ('h264-quality-adaptive', 'obs_nvenc_h264_tex', 'H264', [], {'adaptive_bitrate':True}, {'rate_control':'CQP','cqp':20}),
    ('hevc-601-full', 'obs_nvenc_hevc_tex', 'HEVC', ['--color-space','601','--color-range','Full'], {}, {}),
    ('av1-loss', 'av1_texture_amf', 'AV1', ['--color-space','601','--browser-ice',a.browser_ice], {}, {}),
    ('h264-loss', 'obs_nvenc_h264_tex', 'H264', ['--browser-ice',a.browser_ice], {}, {}),
]
a.output.mkdir(parents=True, exist_ok=True)
results = []
for name, encoder, codec, flags, service, encoder_options in cases:
    if a.only and name not in a.only.split(','): continue
    if name.endswith('-loss') and a.browser_ice == 'null': continue
    target = a.output/name
    assert not target.exists(), 'Use a new output directory; previous evidence must not be overwritten'
    command = [sys.executable, str(Path(__file__).with_name('run-codec-native-e2e.py')),
        '--encoder',encoder,'--codec',codec,'--browser-check','--ffmpeg',a.ffmpeg,'--output',str(target),
        '--service-options',json.dumps(service),'--encoder-options',json.dumps(encoder_options),*flags]
    print('Starting '+name, flush=True)
    result = subprocess.run(command)
    results.append({'case':name,'exit':result.returncode,'report':str(target/'results.json')})
    (a.output/'matrix.json').write_text(json.dumps(results,indent=2))
    print(name+' exit='+str(result.returncode), flush=True)
sys.exit(int(any(r['exit'] for r in results)))
