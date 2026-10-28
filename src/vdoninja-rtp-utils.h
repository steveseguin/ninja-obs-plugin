/*
 * OBS VDO.Ninja Plugin
 * RTP utility functions — codec-specific payload descriptor parsing
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vdoninja
{

struct RtpPayloadView {
	size_t offset = 0;
	size_t size = 0;
	uint8_t payloadType = 0;
};

// Validate RTP headers, extensions, and padding before accessing the payload.
std::optional<RtpPayloadView> parseRtpPayloadView(const uint8_t *packetData, size_t packetSize);

inline constexpr const char *kColorSpaceExtensionUri = "http://www.webrtc.org/experiments/rtp-hdrext/color-space";
// Add negotiated SDR color metadata to a bare RTP marker packet only. Preserve
// sequence/timestamp/payload so pacing and NACK repair use the identical packet.
bool addRtpColorSpaceExtension(std::vector<std::byte> &packet, uint8_t id, const std::array<uint8_t, 4> &color);

// Extract the primary encoded block from an RFC 2198 payload.
std::optional<std::vector<uint8_t>> extractRedPrimaryPayload(const uint8_t *payload, size_t payloadSize);

// Remove the RTX original-sequence prefix in place after validating packet bounds.
// Returns the new packet size; invalid packets are left untouched.
std::optional<size_t> normalizeRtxPacket(uint8_t *packetData, size_t packetSize, uint8_t originalPayloadType);

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

// Inspect the encoded access unit, never the sender's RTP prediction flag.
bool isVp9Keyframe(const uint8_t *data, size_t size);
bool isH264Keyframe(const uint8_t *data, size_t size);
bool isHevcKeyframe(const uint8_t *data, size_t size);
bool av1HasSequenceHeader(const uint8_t *data, size_t size);

// Payload-only packetization lets the publisher retain its existing RTP
// sequence numbers, pacing, retransmission cache and keyframe gate.
using VideoRtpPayloads = std::vector<std::vector<uint8_t>>;
std::optional<VideoRtpPayloads> packetizeHevc(const uint8_t *data, size_t size, size_t maxPayload);
std::optional<VideoRtpPayloads> packetizeAv1(const uint8_t *data, size_t size, size_t maxPayload);

enum class ExtendedVideoCodec { HEVC, AV1 };

// Derive SDP profile/tier/level from the encoder's sequence header, including
// HEVC range-extension profiles used for 4:4:4. Never guess from the encoder ID.
std::optional<std::string> encodedVideoFormat(ExtendedVideoCodec codec, const uint8_t *data, size_t size);
bool offeredVideoFormatSupports(ExtendedVideoCodec codec, const std::string &offer, const std::string &encoded);

// A bounded access-unit assembler. The receive queue checks RTP sequence
// continuity; reset() is also required whenever that queue detects a gap.
class VideoRtpAssembler
{
public:
	static constexpr size_t maxBytes = 4 * 1024 * 1024;
	void reset();
	std::optional<std::vector<uint8_t>> push(ExtendedVideoCodec codec, const uint8_t *payload, size_t size,
	                                         uint32_t timestamp, bool marker);

private:
	bool appendHevc(const uint8_t *payload, size_t size);
	bool appendAv1(const uint8_t *payload, size_t size);
	bool appendObu(const uint8_t *data, size_t size);
	bool active_ = false;
	uint32_t timestamp_ = 0;
	std::optional<uint32_t> rejectedTimestamp_;
	std::vector<uint8_t> frame_;
	std::vector<uint8_t> fragment_;
};

// True when at least one clock-second of RTP time has elapsed since the last
// sender report. RTP timestamps wrap, so the comparison is made on a wrapped
// delta; a timestamp that moved backwards is never treated as due.
bool isRtcpSenderReportDue(uint32_t currentTimestamp, uint32_t lastReportedTimestamp, uint32_t clockRate);

// Preserve forward RTP time across 32-bit rollover. Only duplicate/backward
// encoder timestamps need the fallback step; first timestamps are unchanged.
uint32_t sanitizeMonotonicTimestamp(uint32_t candidate, bool &hasLast, uint32_t &last, uint32_t fallbackStep);

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
