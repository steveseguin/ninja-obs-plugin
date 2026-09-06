"""Test decode scheduling headroom without changing render targets or queue caps.

Requires the pinned browser timing trace. This is an opt-in Linux 60 Hz control,
not a general display-refresh-aware implementation or a shipping plugin change.
"""
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
if subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() != \
        'b47e68e6966d5a5a0e4bc861ff364221600f31c3':
    raise RuntimeError('Expected the Chromium 145 pinned WebRTC checkout')
changes = []
for relative, replacements in [
    ('modules/video_coding/timing/timing.h', [
        ('  const bool trace_timing_;', '  const bool trace_timing_;\n  const TimeDelta decode_headroom_;'),
    ]),
    ('modules/video_coding/timing/timing.cc', [
        ('      trace_timing_(field_trials.IsEnabled("WebRTC-ReceiverTimingTrace")),',
         '''      trace_timing_(field_trials.IsEnabled("WebRTC-ReceiverTimingTrace")),
      decode_headroom_(field_trials.IsEnabled("WebRTC-DecodeSchedulingHeadroom")
                           ? TimeDelta::Millis(16) : TimeDelta::Zero()),'''),
        ('                    << " min_ms=" << min_playout_delay_.ms();',
         '''                    << " min_ms=" << min_playout_delay_.ms()
                    << " decode_ms=" << EstimatedMaxDecodeTime().ms()
                    << " render_delay_ms=" << render_delay_.ms()
                    << " current_delay_ms=" << current_delay_.ms()
                    << " headroom_ms=" << decode_headroom_.ms();'''),
        ('  return render_time - now - EstimatedMaxDecodeTime() - render_delay_;',
         '''  return render_time - now - EstimatedMaxDecodeTime() - render_delay_ -
         decode_headroom_;'''),
    ]),
]:
    path = root / relative
    text = path.read_text()
    for before, after in replacements:
        if text.count(before) != 1:
            raise RuntimeError(f'Expected one unpatched source site: {relative}: {before!r}')
        text = text.replace(before, after)
    changes.append((path, text))
for path, text in changes:
    path.write_text(text)
print('Applied opt-in 16 ms decode scheduling headroom and delay-state trace')
