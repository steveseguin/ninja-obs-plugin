"""Test source-clock cadence samples without changing renderer drift thresholds."""
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
if subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() != '47e20adcc15fc15f01825aa17e570c8f5492ac0f':
    raise RuntimeError('Expected Chromium 145.0.7632.6')
path = root / 'media/filters/video_renderer_algorithm.cc'
text = path.read_text()
site = '    if (new_sample)\n      frame_duration_calculator_.AddSample(frame.end_time - frame.start_time);'
if text.count(site) != 1:
    raise RuntimeError('Expected one frame duration sample site')
text = text.replace('#include "media/filters/video_renderer_algorithm.h"',
                    '#include "media/filters/video_renderer_algorithm.h"\n#include "base/metrics/field_trial.h"')
text = text.replace(site, '''    if (new_sample) {
      base::TimeDelta duration = frame.end_time - frame.start_time;
      const auto& next = frame_queue_[i + 1];
      if (base::FieldTrialList::FindFullName("WebRTC-RtpCadenceSamples") == "Enabled" &&
          frame.frame->metadata().rtp_timestamp && next.frame->metadata().rtp_timestamp) {
        const uint32_t ticks = static_cast<uint32_t>(*next.frame->metadata().rtp_timestamp) -
                               static_cast<uint32_t>(*frame.frame->metadata().rtp_timestamp);
        if (ticks > 0 && ticks < 90000)
          duration = base::Microseconds(static_cast<int64_t>(ticks) * 1000000 / 90000);
      }
      frame_duration_calculator_.AddSample(duration);
    }''')
path.write_text(text)
print('Applied opt-in RTP source-clock cadence experiment; drift thresholds unchanged')
