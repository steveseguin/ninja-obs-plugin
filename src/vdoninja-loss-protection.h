/*
 * OBS VDO.Ninja Plugin
 * Opt-in video loss-protection policy
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstdint>

namespace vdoninja
{

// This setting controls paced duplication of ordinary RTP packets. It is
// deliberately distinct from negotiated RTP RED/ULPFEC.
enum class VideoProtectionMode {
	Off = 0,
	Low = 1,
	Medium = 2,
	High = 3,
};

struct VideoProtectionPolicy {
	bool duplicateKeyframes = false;
	// Zero disables delta duplication; one duplicates every packet; values
	// above one select one packet from each deterministic interval.
	uint16_t deltaPacketInterval = 0;
	uint16_t duplicateBudgetPercent = 0;
};

VideoProtectionMode videoProtectionModeFromInt(int value);
VideoProtectionPolicy videoProtectionPolicy(VideoProtectionMode mode);
uint64_t videoProtectionBitrateForEncoderRate(int encoderBitrate, VideoProtectionMode mode) noexcept;
bool shouldDuplicateVideoPacket(VideoProtectionMode mode, bool keyframe, uint16_t sequenceNumber);
const char *videoProtectionModeName(VideoProtectionMode mode);

} // namespace vdoninja
