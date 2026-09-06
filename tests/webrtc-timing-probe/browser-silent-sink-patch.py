"""Trace silent-output clock transitions in the pinned diagnostic Chromium build."""
import pathlib
import subprocess
import sys
root = pathlib.Path(sys.argv[1]).resolve()
if subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip() != '47e20adcc15fc15f01825aa17e570c8f5492ac0f':
    raise RuntimeError('Expected Chromium 145.0.7632.6')
path = root / 'media/base/silent_sink_suspender.cc'
text = path.read_text()
include = '#include "media/base/silent_sink_suspender.h"'
site = '  if (use_fake_sink) {\n    sink_->Pause();'
if text.count(include) != 1 or text.count(site) != 1 or 'VDONINJA_SILENT_SINK' in text:
    raise RuntimeError('Expected one unpatched silent-sink transition site')
text = text.replace(include, include+'\n#include "base/logging.h"\n#include "base/metrics/field_trial.h"')
text = text.replace(site, '''  if (base::FieldTrialList::FindFullName("WebRTC-AudioFifoTrace") == "Enabled") {
    LOG(ERROR) << "VDONINJA_SILENT_SINK object=" << this
               << " fake=" << use_fake_sink
               << " now_us=" << base::TimeTicks::Now().ToInternalValue();
  }
''' + site)
path.write_text(text)
print('Applied opt-in silent-sink transition logging')
