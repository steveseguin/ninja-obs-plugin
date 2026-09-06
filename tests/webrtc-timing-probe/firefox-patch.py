"""Instrument the verified Firefox 146.0.1 release source with opt-in trials.

Uses the same timing probe sites as the Chromium diagnostic patch. Firefox does
not support Chromium's command-line field-trial switch: this diagnostic adapter
exposes only our explicitly named trials via environment variables.
"""
import importlib.util
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).resolve()
if (root / 'browser/config/version.txt').read_text().strip() != '146.0.1' or \
        '86bb7f6af6312ba3c0161085f854bcdff68f1a91' not in (root / 'sourcestamp.txt').read_text():
    raise RuntimeError('Expected Firefox 146.0.1 release source')
spec = importlib.util.spec_from_file_location('browser_patch', pathlib.Path(__file__).with_name('browser-patch.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
changes = module.build_changes(root / 'third_party/libwebrtc')
path = root / 'dom/media/webrtc/jsapi/PeerConnectionCtx.h'
text = path.read_text()
before = '  std::string Lookup(absl::string_view key) const override {'
if text.count(before) != 1 or '#include <map>' not in text:
    raise RuntimeError('Expected one Firefox field-trial adapter')
text = text.replace('#include <map>', '#include <map>\n#include <cstdlib>')
text = text.replace(before, before + '''
    const char* trace = std::getenv("VDONINJA_FIREFOX_TIMING_TRACE");
    const char* samples = std::getenv("VDONINJA_FIREFOX_TIMING_SAMPLES");
    if (key == "WebRTC-ReceiverTimingTrace" && trace && std::string(trace) == "1")
      return "Enabled";
    if (key == "WebRTC-RetransmittedTimingSamples" && samples && std::string(samples) == "all")
      return "Enabled";
''')
changes.append((path, text))
for path, text in changes:
    path.write_text(text)
print('Applied Firefox timing probe and explicit diagnostic-only environment trials')
