"""Extend the pinned, already instrumented diagnostic browser; never shipping code."""
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
if subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() != '47e20adcc15fc15f01825aa17e570c8f5492ac0f':
    raise RuntimeError('Expected Chromium 145.0.7632.6')
changes = []
def edit(relative, replacements):
    path = root / relative
    text = path.read_text()
    for before, after in replacements:
        if text.count(before) != 1:
            raise RuntimeError(f'Expected one source site: {relative}: {before!r}')
        text = text.replace(before, after)
    changes.append((path, text))

edit('third_party/webrtc/video/video_stream_buffer_controller.cc', [
    ('struct FrameMetadata {', 'Timestamp MinReceiveTime(const EncodedFrame& frame);\n\nstruct FrameMetadata {'),
    ('receive_time(frame.ReceivedTimestamp()) {}',
     'receive_time(frame.ReceivedTimestamp()),\n        first_receive_time(MinReceiveTime(frame)) {}'),
    ('  const std::optional<Timestamp> receive_time;',
     '  const std::optional<Timestamp> receive_time;\n  const Timestamp first_receive_time;'),
    ('      timing_->IncomingTimestamp(metadata.rtp_timestamp,\n                                 *metadata.receive_time);',
     '''      const Timestamp sample =
          field_trials_.IsEnabled("WebRTC-FirstPacketTimingSamples") &&
                  metadata.first_receive_time.IsFinite()
              ? metadata.first_receive_time : *metadata.receive_time;
      timing_->IncomingTimestamp(metadata.rtp_timestamp, sample);'''),
    ('  receiver_->OnEncodedFrame(std::move(frame));',
     '''  if (field_trials_.IsEnabled("WebRTC-ReceiverTimingTrace")) {
    RTC_LOG(LS_ERROR) << "VDONINJA_TIMING controller=" << this << " timing=" << timing_
                      << " event=release rtp=" << frame->RtpTimestamp()
                      << " now_ms=" << now.ms() << " render_ms=" << frame->RenderTime()
                      << " receive_ms=" << frame->ReceivedTime();
  }
  receiver_->OnEncodedFrame(std::move(frame));'''),
])
edit('third_party/blink/renderer/modules/mediastream/web_media_player_ms_compositor.cc', [
    ('#include "base/feature_list.h"', '#include "base/feature_list.h"\n#include "base/metrics/field_trial.h"'),
    ('  RecordFrameDecodedStats(frame->metadata().receive_time,',
     '''  if (base::FieldTrialList::FindFullName("WebRTC-CompositorTimingTrace") == "Enabled") {
    LOG(ERROR) << "VDONINJA_COMPOSITOR object=" << this << " event=enqueue rtp="
               << enqueue_frame_rtp_timestamp.value_or(0)
               << " now_us=" << base::TimeTicks::Now().ToInternalValue()
               << " reference_us=" << frame->metadata().reference_time.value_or(base::TimeTicks()).ToInternalValue()
               << " media_us=" << frame->timestamp().InMicroseconds();
  }
  RecordFrameDecodedStats(frame->metadata().receive_time,'''),
    ('  dropped_frame_count_ += frames_dropped;',
     '''  dropped_frame_count_ += frames_dropped;
  if (base::FieldTrialList::FindFullName("WebRTC-CompositorTimingTrace") == "Enabled") {
    LOG(ERROR) << "VDONINJA_COMPOSITOR object=" << this << " event=select rtp="
               << (frame ? static_cast<uint32_t>(frame->metadata().rtp_timestamp.value_or(0)) : 0)
               << " deadline_us=" << deadline_min.ToInternalValue()
               << " deadline_max_us=" << deadline_max.ToInternalValue()
               << " dropped=" << frames_dropped
               << " repeated=" << (frame && frame == current_frame_);
  }'''),
])
for path, text in changes:
    path.write_text(text)
print('Applied release/compositor traces and optional first-packet sample experiment')
