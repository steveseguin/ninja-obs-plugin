"""Patch only a pinned diagnostic Chromium checkout; never an installed browser.

One binary compares default sample exclusion with an opt-in all-arrivals trial.
The one-time bootstrap candidate is deliberately not included: controller tests
show long stalls and uncorrected drift. Logging is diagnostic and changes load.
"""
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
expected = 'b47e68e6966d5a5a0e4bc861ff364221600f31c3'
revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
if revision != expected:
    raise RuntimeError('Expected the Chromium 145 pinned WebRTC checkout')


def edit(relative, replacements):
    path = root / relative
    text = path.read_text()
    for before, after in replacements:
        if text.count(before) != 1:
            raise RuntimeError(f'Expected exactly one source site in {relative}: {before!r}')
        text = text.replace(before, after)
    return path, text


changes = [edit('video/video_stream_buffer_controller.cc', [
    ('    if (!metadata.delayed_by_retransmission && metadata.receive_time &&',
     '''    if (field_trials_.IsEnabled("WebRTC-ReceiverTimingTrace")) {
      RTC_LOG(LS_ERROR) << "VDONINJA_TIMING controller=" << this << " timing=" << timing_
                        << " event=insert rtp=" << metadata.rtp_timestamp
                        << " nack=" << metadata.delayed_by_retransmission;
    }
    if ((!metadata.delayed_by_retransmission ||
         field_trials_.IsEnabled("WebRTC-RetransmittedTimingSamples")) &&
        (!field_trials_.IsEnabled("WebRTC-ForceTimingSampleExclusion") ||
         field_trials_.IsEnabled("WebRTC-RetransmittedTimingSamples")) && metadata.receive_time &&'''),
 ]), edit('modules/video_coding/timing/timing.h', [
    ('  Clock* const clock_;', '  Clock* const clock_;\n  const bool trace_timing_;'),
]), edit('modules/video_coding/timing/timing.cc', [
    ('    : clock_(clock),', '    : clock_(clock),\n      trace_timing_(field_trials.IsEnabled("WebRTC-ReceiverTimingTrace")),') ,
    ('  ts_extrapolator_->Update(now, rtp_timestamp);',
     '''  if (trace_timing_) {
    RTC_LOG(LS_ERROR) << "VDONINJA_TIMING timing=" << this
                      << " event=incoming rtp=" << rtp_timestamp << " now_ms=" << now.ms();
  }
  ts_extrapolator_->Update(now, rtp_timestamp);'''),
    ('''  if (!local_time.has_value()) {
    return now;
  }''', '''  if (!local_time.has_value()) {
    if (trace_timing_) {
      RTC_LOG(LS_ERROR) << "VDONINJA_TIMING timing=" << this
                      << " event=uninitialized rtp=" << frame_timestamp
                      << " now_ms=" << now.ms() << " min_ms=" << min_playout_delay_.ms();
    }
    return now;
  }'''),
    ('  return estimated_complete_time + actual_delay;',
     '''  if (trace_timing_) {
    RTC_LOG(LS_ERROR) << "VDONINJA_TIMING timing=" << this
                    << " event=render rtp=" << frame_timestamp << " now_ms=" << now.ms()
                    << " render_ms=" << (estimated_complete_time + actual_delay).ms()
                    << " min_ms=" << min_playout_delay_.ms();
  }
  return estimated_complete_time + actual_delay;'''),
])]
# Validate every source site before mutating any file.
for path, text in changes:
    path.write_text(text)
print('Applied diagnostic logging and opt-in WebRTC-RetransmittedTimingSamples/Enabled/')
