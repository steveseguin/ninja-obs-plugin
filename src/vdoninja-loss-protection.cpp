/*
 * OBS VDO.Ninja Plugin
 * Opt-in video loss-protection policy
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-loss-protection.h"

namespace vdoninja
{

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
