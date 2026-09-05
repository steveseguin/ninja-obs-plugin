/*
 * OBS VDO.Ninja Plugin
 * RTP utility functions — codec-specific payload descriptor parsing
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-rtp-utils.h"

#include <algorithm>
#include <cstring>

namespace vdoninja
{

std::optional<RtpPayloadView> parseRtpPayloadView(const uint8_t *packetData, size_t packetSize)
{
	if (!packetData || packetSize < 12) {
		return std::nullopt;
	}

	const uint8_t version = static_cast<uint8_t>((packetData[0] >> 6) & 0x03);
	if (version != 2) {
		return std::nullopt;
	}

	// With RTP/RTCP mux enabled, track callbacks can surface RTCP control packets
	// alongside media packets. RTCP packet types occupy the raw second-octet range
	// 192-223, while our RTP payload types are dynamic values outside that band.
	const uint8_t rawPacketType = packetData[1];
	if (rawPacketType >= 192 && rawPacketType <= 223) {
		return std::nullopt;
	}

	const bool hasPadding = (packetData[0] & 0x20) != 0;
	const bool hasExtension = (packetData[0] & 0x10) != 0;
	const size_t csrcCount = static_cast<size_t>(packetData[0] & 0x0F);
	const uint8_t payloadType = static_cast<uint8_t>(packetData[1] & 0x7F);

	size_t headerSize = 12 + (csrcCount * 4);
	if (headerSize > packetSize) {
		return std::nullopt;
	}

	if (hasExtension) {
		if (headerSize + 4 > packetSize) {
			return std::nullopt;
		}
		const size_t extensionWords = static_cast<size_t>((static_cast<uint16_t>(packetData[headerSize + 2]) << 8) |
		                                                  static_cast<uint16_t>(packetData[headerSize + 3]));
		headerSize += 4 + (extensionWords * 4);
		if (headerSize > packetSize) {
			return std::nullopt;
		}
	}

	size_t payloadSize = packetSize - headerSize;
	if (hasPadding) {
		const uint8_t paddingSize = packetData[packetSize - 1];
		if (paddingSize == 0 || paddingSize > payloadSize) {
			return std::nullopt;
		}
		payloadSize -= paddingSize;
	}

	if (payloadSize == 0) {
		return std::nullopt;
	}

	return RtpPayloadView{headerSize, payloadSize, payloadType};
}

std::optional<std::vector<uint8_t>> extractRedPrimaryPayload(const uint8_t *payload, size_t payloadSize)
{
	if (!payload || payloadSize < 2) {
		return std::nullopt;
	}
	size_t index = 0;
	size_t redundantBytes = 0;
	while (index < payloadSize) {
		if ((payload[index] & 0x80) == 0) {
			++index;
			if (redundantBytes >= payloadSize - index) {
				return std::nullopt;
			}
			index += redundantBytes;
			return std::vector<uint8_t>(payload + index, payload + payloadSize);
		}
		if (payloadSize - index < 4) {
			return std::nullopt;
		}
		const size_t blockLength = (static_cast<size_t>(payload[index + 2] & 0x03) << 8) | payload[index + 3];
		if (blockLength > payloadSize - redundantBytes) {
			return std::nullopt;
		}
		redundantBytes += blockLength;
		index += 4;
	}
	return std::nullopt;
}

std::optional<size_t> normalizeRtxPacket(uint8_t *packetData, size_t packetSize, uint8_t originalPayloadType)
{
	const auto payload = parseRtpPayloadView(packetData, packetSize);
	if (!payload || payload->size <= 2 || originalPayloadType > 127) {
		return std::nullopt;
	}
	// The first two payload bytes are the original sequence number. Preserve
	// CSRCs, header extensions, marker, SSRC, and trailing RTP padding.
	packetData[2] = packetData[payload->offset];
	packetData[3] = packetData[payload->offset + 1];
	packetData[1] = (packetData[1] & 0x80) | originalPayloadType;
	std::memmove(packetData + payload->offset, packetData + payload->offset + 2, packetSize - payload->offset - 2);
	return packetSize - 2;
}

// ---------------------------------------------------------------------------
// VP9 RTP payload descriptor parser — RFC 9628 section 4.2
//
// Mandatory first byte layout:
//   bit 7: I  — PictureID present
//   bit 6: P  — inter-picture predicted layer frame
//   bit 5: L  — layer indices present
//   bit 4: F  — flexible mode (P_DIFFs present when P=1)
//   bit 3: B  — start of VP9 frame (first packet)
//   bit 2: E  — end of VP9 frame (last packet)
//   bit 1: V  — scalability structure (SS) present
//   bit 0: Z  — not a reference for upper spatial layers
//
// Optional fields follow in this order:
//   PictureID     (if I=1): 1 byte (M=0, 7-bit PID) or 2 bytes (M=1, 15-bit PID)
//   Layer indices (if L=1): 1 byte + 1 byte TL0PICIDX (non-flexible, F=0 only)
//   P_DIFFs       (if F=1 and P=1): variable-length, each byte is P_DIFF(7)+N; stop when N=0
//   Scalability structure (if V=1): variable-length
// ---------------------------------------------------------------------------

Vp9DescriptorResult parseVP9PayloadDescriptor(const uint8_t *payload, size_t size)
{
	Vp9DescriptorResult result;

	if (!payload || size == 0) {
		return result;
	}

	size_t offset = 0;

	// --- Mandatory descriptor byte ---
	const uint8_t desc = payload[offset++];
	const bool I = (desc & 0x80) != 0;
	const bool P = (desc & 0x40) != 0;
	const bool L = (desc & 0x20) != 0;
	const bool F = (desc & 0x10) != 0;
	const bool B = (desc & 0x08) != 0;
	const bool E = (desc & 0x04) != 0;
	const bool V = (desc & 0x02) != 0;
	// Z bit (0x01) is not needed for reassembly

	// --- PictureID (optional) ---
	if (I) {
		if (offset >= size) {
			return result;
		}
		const bool M = (payload[offset] & 0x80) != 0;
		offset++; // consume first PID byte
		if (M) {
			if (offset >= size) {
				return result;
			}
			offset++; // consume second byte of 15-bit PID
		}
	}

	// --- Layer indices (optional) ---
	if (L) {
		if (offset >= size) {
			return result;
		}
		offset++; // TID/U/SID/D byte
		if (!F) {
			// Non-flexible mode: TL0PICIDX follows
			if (offset >= size) {
				return result;
			}
			offset++; // TL0PICIDX
		}
	}

	// --- P_DIFFs (flexible mode, present when F=1 and P=1) ---
	if (F && P) {
		// Each P_DIFF is one byte: bits[7:1] = P_DIFF value, bit[0] = N (another follows if 1)
		// RFC 9628 allows at most 3 P_DIFFs.
		for (int i = 0; i < 3; ++i) {
			if (offset >= size) {
				return result;
			}
			const bool N = (payload[offset] & 0x01) != 0;
			if ((payload[offset] >> 1) == 0 || (i == 2 && N)) {
				// RFC 9628 forbids zero P_DIFF and more than three references.
				return result;
			}
			offset++;
			if (!N) {
				break;
			}
		}
	}

	// --- Scalability structure (optional) ---
	if (V) {
		if (offset >= size) {
			return result;
		}
		const uint8_t ss = payload[offset++];
		const uint8_t N_S = static_cast<uint8_t>((ss >> 5) & 0x07);
		const bool Y = (ss & 0x10) != 0;
		const bool G = (ss & 0x08) != 0;

		// Per-layer resolution: (N_S + 1) entries of WIDTH(2) + HEIGHT(2) = 4 bytes each
		if (Y) {
			const size_t resolutionBytes = static_cast<size_t>(N_S + 1) * 4;
			if (offset + resolutionBytes > size) {
				return result;
			}
			offset += resolutionBytes;
		}

		// Picture group description
		if (G) {
			if (offset >= size) {
				return result;
			}
			const uint8_t N_G = payload[offset++];
			for (uint8_t g = 0; g < N_G; ++g) {
				// |T(3)|U|R(2)|RES(2)|
				if (offset >= size) {
					return result;
				}
				const uint8_t R = (payload[offset] >> 2) & 0x03;
				offset++;
				// R reference P_DIFF bytes (8-bit each in SS context)
				if (offset + R > size) {
					return result;
				}
				offset += R;
			}
		}
	}

	result.valid = true;
	result.startOfFrame = B;
	result.endOfFrame = E;
	result.payloadOffset = offset;
	return result;
}

bool isRtcpSenderReportDue(uint32_t currentTimestamp, uint32_t lastReportedTimestamp, uint32_t clockRate)
{
	if (clockRate == 0) {
		return false;
	}

	// Modular subtraction keeps forward progress correct across the 32-bit
	// wrap, but it also turns a backwards timestamp into a value just under
	// 2^32, which would read as a hugely overdue report. Anything landing in
	// the upper half of the range is a regression, not elapsed time.
	constexpr uint32_t kBackwardsThreshold = 0x80000000u;
	const uint32_t elapsed = currentTimestamp - lastReportedTimestamp;
	if (elapsed >= kBackwardsThreshold) {
		return false;
	}

	return elapsed >= clockRate;
}

std::vector<uint8_t> buildOpusRtpPacket(const uint8_t *payload, size_t payloadSize, uint8_t payloadType,
                                        uint16_t sequenceNumber, uint32_t timestamp, uint32_t ssrc)
{
	std::vector<uint8_t> packet;
	if ((!payload && payloadSize != 0) || payloadSize > packet.max_size() - 12) {
		return {};
	}

	packet.resize(12 + payloadSize);
	packet[0] = 0x80;               // V=2, P=0, X=0, CC=0
	packet[1] = payloadType & 0x7F; // M=0
	packet[2] = sequenceNumber >> 8;
	packet[3] = sequenceNumber & 0xFF;
	packet[4] = timestamp >> 24;
	packet[5] = (timestamp >> 16) & 0xFF;
	packet[6] = (timestamp >> 8) & 0xFF;
	packet[7] = timestamp & 0xFF;
	packet[8] = ssrc >> 24;
	packet[9] = (ssrc >> 16) & 0xFF;
	packet[10] = (ssrc >> 8) & 0xFF;
	packet[11] = ssrc & 0xFF;
	if (payloadSize != 0) {
		std::memcpy(packet.data() + 12, payload, payloadSize);
	}
	return packet;
}

RtpTimestampStepTracker::RtpTimestampStepTracker(uint32_t expectedStep) : expectedStep_(expectedStep) {}

void RtpTimestampStepTracker::observe(uint32_t timestamp)
{
	interval_.packets++;
	if (!hasLastTimestamp_) {
		hasLastTimestamp_ = true;
		lastTimestamp_ = timestamp;
		return;
	}

	const uint32_t step = timestamp - lastTimestamp_;
	lastTimestamp_ = timestamp;

	constexpr uint32_t kBackwardsThreshold = 0x80000000u;
	if (step == 0 || step >= kBackwardsThreshold) {
		interval_.nonForwardSteps++;
		return;
	}

	interval_.maxForwardStep = std::max(interval_.maxForwardStep, step);
	if (expectedStep_ != 0 && step > expectedStep_) {
		interval_.largeSteps++;
	}
}

RtpTimestampStepStats RtpTimestampStepTracker::takeInterval()
{
	const RtpTimestampStepStats snapshot = interval_;
	interval_ = {};
	return snapshot;
}

void RtpTimestampStepTracker::reset()
{
	hasLastTimestamp_ = false;
	lastTimestamp_ = 0;
	interval_ = {};
}

} // namespace vdoninja
