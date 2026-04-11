/*
 * OBS VDO.Ninja Plugin
 * RTP utility functions — codec-specific payload descriptor parsing
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace vdoninja
{

// ---------------------------------------------------------------------------
// VP9 RTP payload descriptor (RFC 9628)
// ---------------------------------------------------------------------------

struct Vp9DescriptorResult {
	bool valid = false;        // false if descriptor is malformed or payload too short
	bool startOfFrame = false; // B bit: this packet is the first packet of a VP9 frame
	bool endOfFrame = false;   // E bit: this packet is the last packet of a VP9 frame
	size_t payloadOffset = 0;  // byte offset past the descriptor; VP9 bitstream starts here
};

// Parse the VP9 RTP payload descriptor per RFC 9628.
//
// payload: pointer to the start of the RTP payload (after the fixed RTP header,
//          any CSRC list, any RTP header extension, and any padding removal).
// size:    number of bytes available at payload.
//
// Returns a Vp9DescriptorResult. Check .valid before using other fields.
// On success, the VP9 bitstream data begins at payload[result.payloadOffset].
Vp9DescriptorResult parseVP9PayloadDescriptor(const uint8_t *payload, size_t size);

} // namespace vdoninja
