/*
 * OBS VDO.Ninja Plugin
 * Helpers for pairing VP9 alpha frames with primary video frames
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct AVFrame;

namespace vdoninja
{

struct PendingPrimaryFrame {
	std::shared_ptr<AVFrame> frame;
	int width = 0;
	int height = 0;
	uint32_t rtpTimestamp = 0;
	uint64_t mediaEpoch = 0;
};

struct PendingAlphaFrame {
	std::vector<uint8_t> yData;
	int width = 0;
	int height = 0;
	int yLinesize = 0;
	uint32_t rtpTimestamp = 0;
	uint64_t mediaEpoch = 0;
};

struct AlphaFramePair {
	PendingPrimaryFrame primary;
	PendingAlphaFrame alpha;
	uint64_t generation = 0;
	uint64_t mediaEpoch = 0;

	bool dimensionsMatch() const { return primary.width == alpha.width && primary.height == alpha.height; }
};

// A callback captures the current epoch while its rtc::Track identity is
// validated. Every later assembly/decode/pair/output admission must recheck the
// token. Any two-track transition advances the epoch before clearing state,
// making an already-running callback deterministically stale.
class MediaEpochGate
{
public:
	uint64_t capture() const;
	uint64_t advance();
	bool isCurrent(uint64_t epoch) const;

private:
	std::atomic<uint64_t> epoch_{1};
};

// Primary and alpha may arrive in either order, but once either track selects a
// peer the other track must come from that same peer.
bool mediaTrackPeerCanOwn(const std::string &incomingPeer, const std::string &primaryPeer,
                          const std::string &alphaPeer);

template <typename T> bool clearSharedSlotIfMatches(std::shared_ptr<T> &slot, const std::shared_ptr<T> &expected)
{
	if (!expected || slot != expected) {
		return false;
	}
	slot.reset();
	return true;
}

// Maps RTP media time only after a frame has passed final serialized output
// ordering. Equal/older frames are rejected, including across uint32 wrap.
class RtpOutputTimestampMapper
{
public:
	std::optional<uint64_t> map(uint32_t rtpTimestamp, uint64_t nowNs, uint32_t clockRate = 90000);
	void reset();

private:
	bool initialized_ = false;
	uint32_t lastRtpTimestamp_ = 0;
	uint64_t extendedRtpTicks_ = 0;
	uint64_t baseTimestampNs_ = 0;
	uint64_t lastTimestampNs_ = 0;
};

struct AlphaFrameSyncResult {
	std::optional<AlphaFramePair> pair;
	size_t droppedPrimaryFrames = 0;
	size_t droppedAlphaFrames = 0;
	bool queued = false;
	bool rejectedIncomingFrame = false;
};

// Pairs the two independently delivered tracks by exact RTP timestamp. Queues
// are bounded both by frame count and by RTP-clock distance so a missing mate
// can never cause unbounded retention or reuse of an adjacent frame's mask.
class AlphaFrameSynchronizer
{
public:
	explicit AlphaFrameSynchronizer(size_t maxFrames = 8, uint32_t maxTimestampDelta = 9000);

	AlphaFrameSyncResult pushPrimary(PendingPrimaryFrame frame);
	AlphaFrameSyncResult pushAlpha(PendingAlphaFrame frame);
	void reset();

	size_t pendingPrimaryCount() const;
	size_t pendingAlphaCount() const;
	uint64_t generation() const;
	bool isCurrentGeneration(uint64_t generation) const;

private:
	bool observeTimestamp(uint32_t rtpTimestamp, AlphaFrameSyncResult &result);
	void pruneExpired(AlphaFrameSyncResult &result);
	void pruneEmitted(uint32_t rtpTimestamp, AlphaFrameSyncResult &result);
	void enforceCapacity(AlphaFrameSyncResult &result);

	size_t maxFrames_ = 0;
	uint32_t maxTimestampDelta_ = 0;
	std::deque<PendingPrimaryFrame> primaryFrames_;
	std::deque<PendingAlphaFrame> alphaFrames_;
	bool hasNewestTimestamp_ = false;
	uint32_t newestTimestamp_ = 0;
	bool hasLastEmittedTimestamp_ = false;
	uint32_t lastEmittedTimestamp_ = 0;
	uint64_t generation_ = 1;
};

bool isRtpTimestampBefore(uint32_t lhs, uint32_t rhs);
std::optional<uint32_t> resolveDecodedRtpTimestamp(int64_t framePts, int64_t bestEffortTimestamp);
bool scaleAlphaPlaneNearest(const std::vector<uint8_t> &src, int srcWidth, int srcHeight, int srcLinesize, int dstWidth,
                            int dstHeight, std::vector<uint8_t> &dst);

} // namespace vdoninja
