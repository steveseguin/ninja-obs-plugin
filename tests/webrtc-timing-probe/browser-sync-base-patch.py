"""Test configured common buffer initialization in the pinned synchronizer.

Apply after browser-sync-patch.py. This opt-in experiment changes no configured
receiver target, synchronization threshold, or sender/encoder behavior.
Runtime testing rejected this as a general fix: dynamic, unequal receiver floors
can cause abrupt target reductions. Retain it only as an isolated comparison.
"""
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
if revision != 'b47e68e6966d5a5a0e4bc861ff364221600f31c3':
    raise RuntimeError('Expected the Chromium 145 pinned WebRTC checkout')
changes = []


def edit(relative, replacements):
    path = root / relative
    text = path.read_text()
    for before, after in replacements:
        if text.count(before) != 1:
            raise RuntimeError(f'Expected one unpatched site: {relative}: {before!r}')
        text = text.replace(before, after)
    changes.append((path, text))


edit('call/syncable.h', [
    ('    TimeDelta current_delay;', '''    TimeDelta current_delay;
    // Configured receiver floor, excluding delay added by synchronization.
    TimeDelta base_minimum_delay = TimeDelta::Zero();'''),
])
edit('audio/channel_receive.cc', [
    ('  int jitter_buffer_delay = neteq_->FilteredCurrentDelayMs();',
     '''  info.base_minimum_delay = TimeDelta::Millis(neteq_->GetBaseMinimumDelayMs());
  int jitter_buffer_delay = neteq_->FilteredCurrentDelayMs();'''),
])
edit('video/video_receive_stream2.cc', [
    ('  info->current_delay = timing_->TargetVideoDelay();',
     '''  info->base_minimum_delay =
      TimeDelta::Millis(std::max(0, GetBaseMinimumPlayoutDelayMs()));
  info->current_delay = timing_->TargetVideoDelay();'''),
])
edit('video/rtp_streams_synchronizer2.cc', [
    ('#include <cstdint>', '#include <algorithm>\n#include <cstdint>'),
    ('  int target_audio_delay_ms = 0;', '''  const int common_base_ms =
      std::max(0, std::min(audio_info->base_minimum_delay.ms<int>(),
                           video_info->base_minimum_delay.ms<int>()));
  if (env_.field_trials().IsEnabled("WebRTC-SyncConfiguredBase")) {
    sync_->SetTargetBufferingDelay(common_base_ms);
  }
  int target_audio_delay_ms = 0;'''),
    ('                     << " relative_ms=" << relative_delay_ms',
     '''                     << " audio_base_ms=" << audio_info->base_minimum_delay.ms()
                     << " video_base_ms=" << video_info->base_minimum_delay.ms()
                     << " common_base_ms=" << common_base_ms
                     << " relative_ms=" << relative_delay_ms'''),
])
for path, text in changes:
    path.write_text(text)
print('Applied opt-in configured common-buffer synchronization experiment')
