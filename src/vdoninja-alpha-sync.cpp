/*
 * OBS VDO.Ninja Plugin
 * Helpers for pairing VP9 alpha frames with primary video frames
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-alpha-sync.h"

#include <algorithm>
#include <utility>

namespace vdoninja
{

bool isRtpTimestampBefore(uint32_t lhs, uint32_t rhs)
{
	return static_cast<int32_t>(lhs - rhs) < 0;
}

uint64_t MediaEpochGate::capture() const
{
	return epoch_.load(std::memory_order_acquire);
}

uint64_t MediaEpochGate::advance()
{
	return epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool MediaEpochGate::isCurrent(uint64_t epoch) const
{
	return epoch != 0 && capture() == epoch;
}

bool mediaTrackPeerCanOwn(const std::string &incomingPeer, const std::string &primaryPeer, const std::string &alphaPeer)
{
	return !incomingPeer.empty() && (primaryPeer.empty() || primaryPeer == incomingPeer) &&
	       (alphaPeer.empty() || alphaPeer == incomingPeer);
}

std::optional<uint64_t> RtpOutputTimestampMapper::map(uint32_t rtpTimestamp, uint64_t nowNs)
{
	if (!initialized_) {
		initialized_ = true;
		lastRtpTimestamp_ = rtpTimestamp;
		extendedRtpTicks_ = 0;
		baseTimestampNs_ = nowNs;
		lastTimestampNs_ = nowNs;
		return nowNs;
	}

	if (rtpTimestamp == lastRtpTimestamp_ || isRtpTimestampBefore(rtpTimestamp, lastRtpTimestamp_)) {
		return std::nullopt;
	}

	extendedRtpTicks_ += static_cast<uint32_t>(rtpTimestamp - lastRtpTimestamp_);
	uint64_t mapped = baseTimestampNs_ + (extendedRtpTicks_ * 1000000000ULL) / 90000ULL;
	if (mapped <= lastTimestampNs_) {
		mapped = lastTimestampNs_ + 1;
	}
	lastRtpTimestamp_ = rtpTimestamp;
	lastTimestampNs_ = mapped;
	return mapped;
}

void RtpOutputTimestampMapper::reset()
{
	initialized_ = false;
	lastRtpTimestamp_ = 0;
	extendedRtpTicks_ = 0;
	baseTimestampNs_ = 0;
	lastTimestampNs_ = 0;
}

std::optional<uint32_t> resolveDecodedRtpTimestamp(int64_t framePts, int64_t bestEffortTimestamp)
{
	constexpr int64_t kMaxRtpTimestamp = static_cast<int64_t>(UINT32_MAX);
	if (framePts >= 0 && framePts <= kMaxRtpTimestamp) {
		return static_cast<uint32_t>(framePts);
	}
	if (bestEffortTimestamp >= 0 && bestEffortTimestamp <= kMaxRtpTimestamp) {
		return static_cast<uint32_t>(bestEffortTimestamp);
	}
	return std::nullopt;
}

bool scaleAlphaPlaneNearest(const std::vector<uint8_t> &src, int srcWidth, int srcHeight, int srcLinesize, int dstWidth,
                            int dstHeight, std::vector<uint8_t> &dst)
{
	if (srcWidth <= 0 || srcHeight <= 0 || srcLinesize < srcWidth || dstWidth <= 0 || dstHeight <= 0) {
		return false;
	}
	if (src.size() < static_cast<size_t>(srcLinesize) * static_cast<size_t>(srcHeight)) {
		return false;
	}

	dst.resize(static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight));
	for (int y = 0; y < dstHeight; ++y) {
		const int srcY = std::min(srcHeight - 1, (y * srcHeight) / dstHeight);
		const uint8_t *srcRow = src.data() + static_cast<size_t>(srcY) * static_cast<size_t>(srcLinesize);
		uint8_t *dstRow = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(dstWidth);
		for (int x = 0; x < dstWidth; ++x) {
			const int srcX = std::min(srcWidth - 1, (x * srcWidth) / dstWidth);
			dstRow[x] = srcRow[srcX];
		}
	}
	return true;
}

AlphaFrameSynchronizer::AlphaFrameSynchronizer(size_t maxFrames, uint32_t maxTimestampDelta)
    : maxFrames_(maxFrames), maxTimestampDelta_(maxTimestampDelta)
{
}

bool AlphaFrameSynchronizer::observeTimestamp(uint32_t rtpTimestamp, AlphaFrameSyncResult &result)
{
	if (hasLastEmittedTimestamp_ &&
	    (rtpTimestamp == lastEmittedTimestamp_ || isRtpTimestampBefore(rtpTimestamp, lastEmittedTimestamp_))) {
		result.rejectedIncomingFrame = true;
		return false;
	}

	if (!hasNewestTimestamp_) {
		hasNewestTimestamp_ = true;
		newestTimestamp_ = rtpTimestamp;
	} else if (isRtpTimestampBefore(newestTimestamp_, rtpTimestamp)) {
		newestTimestamp_ = rtpTimestamp;
	} else if (isRtpTimestampBefore(rtpTimestamp, newestTimestamp_) &&
	           static_cast<uint32_t>(newestTimestamp_ - rtpTimestamp) > maxTimestampDelta_) {
		result.rejectedIncomingFrame = true;
		pruneExpired(result);
		return false;
	}

	pruneExpired(result);
	return true;
}

void AlphaFrameSynchronizer::pruneExpired(AlphaFrameSyncResult &result)
{
	if (!hasNewestTimestamp_) {
		return;
	}

	auto primaryEnd = std::remove_if(primaryFrames_.begin(), primaryFrames_.end(), [this](const auto &frame) {
		return isRtpTimestampBefore(frame.rtpTimestamp, newestTimestamp_) &&
		       static_cast<uint32_t>(newestTimestamp_ - frame.rtpTimestamp) > maxTimestampDelta_;
	});
	result.droppedPrimaryFrames += static_cast<size_t>(std::distance(primaryEnd, primaryFrames_.end()));
	primaryFrames_.erase(primaryEnd, primaryFrames_.end());

	auto alphaEnd = std::remove_if(alphaFrames_.begin(), alphaFrames_.end(), [this](const auto &frame) {
		return isRtpTimestampBefore(frame.rtpTimestamp, newestTimestamp_) &&
		       static_cast<uint32_t>(newestTimestamp_ - frame.rtpTimestamp) > maxTimestampDelta_;
	});
	result.droppedAlphaFrames += static_cast<size_t>(std::distance(alphaEnd, alphaFrames_.end()));
	alphaFrames_.erase(alphaEnd, alphaFrames_.end());
}

void AlphaFrameSynchronizer::pruneEmitted(uint32_t rtpTimestamp, AlphaFrameSyncResult &result)
{
	auto primaryEnd = std::remove_if(primaryFrames_.begin(), primaryFrames_.end(), [rtpTimestamp](const auto &frame) {
		return frame.rtpTimestamp == rtpTimestamp || isRtpTimestampBefore(frame.rtpTimestamp, rtpTimestamp);
	});
	result.droppedPrimaryFrames += static_cast<size_t>(std::distance(primaryEnd, primaryFrames_.end()));
	primaryFrames_.erase(primaryEnd, primaryFrames_.end());

	auto alphaEnd = std::remove_if(alphaFrames_.begin(), alphaFrames_.end(), [rtpTimestamp](const auto &frame) {
		return frame.rtpTimestamp == rtpTimestamp || isRtpTimestampBefore(frame.rtpTimestamp, rtpTimestamp);
	});
	result.droppedAlphaFrames += static_cast<size_t>(std::distance(alphaEnd, alphaFrames_.end()));
	alphaFrames_.erase(alphaEnd, alphaFrames_.end());
}

void AlphaFrameSynchronizer::enforceCapacity(AlphaFrameSyncResult &result)
{
	while (primaryFrames_.size() > maxFrames_) {
		primaryFrames_.pop_front();
		result.droppedPrimaryFrames++;
	}
	while (alphaFrames_.size() > maxFrames_) {
		alphaFrames_.pop_front();
		result.droppedAlphaFrames++;
	}
}

AlphaFrameSyncResult AlphaFrameSynchronizer::pushPrimary(PendingPrimaryFrame frame)
{
	AlphaFrameSyncResult result;
	if (!observeTimestamp(frame.rtpTimestamp, result)) {
		return result;
	}

	auto alphaMatch = std::find_if(alphaFrames_.begin(), alphaFrames_.end(), [&frame](const auto &alphaFrame) {
		return alphaFrame.rtpTimestamp == frame.rtpTimestamp && alphaFrame.mediaEpoch == frame.mediaEpoch;
	});
	if (alphaMatch != alphaFrames_.end()) {
		AlphaFramePair pair;
		pair.primary = std::move(frame);
		pair.alpha = std::move(*alphaMatch);
		pair.generation = generation_;
		pair.mediaEpoch = pair.primary.mediaEpoch;
		alphaFrames_.erase(alphaMatch);
		lastEmittedTimestamp_ = pair.primary.rtpTimestamp;
		hasLastEmittedTimestamp_ = true;
		pruneEmitted(lastEmittedTimestamp_, result);
		result.pair = std::move(pair);
		return result;
	}

	auto existing = std::find_if(primaryFrames_.begin(), primaryFrames_.end(), [&frame](const auto &primaryFrame) {
		return primaryFrame.rtpTimestamp == frame.rtpTimestamp;
	});
	if (existing != primaryFrames_.end()) {
		*existing = std::move(frame);
	} else {
		primaryFrames_.push_back(std::move(frame));
	}
	result.queued = true;
	enforceCapacity(result);
	return result;
}

AlphaFrameSyncResult AlphaFrameSynchronizer::pushAlpha(PendingAlphaFrame frame)
{
	AlphaFrameSyncResult result;
	if (!observeTimestamp(frame.rtpTimestamp, result)) {
		return result;
	}

	auto primaryMatch = std::find_if(primaryFrames_.begin(), primaryFrames_.end(), [&frame](const auto &primaryFrame) {
		return primaryFrame.rtpTimestamp == frame.rtpTimestamp && primaryFrame.mediaEpoch == frame.mediaEpoch;
	});
	if (primaryMatch != primaryFrames_.end()) {
		AlphaFramePair pair;
		pair.primary = std::move(*primaryMatch);
		pair.alpha = std::move(frame);
		pair.generation = generation_;
		pair.mediaEpoch = pair.primary.mediaEpoch;
		primaryFrames_.erase(primaryMatch);
		lastEmittedTimestamp_ = pair.primary.rtpTimestamp;
		hasLastEmittedTimestamp_ = true;
		pruneEmitted(lastEmittedTimestamp_, result);
		result.pair = std::move(pair);
		return result;
	}

	auto existing = std::find_if(alphaFrames_.begin(), alphaFrames_.end(), [&frame](const auto &alphaFrame) {
		return alphaFrame.rtpTimestamp == frame.rtpTimestamp;
	});
	if (existing != alphaFrames_.end()) {
		*existing = std::move(frame);
	} else {
		alphaFrames_.push_back(std::move(frame));
	}
	result.queued = true;
	enforceCapacity(result);
	return result;
}

void AlphaFrameSynchronizer::reset()
{
	primaryFrames_.clear();
	alphaFrames_.clear();
	hasNewestTimestamp_ = false;
	newestTimestamp_ = 0;
	hasLastEmittedTimestamp_ = false;
	lastEmittedTimestamp_ = 0;
	generation_++;
}

size_t AlphaFrameSynchronizer::pendingPrimaryCount() const
{
	return primaryFrames_.size();
}

size_t AlphaFrameSynchronizer::pendingAlphaCount() const
{
	return alphaFrames_.size();
}

uint64_t AlphaFrameSynchronizer::generation() const
{
	return generation_;
}

bool AlphaFrameSynchronizer::isCurrentGeneration(uint64_t generation) const
{
	return generation_ == generation;
}

} // namespace vdoninja
