/*
 * OBS VDO.Ninja Plugin
 * RTP utility functions — codec-specific payload descriptor parsing
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

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

// True when at least one clock-second of RTP time has elapsed since the last
// sender report. RTP timestamps wrap, so the comparison is made on a wrapped
// delta; a timestamp that moved backwards is never treated as due.
bool isRtcpSenderReportDue(uint32_t currentTimestamp, uint32_t lastReportedTimestamp, uint32_t clockRate);

// Build one RFC 3550 RTP packet carrying an already-encoded Opus payload.
// The caller owns sequence/timestamp state so it can preserve continuity
// across peer repairs.
std::vector<uint8_t> buildOpusRtpPacket(const uint8_t *payload, size_t payloadSize, uint8_t payloadType,
                                        uint16_t sequenceNumber, uint32_t timestamp, uint32_t ssrc);

struct RtpTimestampStepStats {
	uint64_t packets = 0;
	uint64_t largeSteps = 0;
	uint64_t nonForwardSteps = 0;
	uint32_t maxForwardStep = 0;
};

// Records objective RTP timestamp continuity without changing timestamps.
// A step larger than expected may be a missing encoded packet or an encoder
// using a longer frame duration; the rolling log reports it as a "large step"
// rather than assuming packet loss.
class RtpTimestampStepTracker
{
public:
	explicit RtpTimestampStepTracker(uint32_t expectedStep);

	void observe(uint32_t timestamp);
	RtpTimestampStepStats takeInterval();
	void reset();

private:
	const uint32_t expectedStep_;
	bool hasLastTimestamp_ = false;
	uint32_t lastTimestamp_ = 0;
	RtpTimestampStepStats interval_;
};

} // namespace vdoninja
