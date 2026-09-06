// Actual upstream decode schedulers; synthetic metronome and task queue.
// A passing metronome case characterizes coalescing, not media continuity.
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "api/metronome/metronome.h"
#include "api/task_queue/task_queue_base.h"
#include "rtc_base/time_utils.h"
#include "system_wrappers/include/clock.h"
#include "video/decode_synchronizer.h"
#include "video/task_queue_frame_decode_scheduler.h"
using namespace webrtc;
namespace
{
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

class Ticks final : public Metronome
{
public:
	Ticks(Clock &clock, TaskQueueBase &queue) : clock(clock), queue(queue) {}
	TimeDelta TickPeriod() const override { return TimeDelta::Micros(15625); }
	void RequestCallOnNextTick(absl::AnyInvocable<void() &&> callback) override
	{
		const int64_t now = clock.CurrentTime().us();
		queue.PostDelayedHighPrecisionTask(std::move(callback), TimeDelta::Micros((now / 15625 + 1) * 15625 - now));
	}

private:
	Clock &clock;
	TaskQueueBase &queue;
};
} // namespace
int main(int argc, char **argv)
{
	if (argc != 2 || (std::string(argv[1]) != "metronome" && std::string(argv[1]) != "precise"))
		return 2;
	const bool precise = std::string(argv[1]) == "precise";
	int scenarios = 0, coalesced = 0;
	for (int fps : {30, 60, 120})
		for (int phase = 0; phase < 15625; phase += 1000)
			for (int reset = 0; reset < 2; ++reset) {
				SimulatedClock clock(Timestamp::Millis(1000));
				GlobalClock global(clock);
				Queue queue(clock);
				Ticks ticks(clock, queue);
				DecodeSynchronizer sync(&clock, &ticks, &queue);
				auto scheduler = precise ? std::unique_ptr<FrameDecodeScheduler>(
				                               std::make_unique<TaskQueueFrameDecodeScheduler>(&clock, &queue))
				                         : sync.CreateSynchronizedFrameScheduler();
				std::vector<int64_t> releases;
				int64_t max_gap = 0, min_error = 0, max_error = 0;
				constexpr int frames = 1800;
				const int64_t start = 1500000 + phase;
				auto schedule = [&](auto &&self, int frame) -> void {
					const int64_t deadline = start + int64_t(frame) * 1000000 / fps;
					scheduler->ScheduleFrame(90000 + frame * (90000 / fps),
					                         {Timestamp::Micros(deadline), Timestamp::Micros(deadline + 15000)},
					                         [&, frame, deadline](uint32_t rtp, Timestamp render) {
						                         if (rtp != 90000 + frame * (90000 / fps) ||
						                             render.us() != deadline + 15000)
							                         std::abort();
						                         const int64_t now = clock.CurrentTime().us();
						                         if (!releases.empty())
							                         max_gap = std::max(max_gap, now - releases.back());
						                         releases.push_back(now);
						                         min_error = std::min(min_error, now - deadline);
						                         max_error = std::max(max_error, now - deadline);
						                         if (frame + 1 < frames)
							                         self(self, frame + 1);
					                         });
				};
				schedule(schedule, 0);
				queue.Until(Timestamp::Micros(start + int64_t(frames) * 1000000 / fps + 100000));
				scheduler->Stop();
				scheduler.reset();
				if (releases.size() != frames || max_error > 10000 || min_error < -15625)
					return 1;
				if (precise && (min_error != 0 || max_error != 0 || max_gap > 1000000 / fps + 1))
					return 1;
				coalesced += max_gap > 1000000 / fps + 8000;
				++scenarios;
				std::cout << argv[1] << ',' << fps << ',' << phase << ',' << reset << ',' << min_error << ','
				          << max_error << ',' << max_gap << '\n';
			}
	std::cerr << "scenarios=" << scenarios << " coalesced_release_cases=" << coalesced << '\n';
	return precise ? (coalesced ? 1 : 0) : (coalesced ? 0 : 1);
}
