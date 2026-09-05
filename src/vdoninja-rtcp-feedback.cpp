/*
 * OBS VDO.Ninja Plugin
 * Passive RTCP feedback telemetry
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-rtcp-feedback.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace vdoninja
{

namespace
{

constexpr uint8_t kRtcpVersion = 2;
constexpr uint8_t kReceiverReportPayloadType = 201;
constexpr uint8_t kTransportFeedbackPayloadType = 205;
constexpr uint8_t kPayloadFeedbackPayloadType = 206;
constexpr uint8_t kNackFeedbackMessageType = 1;
constexpr uint8_t kPliFeedbackMessageType = 1;
constexpr uint8_t kFirFeedbackMessageType = 4;
constexpr uint8_t kApplicationFeedbackMessageType = 15;
constexpr size_t kRtcpHeaderBytes = 4;
constexpr size_t kFeedbackHeaderBytes = 12;
constexpr size_t kRembHeaderBytes = 20;
constexpr size_t kReceiverReportHeaderBytes = 8;
constexpr size_t kReceiverReportBlockBytes = 24;
constexpr uint64_t kMaximumPlausibleRttMs = 60000;
constexpr uint64_t kNtpEpochOffsetSeconds = 2208988800ULL;

uint16_t readU16(const uint8_t *data)
{
	return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]));
}

uint32_t readU32(const uint8_t *data)
{
	return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
	       (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

int32_t readSignedU24(const uint8_t *data)
{
	uint32_t value = (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) | data[2];
	if ((value & 0x00800000U) != 0) {
		value |= 0xFF000000U;
	}
	return static_cast<int32_t>(value);
}

uint32_t countBits(uint16_t value)
{
	uint32_t count = 0;
	while (value != 0) {
		value &= static_cast<uint16_t>(value - 1);
		++count;
	}
	return count;
}

uint64_t decodeRembBitrate(const uint8_t *packet)
{
	const uint8_t exponent = packet[17] >> 2U;
	const uint32_t mantissa = (static_cast<uint32_t>(packet[17] & 0x03U) << 16U) |
	                          (static_cast<uint32_t>(packet[18]) << 8U) | static_cast<uint32_t>(packet[19]);
	if (mantissa == 0) {
		return 0;
	}
	if (exponent >= 64 || static_cast<uint64_t>(mantissa) > (std::numeric_limits<uint64_t>::max() >> exponent)) {
		return std::numeric_limits<uint64_t>::max();
	}
	return static_cast<uint64_t>(mantissa) << exponent;
}

} // namespace

RtcpFeedbackTracker::RtcpFeedbackTracker(uint32_t mediaSsrc) : mediaSsrc_(mediaSsrc) {}

bool RtcpFeedbackTracker::matchesMediaSsrc(uint32_t mediaSsrc) const noexcept
{
	return mediaSsrc_ == 0 || mediaSsrc == mediaSsrc_;
}

void RtcpFeedbackTracker::observe(const uint8_t *data, size_t size, uint32_t compactNtpNow)
{
	RtcpFeedbackStats observed;
	observed.compoundPackets = 1;
	std::optional<RtcpRembEstimate> observedRemb;

	if (!data || size < kRtcpHeaderBytes) {
		observed.malformedPackets = 1;
		std::lock_guard<std::mutex> lock(mutex_);
		stats_.compoundPackets += observed.compoundPackets;
		stats_.malformedPackets += observed.malformedPackets;
		return;
	}

	bool malformed = false;
	size_t offset = 0;
	while (offset + kRtcpHeaderBytes <= size) {
		const uint8_t first = data[offset];
		const uint8_t version = first >> 6;
		const uint8_t reportCount = first & 0x1FU;
		const uint8_t payloadType = data[offset + 1];
		const size_t packetBytes = (static_cast<size_t>(readU16(data + offset + 2)) + 1U) * 4U;
		if (version != kRtcpVersion || packetBytes < kRtcpHeaderBytes || packetBytes > size - offset) {
			malformed = true;
			break;
		}

		const uint8_t *packet = data + offset;
		size_t contentBytes = packetBytes;
		if ((first & 0x20U) != 0) {
			// RTCP padding is valid only on the final packet of a compound
			// packet. Exclude it before interpreting feedback-control fields;
			// otherwise padding bytes can look like extra NACK requests.
			const size_t paddingBytes = packet[packetBytes - 1];
			if (offset + packetBytes != size || paddingBytes == 0 || paddingBytes > packetBytes - kRtcpHeaderBytes) {
				malformed = true;
				break;
			}
			contentBytes -= paddingBytes;
		}
		if (payloadType == kTransportFeedbackPayloadType && reportCount == kNackFeedbackMessageType) {
			if (contentBytes < kFeedbackHeaderBytes || (contentBytes - kFeedbackHeaderBytes) % 4U != 0) {
				malformed = true;
			} else if (matchesMediaSsrc(readU32(packet + 8))) {
				++observed.nackMessages;
				for (size_t fieldOffset = kFeedbackHeaderBytes; fieldOffset + 4U <= contentBytes; fieldOffset += 4U) {
					observed.nackRequestedPackets += 1U + countBits(readU16(packet + fieldOffset + 2));
				}
			}
		} else if (payloadType == kPayloadFeedbackPayloadType && reportCount == kPliFeedbackMessageType) {
			if (contentBytes < kFeedbackHeaderBytes) {
				malformed = true;
			} else if (matchesMediaSsrc(readU32(packet + 8))) {
				++observed.pliMessages;
			}
		} else if (payloadType == kPayloadFeedbackPayloadType && reportCount == kFirFeedbackMessageType) {
			if (contentBytes < kFeedbackHeaderBytes || (contentBytes - kFeedbackHeaderBytes) % 8U != 0) {
				malformed = true;
			} else {
				bool matched = mediaSsrc_ == 0;
				for (size_t fieldOffset = kFeedbackHeaderBytes; fieldOffset + 8U <= contentBytes; fieldOffset += 8U) {
					matched = matched || matchesMediaSsrc(readU32(packet + fieldOffset));
				}
				if (matched) {
					++observed.firMessages;
				}
			}
		} else if (payloadType == kPayloadFeedbackPayloadType && reportCount == kApplicationFeedbackMessageType &&
		           contentBytes >= 16 && packet[12] == 'R' && packet[13] == 'E' && packet[14] == 'M' &&
		           packet[15] == 'B') {
			if (contentBytes < kRembHeaderBytes) {
				malformed = true;
			} else {
				const uint8_t ssrcCount = packet[16];
				const size_t requiredBytes = kRembHeaderBytes + static_cast<size_t>(ssrcCount) * 4U;
				if (ssrcCount == 0 || contentBytes < requiredBytes) {
					malformed = true;
				} else {
					bool matched = mediaSsrc_ == 0;
					for (uint8_t index = 0; index < ssrcCount; ++index) {
						matched = matched || matchesMediaSsrc(readU32(packet + kRembHeaderBytes + index * 4U));
					}
					if (matched) {
						const uint64_t bitrate = decodeRembBitrate(packet);
						if (bitrate == 0) {
							malformed = true;
						} else {
							++observed.rembMessages;
							observed.minRembBitrateBps = observed.minRembBitrateBps == 0
							                                 ? bitrate
							                                 : std::min(observed.minRembBitrateBps, bitrate);
							observed.maxRembBitrateBps = std::max(observed.maxRembBitrateBps, bitrate);
							observedRemb = RtcpRembEstimate{bitrate, std::chrono::steady_clock::now()};
						}
					}
				}
			}
		} else if (payloadType == kReceiverReportPayloadType) {
			const size_t requiredBytes =
			    kReceiverReportHeaderBytes + static_cast<size_t>(reportCount) * kReceiverReportBlockBytes;
			if (contentBytes < requiredBytes) {
				malformed = true;
			} else {
				bool matchedReport = false;
				for (uint8_t index = 0; index < reportCount; ++index) {
					const uint8_t *block =
					    packet + kReceiverReportHeaderBytes + static_cast<size_t>(index) * kReceiverReportBlockBytes;
					if (!matchesMediaSsrc(readU32(block))) {
						continue;
					}

					matchedReport = true;
					++observed.reportBlocks;
					observed.maxFractionLost = std::max(observed.maxFractionLost, block[4]);
					observed.maxCumulativeLost = std::max(observed.maxCumulativeLost, readSignedU24(block + 5));
					observed.maxJitterTicks = std::max(observed.maxJitterTicks, readU32(block + 12));

					const uint32_t lastSenderReport = readU32(block + 16);
					const uint32_t delaySinceSenderReport = readU32(block + 20);
					if (compactNtpNow != 0 && lastSenderReport != 0) {
						const uint32_t roundTripUnits = compactNtpNow - lastSenderReport - delaySinceSenderReport;
						const uint64_t roundTripMs = (static_cast<uint64_t>(roundTripUnits) * 1000ULL) / 65536ULL;
						if (roundTripMs <= kMaximumPlausibleRttMs) {
							observed.maxRttMs = std::max(observed.maxRttMs, roundTripMs);
						}
					}
				}
				if (matchedReport) {
					++observed.receiverReports;
				}
			}
		}

		offset += packetBytes;
	}

	if (offset != size) {
		malformed = true;
	}
	observed.malformedPackets = malformed ? 1U : 0U;

	std::lock_guard<std::mutex> lock(mutex_);
	stats_.compoundPackets += observed.compoundPackets;
	stats_.malformedPackets += observed.malformedPackets;
	stats_.nackMessages += observed.nackMessages;
	stats_.nackRequestedPackets += observed.nackRequestedPackets;
	stats_.pliMessages += observed.pliMessages;
	stats_.firMessages += observed.firMessages;
	stats_.receiverReports += observed.receiverReports;
	stats_.reportBlocks += observed.reportBlocks;
	stats_.maxFractionLost = std::max(stats_.maxFractionLost, observed.maxFractionLost);
	stats_.maxCumulativeLost = std::max(stats_.maxCumulativeLost, observed.maxCumulativeLost);
	stats_.maxJitterTicks = std::max(stats_.maxJitterTicks, observed.maxJitterTicks);
	stats_.maxRttMs = std::max(stats_.maxRttMs, observed.maxRttMs);
	stats_.rembMessages += observed.rembMessages;
	if (observed.minRembBitrateBps > 0) {
		if (stats_.minRembBitrateBps == 0) {
			stats_.minRembBitrateBps = observed.minRembBitrateBps;
		} else {
			stats_.minRembBitrateBps = std::min(stats_.minRembBitrateBps, observed.minRembBitrateBps);
		}
		stats_.maxRembBitrateBps = std::max(stats_.maxRembBitrateBps, observed.maxRembBitrateBps);
	}
	if (observedRemb) {
		latestRemb_ = observedRemb;
	}
}

void RtcpFeedbackTracker::noteNackCacheResult(bool hit)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (hit) {
		++stats_.nackCacheHits;
	} else {
		++stats_.nackCacheMisses;
	}
}

void RtcpFeedbackTracker::noteRetransmissionQueued(bool queued)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (queued) {
		++stats_.retransmissionsQueued;
	} else {
		++stats_.retransmissionsDropped;
	}
}

void RtcpFeedbackTracker::noteRetransmissionCompleted(bool sent)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (sent) {
		++stats_.retransmissionsSent;
	} else {
		++stats_.retransmissionSendFailures;
	}
}

void RtcpFeedbackTracker::noteRetransmissionExpired()
{
	std::lock_guard<std::mutex> lock(mutex_);
	++stats_.retransmissionsExpired;
}

RtcpFeedbackStats RtcpFeedbackTracker::snapshot() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return stats_;
}

RtcpFeedbackStats RtcpFeedbackTracker::take()
{
	std::lock_guard<std::mutex> lock(mutex_);
	const RtcpFeedbackStats snapshot = stats_;
	stats_ = {};
	return snapshot;
}

std::optional<RtcpRembEstimate> RtcpFeedbackTracker::latestRemb(std::chrono::milliseconds maxAge) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!latestRemb_ || maxAge.count() <= 0) {
		return std::nullopt;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now > latestRemb_->observedAt && now - latestRemb_->observedAt > maxAge) {
		return std::nullopt;
	}
	return latestRemb_;
}

void RtcpFeedbackTracker::reset()
{
	std::lock_guard<std::mutex> lock(mutex_);
	stats_ = {};
	latestRemb_.reset();
}

uint32_t RtcpFeedbackTracker::currentCompactNtp()
{
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
	const auto remainder = now - seconds;
	const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(remainder).count();
	const uint64_t ntpSeconds = static_cast<uint64_t>(seconds.count()) + kNtpEpochOffsetSeconds;
	const uint64_t ntpFraction = (static_cast<uint64_t>(std::max<int64_t>(0, nanoseconds)) << 32U) / 1000000000ULL;
	return static_cast<uint32_t>(((ntpSeconds & 0xFFFFULL) << 16U) | ((ntpFraction >> 16U) & 0xFFFFULL));
}

} // namespace vdoninja
