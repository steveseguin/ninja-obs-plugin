#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include "modules/video_coding/timing/timing.h"
#include "system_wrappers/include/clock.h"

namespace
{
int incoming = 0, uninitialized = 0, bootstraps = 0;
class Trials final : public webrtc::FieldTrialsView
{
	std::string Lookup(absl::string_view) const override { return ""; }
};
void require(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}
} // namespace

namespace webrtc
{
void ProbeTimingEvent(const char *event)
{
	const std::string name(event);
	if (name == "incoming")
		++incoming;
	else if (name == "uninitialized")
		++uninitialized;
	else if (name == "bootstrap")
		++bootstraps;
}
} // namespace webrtc

int main()
{
	using namespace webrtc;
	Trials trials;
	std::cout
	    << "variant,case,frames,incoming_samples,uninitialized_queries,bootstrap_samples,min_lead_ms,max_lead_ms\n";
	for (const bool warm : {false, true}) {
		for (const bool wrap : {false, true}) {
			for (const int buffer : {300, 1000}) {
				SimulatedClock clock(Timestamp::Millis(1000));
				VCMTiming timing(&clock, trials);
				// Reuse the timing object through resets to model reconnects.
				for (int reconnect = 0; reconnect < 3; ++reconnect) {
					timing.Reset();
					timing.set_min_playout_delay(TimeDelta::Millis(buffer));
					incoming = uninitialized = bootstraps = 0;
					int64_t min_lead = 100000, max_lead = -100000;
					const auto start = clock.CurrentTime();
					const uint32_t first = wrap ? 0xffff0000u : 90000u;
					Timestamp prior_render = Timestamp::Zero();
					for (int frame = 0; frame < 1800; ++frame) {
						const auto now = start + TimeDelta::Millis(frame * 1000 / 30 + 25 + (frame % 2 ? 8 : -8));
						clock.AdvanceTime(now - clock.CurrentTime());
						const uint32_t rtp = first + static_cast<uint32_t>(frame) * 3000;
						// Model the controller's retransmission exclusion explicitly.
						// This probe runs upstream timing code, not the full controller.
						const bool delayed_by_retransmission = !(warm && frame == 0);
						if (!delayed_by_retransmission)
							timing.IncomingTimestamp(rtp, now);
						const auto render = timing.RenderTime(rtp, now);
						const auto lead = (render - now).ms();
						min_lead = std::min(min_lead, lead);
						max_lead = std::max(max_lead, lead);
						if (warm || PROBE_BOOTSTRAP) {
							require(lead >= buffer - 17 && lead <= buffer + 1,
							        "Initialized timing did not retain the requested buffer");
							if (frame)
								require((render - prior_render).ms() >= 32 && (render - prior_render).ms() <= 34,
								        "RTP-clocked render cadence was not retained");
						} else {
							require(lead == 0, "Baseline starvation was not reproduced");
						}
						prior_render = render;
					}
					require(incoming == (warm ? 1 : 0), "Unexpected clean timing sample");
					require(bootstraps == (!warm && PROBE_BOOTSTRAP ? 1 : 0),
					        "Bootstrap must happen once, including after reset");
					require(uninitialized == (warm              ? 0
					                          : PROBE_BOOTSTRAP ? 1
					                                            : 1800),
					        "Unexpected internal extrapolator state");
					std::cout << (PROBE_BOOTSTRAP ? "bootstrap" : "baseline") << ',' << (warm ? "warm" : "cold")
					          << "-wrap" << wrap << "-buffer" << buffer << "-reconnect" << reconnect << ",1800,"
					          << incoming << ',' << uninitialized << ',' << bootstraps << ',' << min_lead << ','
					          << max_lead << '\n';
				}
			}
		}
	}
}
