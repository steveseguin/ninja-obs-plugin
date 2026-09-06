// Exercise the actual Chromium renderer with exact RTP cadence and perturbed
// reference times. No codec, GPU, network or reimplemented frame selector.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/metrics/field_trial.h"
#include "base/time/time.h"
#include "media/base/media_util.h"
#include "media/base/video_frame.h"
#include "media/filters/video_renderer_algorithm.h"

struct Event {
	int64_t time, deadline;
	int frame;
};

int main(int argc, char **argv)
{
	base::AtExitManager at_exit;
	base::CommandLine::Init(argc, argv);
	base::FieldTrialList trials;
	const auto args = base::CommandLine::ForCurrentProcess()->GetArgs();
	const bool rtp = !args.empty() && args[0] == "rtp";
	const bool delay_step = args.size() > 1 && args[1] == "step";
	const bool decode_step = args.size() > 1 && args[1] == "decode-step";
	const int headroom_ms = decode_step && args.size() > 2 ? std::stoi(args[2]) : 0;
	const int decode_lead_ms = delay_step && args.size() > 2 ? std::stoi(args[2]) : 30;
	if (delay_step && (!rtp || (decode_lead_ms != 15 && decode_lead_ms != 31)))
		return 2;
	if (decode_step && (!rtp || (headroom_ms != 0 && headroom_ms != 16)))
		return 2;
	if (rtp)
		base::FieldTrialList::CreateFieldTrial("WebRTC-RtpCadenceSamples", "Enabled");
	auto features = std::make_unique<base::FeatureList>();
	features->InitFromCommandLine("", "");
	base::FeatureList::SetInstance(std::move(features));
	int failed = 0, baseline_jitter_failures = 0;
	const uint32_t rtp_base = args.size() > 1 && args[1] == "wrap" ? 0xffff0000u : 90000u;
	const std::vector<int> perturbations = delay_step    ? std::vector<int>{0, 15, 19}
	                                       : decode_step ? std::vector<int>{0, 20}
	                                                     : std::vector<int>{0, 2};
	std::cout << "policy,fps,jitter_ms,phase_ms,missing,max_cadence_deviation_ms\n";
	for (int fps : {30, 60})
		for (int jitter : perturbations)
			for (int phase = 0; phase <= 16; phase += 2) {
				const int count = fps * 60;
				const auto epoch = base::TimeTicks() + base::Seconds(1);
				media::NullMediaLog log;
				media::VideoRendererAlgorithm renderer(
				    base::BindRepeating(
				        [](base::TimeTicks epoch, const std::vector<base::TimeDelta> &stamps,
				           std::vector<base::TimeTicks> *wall) {
					        for (auto stamp : stamps)
						        wall->push_back(epoch + stamp);
					        return true;
				        },
				        epoch),
				    &log);
				std::vector<Event> events;
				std::vector<int64_t> references;
				for (int i = 0; i < count; ++i) {
					const int64_t correction = decode_step  ? 0
					                           : delay_step ? (i >= fps * 30 ? jitter * 1000 : 0)
					                                        : ((i / (fps * 2)) % 2 ? jitter * 1000 : 0);
					const int64_t ref = std::llround(i * 1000.0 / fps) * 1000 + correction;
					references.push_back(ref);
					const int lead = decode_step ? 28 - (i >= fps * 30 ? jitter : 0) + headroom_ms : decode_lead_ms;
					events.push_back({ref - lead * 1000, 0, i});
				}
				for (int i = 0; i < 3610; ++i) {
					const int64_t deadline = std::llround(i * 1000000.0 / 60) + phase * 1000;
					events.push_back({deadline - 16000, deadline, -1});
				}
				std::stable_sort(events.begin(), events.end(),
				                 [](const Event &a, const Event &b) { return a.time < b.time; });
				std::set<int> seen;
				int last = -1;
				int64_t last_deadline = 0;
				double deviation = 0;
				for (const auto &event : events) {
					if (event.frame >= 0) {
						auto frame = media::VideoFrame::CreateBlackFrame(gfx::Size(8, 8));
						frame->set_timestamp(base::Microseconds(references[event.frame]));
						frame->metadata().rtp_timestamp =
						    static_cast<double>(rtp_base + static_cast<uint32_t>(event.frame * (90000 / fps)));
						renderer.EnqueueFrame(std::move(frame));
					} else {
						size_t dropped = 0;
						auto frame = renderer.Render(epoch + base::Microseconds(event.deadline),
						                             epoch + base::Microseconds(event.deadline + 16667), &dropped);
						if (!frame)
							continue;
						int id = (static_cast<uint32_t>(*frame->metadata().rtp_timestamp) - rtp_base) / (90000 / fps);
						if (id != last) {
							if (id >= fps * 5 && id < count - fps * 5 && last >= fps * 5) {
								deviation = std::max(
								    deviation, std::abs((event.deadline - last_deadline) / 1000.0 - 1000.0 / fps));
							}
							seen.insert(id);
							last = id;
							last_deadline = event.deadline;
						}
					}
				}
				int missing = 0;
				for (int i = fps * 5; i < count - fps * 5; ++i)
					missing += !seen.contains(i);
				std::cout << (rtp ? "rtp" : "reference") << ',' << fps << ',' << jitter << ',' << phase << ','
				          << missing << ',' << deviation << '\n';
				if (jitter && (missing || deviation > std::max(8.0, 750.0 / fps)))
					++baseline_jitter_failures;
				if ((rtp || jitter == 0) && (missing || deviation > std::max(8.0, 750.0 / fps)))
					++failed;
			}
	if (decode_step) {
		std::cerr << "decode-step: headroom_ms=" << headroom_ms << " strict_failures=" << failed << "/36\n";
		return headroom_ms ? (failed ? 1 : 0)
		                   : (baseline_jitter_failures > 0 && failed == baseline_jitter_failures ? 0 : 1);
	}
	if (delay_step) {
		std::cerr << "delay-step characterization: lead_ms=" << decode_lead_ms << " strict_failures=" << failed
		          << "/54\n";
		// A successful characterization must retain the delay-step failures and
		// pass the zero-step controls. It is not a passing playback result.
		return baseline_jitter_failures > 0 && failed == baseline_jitter_failures ? 0 : 1;
	}
	if (!rtp && !baseline_jitter_failures)
		return 2; // A characterization must actually reproduce the baseline fault.
	return failed ? 1 : 0;
}
