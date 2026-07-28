/*
 * OBS VDO.Ninja Plugin
 * Opt-in video loss-protection policy
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-loss-protection.h"

#include <algorithm>
#include <limits>

namespace vdoninja
{

namespace
{

// A high-protection stream copies every packet. The encoder bitrate describes
// compressed H.264 payload, while the duplicate pacer charges complete RTP
// packets and must also absorb short keyframe bursts. Without a small bounded
// margin, an encoder running exactly at its target leaves copies queued until
// their latency deadline expires.
constexpr uint64_t kHighProtectionPacketizationHeadroomPercent = 5;

} // namespace

VideoProtectionMode videoProtectionModeFromInt(int value)
{
	switch (value) {
	case static_cast<int>(VideoProtectionMode::Low):
		return VideoProtectionMode::Low;
	case static_cast<int>(VideoProtectionMode::Medium):
		return VideoProtectionMode::Medium;
	case static_cast<int>(VideoProtectionMode::High):
		return VideoProtectionMode::High;
	case static_cast<int>(VideoProtectionMode::Off):
	default:
		return VideoProtectionMode::Off;
	}
}

VideoProtectionPolicy videoProtectionPolicy(VideoProtectionMode mode)
{
	switch (mode) {
	case VideoProtectionMode::Low:
		// Protect the reference frame that is most expensive to lose while
		// retaining a modest long-term repair allowance.
		return {true, 0, 20};
	case VideoProtectionMode::Medium:
		// Protect every keyframe packet and a deterministic quarter of delta
		// packets. The common scheduler still paces primary and protection
		// traffic together.
		return {true, 4, 50};
	case VideoProtectionMode::High:
		return {true, 1, 100};
	case VideoProtectionMode::Off:
	default:
		return {};
	}
}

uint64_t videoProtectionBitrateForEncoderRate(int encoderBitrate, VideoProtectionMode mode) noexcept
{
	const VideoProtectionPolicy policy = videoProtectionPolicy(mode);
	uint64_t percent = policy.duplicateBudgetPercent;
	if (percent == 0) {
		return 0;
	}
	if (mode == VideoProtectionMode::High) {
		percent += kHighProtectionPacketizationHeadroomPercent;
	}

	const uint64_t positiveBitrate = static_cast<uint64_t>(std::max(encoderBitrate, 1));
	if (positiveBitrate > std::numeric_limits<uint64_t>::max() / percent) {
		return std::numeric_limits<uint64_t>::max();
	}
	return positiveBitrate * percent / 100U;
}

bool shouldDuplicateVideoPacket(VideoProtectionMode mode, bool keyframe, uint16_t sequenceNumber)
{
	const VideoProtectionPolicy policy = videoProtectionPolicy(mode);
	if (keyframe) {
		return policy.duplicateKeyframes;
	}
	return policy.deltaPacketInterval > 0 && sequenceNumber % policy.deltaPacketInterval == 0;
}

const char *videoProtectionModeName(VideoProtectionMode mode)
{
	switch (mode) {
	case VideoProtectionMode::Low:
		return "low";
	case VideoProtectionMode::Medium:
		return "medium";
	case VideoProtectionMode::High:
		return "high";
	case VideoProtectionMode::Off:
	default:
		return "off";
	}
}

} // namespace vdoninja
