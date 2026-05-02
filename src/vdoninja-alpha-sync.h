/*
 * OBS VDO.Ninja Plugin
 * Helpers for pairing VP9 alpha frames with primary video frames
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace vdoninja
{

struct PendingAlphaFrame {
	std::vector<uint8_t> yData;
	int width = 0;
	int height = 0;
	int yLinesize = 0;
	uint32_t rtpTimestamp = 0;
};

struct ConsumePendingAlphaResult {
	bool hasMatch = false;
	bool dimensionsMatch = false;
	bool futureFramePending = false;
	int width = 0;
	int height = 0;
	int yLinesize = 0;
	std::vector<uint8_t> yData;
};

bool isRtpTimestampBefore(uint32_t lhs, uint32_t rhs);
void upsertPendingAlphaFrame(std::deque<PendingAlphaFrame> &frames, PendingAlphaFrame frame, size_t maxFrames = 64);
ConsumePendingAlphaResult consumePendingAlphaFrame(std::deque<PendingAlphaFrame> &frames, uint32_t rtpTimestamp,
                                                   int expectedWidth, int expectedHeight);

} // namespace vdoninja
