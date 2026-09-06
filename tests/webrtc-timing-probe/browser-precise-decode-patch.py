"""Opt-in precise receiver decode scheduling, with matching diagnostic traces.

Apply after browser-scheduler-trace-patch.py. WebRTC-PreciseDecodeScheduling
selects the existing high-precision task-queue scheduler instead of coalescing
with the metronome. Render targets, receiver buffers and sender pacing are intact.
This is an experiment, not a shipping plugin fix.
"""
from pathlib import Path
import subprocess
import sys
root = Path(sys.argv[1]).resolve()
if subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip() != 'b47e68e6966d5a5a0e4bc861ff364221600f31c3':
    raise RuntimeError('Expected pinned Chromium 145 WebRTC')
pending = {}
def patch(name, replacements):
    p = root / name
    s = p.read_text()
    marker = 'WebRTC-PreciseDecodeScheduling' if name.endswith('video_receive_stream2.cc') else 'VDONINJA_SCHED'
    if marker in s:
        raise RuntimeError(f'{name}: experiment already applied')
    for before, after in replacements:
        if s.count(before) != 1:
            raise RuntimeError(f'{name}: expected one unpatched site {before!r}')
        s = s.replace(before, after)
    pending[p] = s
patch('video/video_receive_stream2.cc', [
    ('  if (env_.field_trials().IsEnabled("WebRTC-ReceiverTimingTrace")) {\n    RTC_LOG(LS_ERROR) << "VDONINJA_SCHED', '''  const bool use_decode_metronome = decode_sync &&
      !env_.field_trials().IsEnabled("WebRTC-PreciseDecodeScheduling");
  if (env_.field_trials().IsEnabled("WebRTC-ReceiverTimingTrace")) {
    RTC_LOG(LS_ERROR) << "VDONINJA_SCHED'''),
    ('<< " metronome=" << (decode_sync != nullptr);', '<< " metronome=" << use_decode_metronome;'),
    ('      decode_sync ? decode_sync->CreateSynchronizedFrameScheduler()', '      use_decode_metronome ? decode_sync->CreateSynchronizedFrameScheduler()'),
])
patch('video/task_queue_frame_decode_scheduler.cc', [
    ('#include <algorithm>', '#include <algorithm>\n#include <cstdlib>'),
    ('#include "rtc_base/checks.h"', '#include "rtc_base/checks.h"\n#include "rtc_base/logging.h"'),
    ('namespace webrtc {', '''namespace webrtc {
namespace {
bool VdoninjaPreciseSchedulerTrace() {
  static const bool enabled = [] {
    const char* value = std::getenv("VDONINJA_DECODE_SCHEDULER_TRACE");
    return value && value[0] == '1';
  }();
  return enabled;
}
}  // namespace'''),
    ('  scheduled_rtp_ = rtp;', '''  if (VdoninjaPreciseSchedulerTrace()) {
    RTC_LOG(LS_ERROR) << "VDONINJA_SCHED event=schedule scheduler=" << this
                     << " rtp=" << rtp << " now_us=" << clock_->CurrentTime().us()
                     << " deadline_us=" << schedule.latest_decode_time.us()
                     << " render_us=" << schedule.render_time.us();
  }
  scheduled_rtp_ = rtp;'''),
    ('                 scheduled_rtp_ = std::nullopt;', '''                 if (VdoninjaPreciseSchedulerTrace()) {
                   RTC_LOG(LS_ERROR) << "VDONINJA_SCHED event=release scheduler=" << this
                                    << " rtp=" << rtp << " now_us=" << clock_->CurrentTime().us()
                                    << " deadline_us=" << schedule.latest_decode_time.us();
                 }
                 scheduled_rtp_ = std::nullopt;'''),
])
for p,s in pending.items():
    p.write_text(s)
print('Applied opt-in precise decode scheduling experiment')
