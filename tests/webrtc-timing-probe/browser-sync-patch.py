"""Trace native A/V synchronization inputs and decisions without changing delays."""
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
if revision != 'b47e68e6966d5a5a0e4bc861ff364221600f31c3':
    raise RuntimeError('Expected the Chromium 145 pinned WebRTC checkout')
path = root / 'video/rtp_streams_synchronizer2.cc'
text = path.read_text()
before = '''  if (!sync_->ComputeDelays(relative_delay_ms, audio_info->current_delay.ms(),
                            &target_audio_delay_ms, &target_video_delay_ms)) {
    return;
  }'''
after = '''  const bool adjusted = sync_->ComputeDelays(
      relative_delay_ms, audio_info->current_delay.ms(),
      &target_audio_delay_ms, &target_video_delay_ms);
  if (env_.field_trials().IsEnabled("WebRTC-AvSyncTrace")) {
    RTC_LOG(LS_ERROR) << "VDONINJA_SYNC object=" << this << " now_ms=" << now_ms
                     << " audio_ssrc=" << sync_->audio_stream_id()
                     << " video_ssrc=" << sync_->video_stream_id()
                     << " relative_ms=" << relative_delay_ms
                     << " current_audio_ms=" << audio_info->current_delay.ms()
                     << " current_video_ms=" << video_info->current_delay.ms()
                     << " target_audio_ms=" << target_audio_delay_ms
                     << " target_video_ms=" << target_video_delay_ms
                     << " adjusted=" << adjusted;
  }
  if (!adjusted) {
    return;
  }'''
include = '#include "api/environment/environment.h"'
if text.count(before) != 1 or text.count(include) != 1:
    raise RuntimeError('Expected exactly one unpatched synchronizer site')
text = text.replace(include, include + '\n#include "api/field_trials_view.h"').replace(before, after)
path.write_text(text)
print('Applied opt-in native A/V synchronization trace')
