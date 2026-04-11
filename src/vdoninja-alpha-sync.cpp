/*
 * OBS VDO.Ninja Plugin
 * Helpers for pairing VP9 alpha frames with primary video frames
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-alpha-sync.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace vdoninja
{

bool isRtpTimestampBefore(uint32_t lhs, uint32_t rhs)
{
	return static_cast<int32_t>(lhs - rhs) < 0;
}

void upsertPendingAlphaFrame(std::deque<PendingAlphaFrame> &frames, PendingAlphaFrame frame, size_t maxFrames)
{
	auto existing = std::find_if(frames.begin(), frames.end(), [&frame](const PendingAlphaFrame &pendingFrame) {
		return pendingFrame.rtpTimestamp == frame.rtpTimestamp;
	});
	if (existing != frames.end()) {
		*existing = std::move(frame);
	} else {
		frames.push_back(std::move(frame));
	}

	while (frames.size() > maxFrames) {
		frames.pop_front();
	}
}

ConsumePendingAlphaResult consumePendingAlphaFrame(std::deque<PendingAlphaFrame> &frames, uint32_t rtpTimestamp,
                                                   int expectedWidth, int expectedHeight)
{
	ConsumePendingAlphaResult result;

	auto match = std::find_if(frames.begin(), frames.end(), [rtpTimestamp](const PendingAlphaFrame &pendingFrame) {
		return pendingFrame.rtpTimestamp == rtpTimestamp;
	});
	if (match != frames.end()) {
		result.hasMatch = true;
		result.width = match->width;
		result.height = match->height;
		result.yLinesize = match->yLinesize;
		result.dimensionsMatch = (match->width == expectedWidth && match->height == expectedHeight);
		if (result.dimensionsMatch) {
			result.yData = std::move(match->yData);
		}
		frames.erase(frames.begin(), std::next(match));
		return result;
	}

	auto staleEnd = std::find_if(frames.begin(), frames.end(), [rtpTimestamp](const PendingAlphaFrame &pendingFrame) {
		return !isRtpTimestampBefore(pendingFrame.rtpTimestamp, rtpTimestamp);
	});
	if (staleEnd != frames.begin()) {
		frames.erase(frames.begin(), staleEnd);
	}

	result.futureFramePending = !frames.empty();
	return result;
}

} // namespace vdoninja
