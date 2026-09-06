"""Instrument pinned upstream VCMTiming; optionally test a bootstrap candidate.

The original checkout is never edited. This is a diagnostic experiment, not a
patch shipped in the OBS plugin or a replacement implementation of WebRTC.
"""
import pathlib
import sys

source, destination, mode = sys.argv[1:]
text = pathlib.Path(source).read_text()


def replace_once(before, after):
    global text
    if text.count(before) != 1:
        raise RuntimeError(f"Unexpected upstream source around {before!r}")
    text = text.replace(before, after)


replace_once("namespace webrtc {", "namespace webrtc {\nvoid ProbeTimingEvent(const char* event);")
replace_once("  ts_extrapolator_->Update(now, rtp_timestamp);",
             '  ProbeTimingEvent("incoming");\n  ts_extrapolator_->Update(now, rtp_timestamp);')
original = """  if (!local_time.has_value()) {
    return now;
  }
  Timestamp estimated_complete_time = *local_time;"""
if mode == "baseline":
    replacement = """  if (!local_time.has_value()) {
    ProbeTimingEvent("uninitialized");
    return now;
  }
  Timestamp estimated_complete_time = *local_time;"""
elif mode == "bootstrap":
    replacement = """  if (!local_time.has_value()) {
    ProbeTimingEvent("uninitialized");
    // Experimental one-time anchor. Subsequent retransmission-delayed frames
    // remain excluded from incoming network timing samples.
    ts_extrapolator_->Update(now, frame_timestamp);
    ProbeTimingEvent("bootstrap");
    local_time = ts_extrapolator_->ExtrapolateLocalTime(frame_timestamp);
    if (!local_time.has_value()) {
      return now;
    }
  }
  Timestamp estimated_complete_time = *local_time;"""
else:
    raise ValueError(mode)
replace_once(original, replacement)
pathlib.Path(destination).write_text(text)
