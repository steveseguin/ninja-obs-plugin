// Runs the pinned upstream frame buffer, controller and decode scheduler.
// Only the task runner, encoded input and decoder sink are synthetic.
#include <cstdlib>
#include <iostream>
#include <map>
#include <vector>

#include "api/rtp_packet_info.h"
#include "api/task_queue/task_queue_base.h"
#include "rtc_base/time_utils.h"
#ifndef PROBE_FIRST_PACKET
#define PROBE_FIRST_PACKET 0
#endif
#ifndef PROBE_ALL_ARRIVALS
#define PROBE_ALL_ARRIVALS 0
#endif
#include "video/task_queue_frame_decode_scheduler.h"
#include "video/video_stream_buffer_controller.h"

using namespace webrtc;
namespace
{
int incoming = 0, bootstraps = 0;
class Trials final : public FieldTrialsView
{
	std::string Lookup(absl::string_view) const override { return ""; }
};
class GlobalClock final : public ClockInterface
{
public:
	explicit GlobalClock(Clock &clock) : clock(clock), previous(SetClockForTesting(this)) {}
	~GlobalClock() override { SetClockForTesting(previous); }
	int64_t TimeNanos() const override { return clock.CurrentTime().us() * 1000; }

private:
	Clock &clock;
	ClockInterface *previous;
};
class Queue final : public TaskQueueBase
{
public:
	explicit Queue(SimulatedClock &clock) : clock(clock), current(this) {}
	void Delete() override { std::abort(); }
	void Until(Timestamp end)
	{
		while (!tasks.empty() && tasks.begin()->first.first <= end.us()) {
			auto node = tasks.extract(tasks.begin());
			clock.AdvanceTime(Timestamp::Micros(node.key().first) - clock.CurrentTime());
			std::move(node.mapped())();
		}
		clock.AdvanceTime(end - clock.CurrentTime());
	}

private:
	void PostTaskImpl(absl::AnyInvocable<void() &&> task, const PostTaskTraits &, const Location &) override
	{
		Add(std::move(task), TimeDelta::Zero());
	}
	void PostDelayedTaskImpl(absl::AnyInvocable<void() &&> task, TimeDelta delay, const PostDelayedTaskTraits &,
	                         const Location &) override
	{
		Add(std::move(task), delay);
	}
	void Add(absl::AnyInvocable<void() &&> task, TimeDelta delay)
	{
		tasks.emplace(std::make_pair((clock.CurrentTime() + delay).us(), serial++), std::move(task));
	}
	SimulatedClock &clock;
	CurrentTaskQueueSetter current;
	int serial = 0;
	std::map<std::pair<int64_t, int>, absl::AnyInvocable<void() &&>> tasks;
};
class Frame final : public EncodedFrame
{
public:
	int64_t received = 0;
	bool nack = true;
	int64_t ReceivedTime() const override { return received; }
	bool delayed_by_retransmission() const override { return nack; }
};
class Sink final : public FrameSchedulingReceiver, public VideoStreamBufferControllerStatsObserver
{
public:
	VideoStreamBufferController *controller = nullptr;
	Clock *clock = nullptr;
	std::vector<int64_t> renders, releases, leads;
	int dropped = 0, timeouts = 0;
	void OnEncodedFrame(std::unique_ptr<EncodedFrame> f) override
	{
		leads.push_back(f->RenderTime() - f->ReceivedTime());
		renders.push_back(f->RenderTime());
		releases.push_back(clock->CurrentTime().ms());
		controller->StartNextDecode(false);
	}
	void OnDecodableFrameTimeout(TimeDelta) override { ++timeouts; }
	void OnCompleteFrame(bool, size_t, VideoContentType) override {}
	void OnDroppedFrames(uint32_t n) override { dropped += n; }
	void OnDecodableFrame(TimeDelta, TimeDelta, TimeDelta) override {}
	void OnFrameBufferTimingsUpdated(int, int, int, int, int, int) override {}
	void OnTimingFrameInfoUpdated(const TimingFrameInfo &) override {}
};
} // namespace
namespace webrtc
{
void ProbeTimingEvent(const char *event)
{
	if (std::string(event) == "incoming")
		++incoming;
	if (std::string(event) == "bootstrap")
		++bootstraps;
}
} // namespace webrtc
int main()
{
	Trials trials;
	std::cout << "variant,case,released,dropped,incoming,bootstraps,max_render_step_ms,max_release_step_ms,last_render_"
	             "lead_ms,nonforward_render_times\n";
	for (const std::string scenario : {"cold", "warm", "cached-idr-2s", "cached-idr-12s", "outage-2s", "drift-1000ppm",
	                                   "drift-minus1000ppm", "clean-after-60", "cold-wrap", "completion-jitter-60",
	                                   "first-packet-missing-60", "first-packet-delayed-60"}) {
		SimulatedClock clock(Timestamp::Millis(1000));
		GlobalClock global(clock);
		Queue queue(clock);
		VCMTiming timing(&clock, trials);
		timing.set_min_playout_delay(TimeDelta::Millis(300));
		Sink sink;
		sink.clock = &clock;
		VideoStreamBufferController controller(&clock, &queue, &timing, &sink, &sink, TimeDelta::Seconds(20),
		                                       TimeDelta::Seconds(20),
		                                       std::make_unique<TaskQueueFrameDecodeScheduler>(&clock, &queue), trials);
		sink.controller = &controller;
		incoming = bootstraps = 0;
		controller.StartNextDecode(true);
		const int fps = (scenario == "completion-jitter-60" || scenario == "first-packet-missing-60" ||
		                 scenario == "first-packet-delayed-60")
		                    ? 60
		                    : 30;
		const int frames = scenario.find("drift-") == 0 ? 18000 : (fps == 60 ? 1200 : 300);
		for (int i = 0; i < frames; ++i) {
			int64_t elapsed = i * 1000 / fps + (i % 2 ? (fps == 60 ? 12 : 8) : 0);
			if (scenario == "outage-2s" && i >= 150)
				elapsed += 2000;
			queue.Until(Timestamp::Millis(1000 + elapsed));
			auto f = std::make_unique<Frame>();
			f->SetId(i);
			f->received = clock.CurrentTime().ms();
			f->nack = !((scenario == "warm" && i == 0) || (scenario == "clean-after-60" && i >= 60));
			uint32_t rtp = (scenario == "cold-wrap" ? 0xffff0000u : 90000u) + i * (90000 / fps);
			if (scenario == "cached-idr-2s" && i > 0)
				rtp += 180000;
			if (scenario == "cached-idr-12s" && i > 0)
				rtp += 1080000;
			if (scenario == "outage-2s" && i >= 150)
				rtp += 180000;
			if (scenario == "drift-1000ppm")
				rtp += i * 3;
			if (scenario == "drift-minus1000ppm")
				rtp -= i * 3;
			f->SetRtpTimestamp(rtp);
			if (scenario != "first-packet-missing-60") {
				const auto first =
				    scenario == "completion-jitter-60" ? Timestamp::Millis(1000 + i * 1000 / fps) : clock.CurrentTime();
				f->SetPacketInfos(RtpPacketInfos({RtpPacketInfo(1, {}, rtp, first)}));
			}
			f->SetEncodedData(EncodedImageBuffer::Create(1000));
			if (i > 0 && !(i == 1 && scenario.find("cached-idr-") == 0)) {
				f->num_references = 1;
				f->references[0] = i - 1;
			}
			controller.InsertFrame(std::move(f));
		}
		queue.Until(clock.CurrentTime() + TimeDelta::Seconds(15));
		controller.Stop();
		if (sink.renders.empty())
			return 7;
		int64_t render_step = 0, release_step = 0;
		int nonforward = 0;
		for (size_t i = 1; i < sink.renders.size(); ++i) {
			if (sink.renders[i] <= sink.renders[i - 1])
				++nonforward;
			render_step = std::max(render_step, sink.renders[i] - sink.renders[i - 1]);
			release_step = std::max(release_step, sink.releases[i] - sink.releases[i - 1]);
		}
		std::cout << (PROBE_FIRST_PACKET   ? "first-packet"
		              : PROBE_ALL_ARRIVALS ? "all-arrivals"
		              : PROBE_BOOTSTRAP    ? "bootstrap"
		                                   : "baseline")
		          << ',' << scenario << ',' << sink.renders.size() << ',' << sink.dropped << ',' << incoming << ','
		          << bootstraps << ',' << render_step << ',' << release_step << ',' << sink.leads.back() << ','
		          << nonforward << '\n';
		if (scenario == "cold" || scenario == "warm") {
			if (sink.renders.size() != static_cast<size_t>(frames) || incoming != (PROBE_ALL_ARRIVALS   ? frames
			                                                                       : scenario == "warm" ? 1
			                                                                                            : 0))
				return 1;
			if (!PROBE_ALL_ARRIVALS && (PROBE_BOOTSTRAP || scenario == "warm") && render_step > 34)
				return 2;
			if (!PROBE_BOOTSTRAP && !PROBE_ALL_ARRIVALS && scenario == "cold" && render_step < 40)
				return 3;
		}
		if (PROBE_ALL_ARRIVALS && scenario.find("cached-idr-") == 0 && release_step > 400)
			return 8;
		if (PROBE_ALL_ARRIVALS && scenario.find("drift-") == 0 && (sink.leads.back() < 270 || sink.leads.back() > 320))
			return 9;
		if (fps == 60 && PROBE_ALL_ARRIVALS && incoming != frames)
			return 11;
		if (PROBE_FIRST_PACKET && scenario == "completion-jitter-60" && (render_step > 18 || nonforward != 0))
			return 12;
		if (scenario == "cold-wrap" && incoming != (PROBE_ALL_ARRIVALS ? frames : 0))
			return 10;
		// Keep the unsafe candidate's observed regression explicit. Passing this
		// characterization does not mean that candidate is suitable for a browser.
		if (PROBE_BOOTSTRAP && scenario == "cached-idr-2s" && release_step < 2000)
			return 4;
		if (PROBE_BOOTSTRAP && scenario == "cached-idr-12s" && release_step < 12000)
			return 5;
		if (PROBE_BOOTSTRAP && scenario == "drift-1000ppm" && sink.leads.back() < 850)
			return 6;
	}
}
