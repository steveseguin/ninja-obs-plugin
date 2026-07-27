/*
 * OBS VDO.Ninja Plugin
 * Bounded RTP retransmission cache and RTCP NACK parsing
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-rtp-repair.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

namespace vdoninja
{

namespace
{

constexpr uint8_t kRtpVersion = 2;
constexpr uint8_t kTransportFeedbackPayloadType = 205;
constexpr uint8_t kNackFeedbackMessageType = 1;
constexpr size_t kMinimumRtpHeaderBytes = 12;
constexpr size_t kRtcpHeaderBytes = 4;
constexpr size_t kFeedbackHeaderBytes = 12;
constexpr size_t kMaximumNackRequestsPerCompoundPacket = 4096;

uint16_t readU16(const uint8_t *data)
{
	return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]));
}

uint32_t readU32(const uint8_t *data)
{
	return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
	       (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

} // namespace

std::vector<uint16_t> parseRtcpNackRequests(const uint8_t *data, size_t size, uint32_t mediaSsrc, bool *malformed)
{
	if (malformed) {
		*malformed = false;
	}
	std::vector<uint16_t> requested;
	if (!data || size < kRtcpHeaderBytes) {
		if (malformed) {
			*malformed = true;
		}
		return requested;
	}

	std::unordered_set<uint16_t> seen;
	size_t offset = 0;
	while (offset + kRtcpHeaderBytes <= size) {
		const uint8_t first = data[offset];
		const uint8_t version = first >> 6;
		const uint8_t feedbackMessageType = first & 0x1FU;
		const uint8_t payloadType = data[offset + 1];
		const size_t packetBytes = (static_cast<size_t>(readU16(data + offset + 2)) + 1U) * 4U;
		if (version != kRtpVersion || packetBytes < kRtcpHeaderBytes || packetBytes > size - offset) {
			if (malformed) {
				*malformed = true;
			}
			return requested;
		}

		const uint8_t *packet = data + offset;
		size_t contentBytes = packetBytes;
		if ((first & 0x20U) != 0) {
			const size_t paddingBytes = packet[packetBytes - 1];
			if (offset + packetBytes != size || paddingBytes == 0 || paddingBytes > packetBytes - kRtcpHeaderBytes) {
				if (malformed) {
					*malformed = true;
				}
				return requested;
			}
			contentBytes -= paddingBytes;
		}
		if (payloadType == kTransportFeedbackPayloadType && feedbackMessageType == kNackFeedbackMessageType) {
			if (contentBytes < kFeedbackHeaderBytes || (contentBytes - kFeedbackHeaderBytes) % 4U != 0) {
				if (malformed) {
					*malformed = true;
				}
				return requested;
			}
			if (mediaSsrc == 0 || readU32(packet + 8) == mediaSsrc) {
				for (size_t fieldOffset = kFeedbackHeaderBytes; fieldOffset + 4U <= contentBytes; fieldOffset += 4U) {
					const uint16_t packetId = readU16(packet + fieldOffset);
					const uint16_t bitmask = readU16(packet + fieldOffset + 2);
					if (seen.insert(packetId).second) {
						requested.push_back(packetId);
					}
					for (uint16_t bit = 0; bit < 16; ++bit) {
						if ((bitmask & static_cast<uint16_t>(1U << bit)) == 0) {
							continue;
						}
						const uint16_t sequenceNumber = static_cast<uint16_t>(packetId + bit + 1U);
						if (seen.insert(sequenceNumber).second) {
							requested.push_back(sequenceNumber);
						}
					}
					if (requested.size() >= kMaximumNackRequestsPerCompoundPacket) {
						return requested;
					}
				}
			}
		}
		offset += packetBytes;
	}

	if (offset != size && malformed) {
		*malformed = true;
	}
	return requested;
}

RtpRetransmissionCache::RtpRetransmissionCache(size_t maxPackets, size_t maxBytes, std::chrono::milliseconds maxAge)
    : maxPackets_(maxPackets), maxBytes_(maxBytes), maxAge_(maxAge)
{
	if (maxPackets_ == 0 || maxBytes_ == 0 || maxAge_.count() <= 0) {
		throw std::invalid_argument("RTP retransmission cache limits must be positive");
	}
	bySequence_.reserve(maxPackets_);
}

bool RtpRetransmissionCache::store(const uint8_t *data, size_t size, Clock::time_point now)
{
	if (!data || size < kMinimumRtpHeaderBytes || (data[0] >> 6) != kRtpVersion || size > maxBytes_) {
		return false;
	}

	auto entry = std::make_shared<Entry>();
	entry->sequenceNumber = readU16(data + 2);
	entry->packet.resize(size);
	std::memcpy(entry->packet.data(), data, size);
	entry->storedAt = now;

	std::lock_guard<std::mutex> lock(mutex_);
	pruneLocked(now);
	const auto existing = bySequence_.find(entry->sequenceNumber);
	if (existing != bySequence_.end()) {
		// Paced packet duplication intentionally sends the same RTP sequence
		// number again. Replace the cached copy without letting duplicates
		// consume the unique-packet history or byte budget twice.
		bytes_ -= existing->second->packet.size();
		existing->second->packet.clear();
	}
	entries_.push_back(entry);
	bySequence_[entry->sequenceNumber] = entry;
	bytes_ += size;
	pruneLocked(now);
	return bySequence_.find(entry->sequenceNumber) != bySequence_.end() && bySequence_[entry->sequenceNumber] == entry;
}

std::optional<RtpRetransmissionCache::Packet> RtpRetransmissionCache::find(uint16_t sequenceNumber,
                                                                           Clock::time_point now)
{
	std::lock_guard<std::mutex> lock(mutex_);
	pruneLocked(now);
	const auto found = bySequence_.find(sequenceNumber);
	if (found == bySequence_.end()) {
		return std::nullopt;
	}
	return found->second->packet;
}

size_t RtpRetransmissionCache::size() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return bySequence_.size();
}

size_t RtpRetransmissionCache::bytes() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return bytes_;
}

void RtpRetransmissionCache::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	entries_.clear();
	bySequence_.clear();
	bytes_ = 0;
}

void RtpRetransmissionCache::pruneLocked(Clock::time_point now)
{
	while (!entries_.empty()) {
		const auto &oldest = entries_.front();
		const bool expired = now > oldest->storedAt && now - oldest->storedAt > maxAge_;
		if (!expired && bySequence_.size() <= maxPackets_ && bytes_ <= maxBytes_) {
			break;
		}

		bytes_ -= oldest->packet.size();
		const auto found = bySequence_.find(oldest->sequenceNumber);
		if (found != bySequence_.end() && found->second == oldest) {
			bySequence_.erase(found);
		}
		entries_.pop_front();
	}
}

} // namespace vdoninja
