"""Trace pinned Chromium decode metronome and A/V-sync early returns.

Set VDONINJA_DECODE_SCHEDULER_TRACE=1 for scheduler events. A/V early returns
use the existing WebRTC-AvSyncTrace trial. No scheduling behavior is changed.
Apply after browser-sync-base-patch.py; rebuild before capturing.
"""
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
if subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() != 'b47e68e6966d5a5a0e4bc861ff364221600f31c3':
    raise RuntimeError('Expected pinned Chromium 145 WebRTC')
pending = {}
def patch(name, replacements):
    p = root / name
    text = p.read_text()
    marker = {'video/decode_synchronizer.cc': 'VDONINJA_SCHED',
              'video/video_receive_stream2.cc': 'VDONINJA_SCHED',
              'video/rtp_streams_synchronizer2.cc': 'VDONINJA_SYNC_STAGE',
              'video/stream_synchronization.cc': 'VDONINJA_SYNC_CLOCK'}[name]
    if marker in text:
        raise RuntimeError(f'{name}: trace already applied')
    for before, after in replacements:
        if text.count(before) != 1:
            raise RuntimeError(f'{name}: expected one unpatched site {before!r}')
        text = text.replace(before, after)
    pending[p] = text

patch('video/decode_synchronizer.cc', [
    ('#include <optional>', '#include <optional>\n#include <cstdlib>'),
    ('namespace webrtc {', '''namespace webrtc {
namespace {
bool VdoninjaSchedulerTrace() {
  static const bool enabled = [] {
    const char* value = std::getenv("VDONINJA_DECODE_SCHEDULER_TRACE");
    return value && value[0] == '1';
  }();
  return enabled;
}
}  // namespace'''),
    ('  auto res = std::move(*next_frame_);', '''  if (VdoninjaSchedulerTrace()) {
    RTC_LOG(LS_ERROR) << "VDONINJA_SCHED event=release scheduler=" << this
                     << " rtp=" << next_frame_->rtp_timestamp()
                     << " now_us=" << sync_->clock_->CurrentTime().us()
                     << " deadline_us=" << next_frame_->LatestDecodeTime().us();
  }
  auto res = std::move(*next_frame_);'''),
    ('  next_frame_ = ScheduledFrame(rtp, std::move(schedule), std::move(cb));', '''  if (VdoninjaSchedulerTrace()) {
    RTC_LOG(LS_ERROR) << "VDONINJA_SCHED event=schedule scheduler=" << this
                     << " rtp=" << rtp << " now_us=" << sync_->clock_->CurrentTime().us()
                     << " deadline_us=" << schedule.latest_decode_time.us()
                     << " render_us=" << schedule.render_time.us();
  }
  next_frame_ = ScheduledFrame(rtp, std::move(schedule), std::move(cb));'''),
    ('  if (decode_before_next_tick || decode_time_in_past) {', '''  if (VdoninjaSchedulerTrace()) {
    RTC_LOG(LS_ERROR) << "VDONINJA_SCHED event=decision scheduler=" << scheduler
                     << " rtp=" << *scheduler->ScheduledRtpTimestamp()
                     << " now_us=" << now.us() << " next_tick_us=" << next_tick.us()
                     << " period_us=" << metronome_->TickPeriod().us()
                     << " deadline_us=" << scheduler->LatestDecodeTime().us()
                     << " immediate=" << (decode_before_next_tick || decode_time_in_past);
  }
  if (decode_before_next_tick || decode_time_in_past) {'''),
    ('  for (auto* scheduler : schedulers_) {', '''  for (auto* scheduler : schedulers_) {
    if (VdoninjaSchedulerTrace() && scheduler->ScheduledRtpTimestamp()) {
      RTC_LOG(LS_ERROR) << "VDONINJA_SCHED event=tick scheduler=" << scheduler
                       << " rtp=" << *scheduler->ScheduledRtpTimestamp()
                       << " now_us=" << clock_->CurrentTime().us()
                       << " next_tick_us=" << expected_next_tick_.us()
                       << " deadline_us=" << scheduler->LatestDecodeTime().us()
                       << " release=" << (scheduler->LatestDecodeTime() < expected_next_tick_);
    }'''),
])
patch('video/video_receive_stream2.cc', [
    ('  std::unique_ptr<FrameDecodeScheduler> scheduler =', '''  if (env_.field_trials().IsEnabled("WebRTC-ReceiverTimingTrace")) {
    RTC_LOG(LS_ERROR) << "VDONINJA_SCHED event=select ssrc=" << config_.rtp.remote_ssrc
                     << " metronome=" << (decode_sync != nullptr);
  }
  std::unique_ptr<FrameDecodeScheduler> scheduler ='''),
])
patch('video/rtp_streams_synchronizer2.cc', [
    ('  syncable_audio_ = syncable_audio;', '''  if (env_.field_trials().IsEnabled("WebRTC-AvSyncTrace")) {
    RTC_LOG(LS_ERROR) << "VDONINJA_SYNC_STAGE event=configure object=" << this
                     << " audio=" << (syncable_audio != nullptr);
  }
  syncable_audio_ = syncable_audio;'''),
    ('  if (!syncable_audio_)\n    return;', '''  const auto trace_stage = [this](const char* stage) {
    if (env_.field_trials().IsEnabled("WebRTC-AvSyncTrace")) {
      RTC_LOG(LS_ERROR) << "VDONINJA_SYNC_STAGE event=" << stage << " object=" << this
                       << " now_ms=" << env_.clock().TimeInMilliseconds();
    }
  };
  if (!syncable_audio_) {
    trace_stage("no_audio");
    return;
  }'''),
    ('  if (!audio_info || !UpdateMeasurements(&audio_measurement_, *audio_info)) {\n    return;', '  if (!audio_info || !UpdateMeasurements(&audio_measurement_, *audio_info)) {\n    trace_stage(audio_info ? "audio_measurement_invalid" : "audio_info_missing");\n    return;'),
    ('    // No new audio packet has been received since last update.', '    trace_stage("audio_unchanged");\n    // No new audio packet has been received since last update.'),
    ('  if (!video_info || !UpdateMeasurements(&video_measurement_, *video_info)) {\n    return;', '  if (!video_info || !UpdateMeasurements(&video_measurement_, *video_info)) {\n    trace_stage(video_info ? "video_measurement_invalid" : "video_info_missing");\n    return;'),
    ('    // No new video packet has been received since last update.', '    trace_stage("video_unchanged");\n    // No new video packet has been received since last update.'),
    ('          audio_measurement_, video_measurement_, &relative_delay_ms)) {\n    return;', '          audio_measurement_, video_measurement_, &relative_delay_ms)) {\n    trace_stage("relative_delay_invalid");\n    return;'),
])
patch('video/stream_synchronization.cc', [
    ('#include <algorithm>', '#include <algorithm>\n#include <cstdlib>'),
    ('  if (!audio_last_capture_time.Valid()) {', '''  if (!audio_last_capture_time.Valid()) {
    if (std::getenv("VDONINJA_DECODE_SCHEDULER_TRACE"))
      RTC_LOG(LS_ERROR) << "VDONINJA_SYNC_CLOCK event=audio_estimate_invalid";'''),
    ('  if (!video_last_capture_time.Valid()) {', '''  if (!video_last_capture_time.Valid()) {
    if (std::getenv("VDONINJA_DECODE_SCHEDULER_TRACE"))
      RTC_LOG(LS_ERROR) << "VDONINJA_SYNC_CLOCK event=video_estimate_invalid";'''),
    ('  if (*relative_delay_ms > kMaxDeltaDelayMs ||', '''  if (std::getenv("VDONINJA_DECODE_SCHEDULER_TRACE")) {
    RTC_LOG(LS_ERROR) << "VDONINJA_SYNC_CLOCK event=relative relative_ms=" << *relative_delay_ms
                     << " audio_capture_ms=" << audio_last_capture_time_ms
                     << " video_capture_ms=" << video_last_capture_time_ms
                     << " audio_receive_ms=" << audio_measurement.latest_receive_time_ms
                     << " video_receive_ms=" << video_measurement.latest_receive_time_ms;
  }
  if (*relative_delay_ms > kMaxDeltaDelayMs ||'''),
])
for p, text in pending.items():
    p.write_text(text)
print('Applied scheduler and synchronization-stage traces')
