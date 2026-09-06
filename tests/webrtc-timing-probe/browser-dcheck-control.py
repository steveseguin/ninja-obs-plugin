"""Use production behavior for one unrelated diagnostic-browser DCHECK.

Non-official Chromium enables fatal DCHECKs even with is_debug=false. A remote
SCTP channel can trigger duplicate scheduler registration before media measurement.
Keep the original release code path and log the assertion condition instead of
aborting. This is an isolated browser-build control, not a sender/receiver fix.
"""
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
expected = '47e20adcc15fc15f01825aa17e570c8f5492ac0f'
revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
if revision != expected:
    raise RuntimeError('Expected Chromium 145.0.7632.6')
path = root/'third_party/blink/renderer/modules/peerconnection/rtc_data_channel.cc'
text = path.read_text()
before = '  DCHECK(!feature_handle_for_scheduler_);'
if text.count(before) != 1:
    raise RuntimeError('Expected exactly one data-channel scheduler assertion')
text = text.replace(before, '''  if (feature_handle_for_scheduler_) {
    LOG(ERROR) << "VDONINJA_DIAGNOSTIC data-channel scheduler DCHECK condition; "
                  "continuing the unmodified production code path";
  }''')
path.write_text(text)
print('Changed only the data-channel scheduler DCHECK to nonfatal diagnostic logging')
