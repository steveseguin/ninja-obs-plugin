/*
 * OBS VDO.Ninja Plugin
 * RFC 2198 audio RED helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vdoninja
{

constexpr uint8_t kDefaultOpusPayloadType = 111;
constexpr uint8_t kDefaultAudioRedPayloadType = 63;
constexpr size_t kDefaultMaximumAudioRtpPayloadSize = 1200;

struct AudioRedPayload {
	std::vector<uint8_t> bytes;
	bool includesRedundantBlock = false;
	size_t redundantBytes = 0;
};

// Builds one RFC 2198 payload whose primary block is the current Opus frame and,
// when it fits the RTP payload budget, whose redundant block is the preceding
// Opus frame. The timestamp subtraction deliberately uses RTP wrap semantics.
AudioRedPayload buildAudioRedPayload(const uint8_t *currentPayload, size_t currentPayloadSize,
                                     uint32_t currentTimestamp, const uint8_t *previousPayload,
                                     size_t previousPayloadSize, uint32_t previousTimestamp,
                                     uint8_t opusPayloadType = kDefaultOpusPayloadType,
                                     size_t maximumPayloadSize = kDefaultMaximumAudioRtpPayloadSize);

// The publisher offers RED before Opus only when the feature is enabled. Use RED
// only when the answer retains a valid RED/Opus mapping and orders RED before
// Opus; otherwise the peer safely remains on ordinary Opus.
bool answerSelectsAudioRed(const std::string &sdp, uint8_t redPayloadType = kDefaultAudioRedPayloadType,
                           uint8_t opusPayloadType = kDefaultOpusPayloadType);

struct AudioRedStats {
	uint64_t packets = 0;
	uint64_t packetsWithRedundancy = 0;
	uint64_t primaryOnlyPackets = 0;
	uint64_t redundantBytes = 0;
};

} // namespace vdoninja
