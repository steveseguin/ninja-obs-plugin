/*
 * OBS VDO.Ninja Plugin
 * Bounded RTP retransmission cache and RTCP NACK parsing
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace vdoninja
{

std::vector<uint16_t> parseRtcpNackRequests(const uint8_t *data, size_t size, uint32_t mediaSsrc,
                                            bool *malformed = nullptr);

class RtpRetransmissionCache
{
public:
	using Packet = std::vector<std::byte>;
	using Clock = std::chrono::steady_clock;

	RtpRetransmissionCache(size_t maxPackets = 2048, size_t maxBytes = 4U * 1024U * 1024U,
	                       std::chrono::milliseconds maxAge = std::chrono::milliseconds(2000));

	bool store(const uint8_t *data, size_t size, Clock::time_point now = Clock::now());
	std::optional<Packet> find(uint16_t sequenceNumber, Clock::time_point now = Clock::now());
	size_t size() const;
	size_t bytes() const;
	void clear();

private:
	struct Entry {
		uint16_t sequenceNumber = 0;
		Packet packet;
		Clock::time_point storedAt;
	};

	void pruneLocked(Clock::time_point now);

	const size_t maxPackets_;
	const size_t maxBytes_;
	const std::chrono::milliseconds maxAge_;
	mutable std::mutex mutex_;
	std::deque<std::shared_ptr<Entry>> entries_;
	std::unordered_map<uint16_t, std::shared_ptr<Entry>> bySequence_;
	size_t bytes_ = 0;
};

} // namespace vdoninja
