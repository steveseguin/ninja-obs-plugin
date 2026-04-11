/*
 * Unit tests for VP9 alpha frame timestamp pairing helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-alpha-sync.h"

using namespace vdoninja;

namespace
{

PendingAlphaFrame makeFrame(uint32_t rtpTimestamp, int width = 640, int height = 360, int yLinesize = 640,
                            uint8_t fill = 0x7F)
{
	PendingAlphaFrame frame;
	frame.rtpTimestamp = rtpTimestamp;
	frame.width = width;
	frame.height = height;
	frame.yLinesize = yLinesize;
	frame.yData.assign(static_cast<size_t>(yLinesize) * static_cast<size_t>(height), fill);
	return frame;
}

} // namespace

TEST(AlphaSyncTest, RtpTimestampOrderingHandlesWrapAround)
{
	EXPECT_TRUE(isRtpTimestampBefore(0xFFFFFFF0u, 0x00000010u));
	EXPECT_FALSE(isRtpTimestampBefore(0x00000010u, 0xFFFFFFF0u));
}

TEST(AlphaSyncTest, UpsertReplacesExistingTimestamp)
{
	std::deque<PendingAlphaFrame> frames;
	upsertPendingAlphaFrame(frames, makeFrame(1000, 640, 360, 640, 0x11));
	upsertPendingAlphaFrame(frames, makeFrame(1000, 640, 360, 640, 0x99));

	ASSERT_EQ(frames.size(), 1u);
	EXPECT_EQ(frames.front().rtpTimestamp, 1000u);
	ASSERT_FALSE(frames.front().yData.empty());
	EXPECT_EQ(frames.front().yData.front(), 0x99);
}

TEST(AlphaSyncTest, UpsertPrunesOldestFramesAtCap)
{
	std::deque<PendingAlphaFrame> frames;
	for (uint32_t ts = 1; ts <= 10; ++ts) {
		upsertPendingAlphaFrame(frames, makeFrame(ts), 8);
	}

	ASSERT_EQ(frames.size(), 8u);
	EXPECT_EQ(frames.front().rtpTimestamp, 3u);
	EXPECT_EQ(frames.back().rtpTimestamp, 10u);
}

TEST(AlphaSyncTest, ConsumeMatchesTimestampAndErasesOlderFrames)
{
	std::deque<PendingAlphaFrame> frames;
	upsertPendingAlphaFrame(frames, makeFrame(100));
	upsertPendingAlphaFrame(frames, makeFrame(200, 640, 360, 672, 0x55));
	upsertPendingAlphaFrame(frames, makeFrame(300));

	const auto result = consumePendingAlphaFrame(frames, 200, 640, 360);

	EXPECT_TRUE(result.hasMatch);
	EXPECT_TRUE(result.dimensionsMatch);
	EXPECT_FALSE(result.futureFramePending);
	EXPECT_EQ(result.width, 640);
	EXPECT_EQ(result.height, 360);
	EXPECT_EQ(result.yLinesize, 672);
	ASSERT_FALSE(result.yData.empty());
	EXPECT_EQ(result.yData.front(), 0x55);
	ASSERT_EQ(frames.size(), 1u);
	EXPECT_EQ(frames.front().rtpTimestamp, 300u);
}

TEST(AlphaSyncTest, ConsumeReportsDimensionMismatchAndDropsMatchedFrame)
{
	std::deque<PendingAlphaFrame> frames;
	upsertPendingAlphaFrame(frames, makeFrame(100, 800, 600));
	upsertPendingAlphaFrame(frames, makeFrame(200));

	const auto result = consumePendingAlphaFrame(frames, 100, 640, 360);

	EXPECT_TRUE(result.hasMatch);
	EXPECT_FALSE(result.dimensionsMatch);
	EXPECT_EQ(result.width, 800);
	EXPECT_EQ(result.height, 600);
	EXPECT_TRUE(result.yData.empty());
	ASSERT_EQ(frames.size(), 1u);
	EXPECT_EQ(frames.front().rtpTimestamp, 200u);
}

TEST(AlphaSyncTest, ConsumeDropsOnlyStaleFramesWhenNoExactTimestampMatch)
{
	std::deque<PendingAlphaFrame> frames;
	upsertPendingAlphaFrame(frames, makeFrame(100));
	upsertPendingAlphaFrame(frames, makeFrame(200));
	upsertPendingAlphaFrame(frames, makeFrame(400));

	const auto result = consumePendingAlphaFrame(frames, 300, 640, 360);

	EXPECT_FALSE(result.hasMatch);
	EXPECT_FALSE(result.dimensionsMatch);
	EXPECT_TRUE(result.futureFramePending);
	ASSERT_EQ(frames.size(), 1u);
	EXPECT_EQ(frames.front().rtpTimestamp, 400u);
}

TEST(AlphaSyncTest, ConsumeLeavesQueueEmptyWhenEverythingIsStale)
{
	std::deque<PendingAlphaFrame> frames;
	upsertPendingAlphaFrame(frames, makeFrame(100));
	upsertPendingAlphaFrame(frames, makeFrame(200));

	const auto result = consumePendingAlphaFrame(frames, 300, 640, 360);

	EXPECT_FALSE(result.hasMatch);
	EXPECT_FALSE(result.futureFramePending);
	EXPECT_TRUE(frames.empty());
}
