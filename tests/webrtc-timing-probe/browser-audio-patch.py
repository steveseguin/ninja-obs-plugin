"""Log Web Audio FIFO underruns and overruns without changing its buffering or output."""
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
if subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() != '47e20adcc15fc15f01825aa17e570c8f5492ac0f':
    raise RuntimeError('Expected Chromium 145.0.7632.6')
path = root / 'third_party/blink/renderer/modules/mediastream/webaudio_media_stream_audio_sink.cc'
text = path.read_text()
include = '#include "third_party/blink/renderer/modules/mediastream/webaudio_media_stream_audio_sink.h"'
site = '    audio_bus->Zero();\n    if (fifo_stats_) {'
if text.count(site) != 1 or text.count(include) != 1:
    raise RuntimeError('Expected one Web Audio underrun site')
text = text.replace(include, include + '\n#include "base/metrics/field_trial.h"')
text = text.replace(site, '''    if (base::FieldTrialList::FindFullName("WebRTC-AudioFifoTrace") == "Enabled") {
      LOG(ERROR) << "VDONINJA_AUDIO object=" << this << " event=underrun"
                 << " now_us=" << base::TimeTicks::Now().ToInternalValue()
                 << " available=" << fifo_->frames() << " requested=" << audio_bus->frames()
                 << " sample_rate=" << source_params_.sample_rate();
    }
''' + site)
overrun = '    if (fifo_stats_) {\n      fifo_stats_->overruns++;'
if text.count(overrun) != 1:
    raise RuntimeError('Expected one Web Audio overrun site')
text = text.replace(overrun, '''    if (base::FieldTrialList::FindFullName("WebRTC-AudioFifoTrace") == "Enabled") {
      LOG(ERROR) << "VDONINJA_AUDIO object=" << this << " event=overrun"
                 << " now_us=" << base::TimeTicks::Now().ToInternalValue()
                 << " available=" << fifo_->max_frames() - fifo_->frames()
                 << " requested=" << audio_bus.frames()
                 << " sample_rate=" << source_params_.sample_rate();
    }
''' + overrun)
path.write_text(text)
print('Applied opt-in Web Audio FIFO underrun trace')
