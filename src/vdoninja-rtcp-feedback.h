/*
 * OBS VDO.Ninja Plugin
 * Passive RTCP feedback telemetry
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace vdoninja
{

struct RtcpFeedbackStats {
	uint64_t compoundPackets = 0;
	uint64_t malformedPackets = 0;
	uint64_t nackMessages = 0;
	uint64_t nackRequestedPackets = 0;
	uint64_t pliMessages = 0;
	uint64_t firMessages = 0;
	uint64_t receiverReports = 0;
	uint64_t reportBlocks = 0;
	uint8_t maxFractionLost = 0;
	int32_t maxCumulativeLost = 0;
	uint32_t maxJitterTicks = 0;
	uint64_t maxRttMs = 0;
	uint64_t nackCacheHits = 0;
	uint64_t nackCacheMisses = 0;
	uint64_t retransmissionsQueued = 0;
	uint64_t retransmissionsSent = 0;
	uint64_t retransmissionsDropped = 0;
	uint64_t retransmissionsExpired = 0;
	uint64_t retransmissionSendFailures = 0;
	uint64_t rembMessages = 0;
	uint64_t minRembBitrateBps = 0;
	uint64_t maxRembBitrateBps = 0;
};

struct RtcpRembEstimate {
	uint64_t bitrateBitsPerSecond = 0;
	std::chrono::steady_clock::time_point observedAt;
};

// Parses only the RTCP fields needed for diagnostics. It never modifies media,
// sends feedback, or participates in recovery decisions.
class RtcpFeedbackTracker
{
public:
	explicit RtcpFeedbackTracker(uint32_t mediaSsrc = 0);

	void observe(const uint8_t *data, size_t size, uint32_t compactNtpNow = 0);
	void noteNackCacheResult(bool hit);
	void noteRetransmissionQueued(bool queued);
	void noteRetransmissionCompleted(bool sent);
	void noteRetransmissionExpired();
	RtcpFeedbackStats snapshot() const;
	RtcpFeedbackStats take();
	std::optional<RtcpRembEstimate> latestRemb(std::chrono::milliseconds maxAge) const;
	void reset();

	static uint32_t currentCompactNtp();

private:
	bool matchesMediaSsrc(uint32_t mediaSsrc) const noexcept;

	const uint32_t mediaSsrc_;
	mutable std::mutex mutex_;
	RtcpFeedbackStats stats_;
	std::optional<RtcpRembEstimate> latestRemb_;
};

} // namespace vdoninja
