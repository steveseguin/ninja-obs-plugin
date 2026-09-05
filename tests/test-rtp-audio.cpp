/*
 * Unit tests for published Opus RTP continuity and send accounting
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "vdoninja-rtp-send-tracker.h"
#include "vdoninja-rtp-utils.h"

using namespace vdoninja;

namespace
{

uint16_t packetSequence(const std::vector<uint8_t> &packet)
{
	return static_cast<uint16_t>((static_cast<uint16_t>(packet[2]) << 8) | packet[3]);
}

uint32_t packetTimestamp(const std::vector<uint8_t> &packet)
{
	return (static_cast<uint32_t>(packet[4]) << 24) | (static_cast<uint32_t>(packet[5]) << 16) |
	       (static_cast<uint32_t>(packet[6]) << 8) | packet[7];
}

uint32_t packetSsrc(const std::vector<uint8_t> &packet)
{
	return (static_cast<uint32_t>(packet[8]) << 24) | (static_cast<uint32_t>(packet[9]) << 16) |
	       (static_cast<uint32_t>(packet[10]) << 8) | packet[11];
}

} // namespace

TEST(OpusRtpPacketTest, BuildsExpectedHeaderAndPreservesPayload)
{
	const std::vector<uint8_t> payload{0xF8, 0xFF, 0xFE, 0x7A};
	const std::vector<uint8_t> packet =
	    buildOpusRtpPacket(payload.data(), payload.size(), 111, 0x1234, 0x89ABCDEF, 0x10203040);

	ASSERT_EQ(packet.size(), payload.size() + 12);
	EXPECT_EQ(packet[0], 0x80);
	EXPECT_EQ(packet[1], 111);
	EXPECT_EQ(packetSequence(packet), 0x1234);
	EXPECT_EQ(packetTimestamp(packet), 0x89ABCDEFu);
	EXPECT_EQ(packetSsrc(packet), 0x10203040u);
	EXPECT_EQ(std::vector<uint8_t>(packet.begin() + 12, packet.end()), payload);
}

TEST(OpusRtpPacketTest, RejectsPayloadSizesThatCannotFitWithTheHeader)
{
	const uint8_t payload = 0xF8;
	const size_t maxSize = std::vector<uint8_t>{}.max_size();
	EXPECT_TRUE(buildOpusRtpPacket(&payload, maxSize - 11, 111, 1, 960, 123).empty());
	EXPECT_TRUE(buildOpusRtpPacket(&payload, std::numeric_limits<size_t>::max(), 111, 1, 960, 123).empty());
}

TEST(OpusRtpPacketTest, PreservesEmptyPayloadAndRejectsNullNonemptyPayload)
{
	EXPECT_TRUE(buildOpusRtpPacket(nullptr, 1, 111, 1, 960, 123).empty());
	const auto empty = buildOpusRtpPacket(nullptr, 0, 111, 1, 960, 123);
	ASSERT_EQ(empty.size(), 12u);
	EXPECT_EQ(packetSequence(empty), 1u);
	EXPECT_EQ(packetTimestamp(empty), 960u);
}

TEST(OpusRtpPacketTest, GeneratesLongContinuousSequenceWithoutHeaderDrift)
{
	const std::vector<uint8_t> payload{0x78, 0x11, 0x22};
	RtpTimestampStepTracker continuity(960);
	constexpr uint16_t firstSequence = 65000;
	constexpr uint32_t firstTimestamp = 0xFFF00000u;

	for (uint32_t index = 0; index < 10000; ++index) {
		const uint16_t sequence = static_cast<uint16_t>(firstSequence + index);
		const uint32_t timestamp = firstTimestamp + index * 960u;
		const std::vector<uint8_t> packet =
		    buildOpusRtpPacket(payload.data(), payload.size(), 111, sequence, timestamp, 0x55667788);

		ASSERT_EQ(packetSequence(packet), sequence);
		ASSERT_EQ(packetTimestamp(packet), timestamp);
		continuity.observe(packetTimestamp(packet));
	}

	const RtpTimestampStepStats stats = continuity.takeInterval();
	EXPECT_EQ(stats.packets, 10000u);
	EXPECT_EQ(stats.largeSteps, 0u);
	EXPECT_EQ(stats.nonForwardSteps, 0u);
	EXPECT_EQ(stats.maxForwardStep, 960u);
}

TEST(RtpTimestampStepTrackerTest, DetectsLargeAndNonForwardAudioSteps)
{
	RtpTimestampStepTracker tracker(960);
	tracker.observe(1000);
	tracker.observe(1960);
	tracker.observe(4840);
	tracker.observe(4840);
	tracker.observe(4000);

	const RtpTimestampStepStats stats = tracker.takeInterval();
	EXPECT_EQ(stats.packets, 5u);
	EXPECT_EQ(stats.largeSteps, 1u);
	EXPECT_EQ(stats.nonForwardSteps, 2u);
	EXPECT_EQ(stats.maxForwardStep, 2880u);
}

TEST(RtpTimestampStepTrackerTest, TreatsTimestampWrapAsContinuous)
{
	RtpTimestampStepTracker tracker(960);
	tracker.observe(0xFFFFFC40u);
	tracker.observe(0);
	tracker.observe(960);

	const RtpTimestampStepStats stats = tracker.takeInterval();
	EXPECT_EQ(stats.packets, 3u);
	EXPECT_EQ(stats.largeSteps, 0u);
	EXPECT_EQ(stats.nonForwardSteps, 0u);
	EXPECT_EQ(stats.maxForwardStep, 960u);
}

TEST(RtpTimestampStepTrackerTest, IntervalResetPreservesCrossWindowContinuity)
{
	RtpTimestampStepTracker tracker(960);
	tracker.observe(0);
	EXPECT_EQ(tracker.takeInterval().packets, 1u);

	tracker.observe(1920);
	const RtpTimestampStepStats stats = tracker.takeInterval();
	EXPECT_EQ(stats.packets, 1u);
	EXPECT_EQ(stats.largeSteps, 1u);
	EXPECT_EQ(stats.maxForwardStep, 1920u);
}

TEST(RtpSendTrackerTest, CountsFalseReturnAsFailure)
{
	RtpSendTracker tracker;

	EXPECT_FALSE(tracker.send([]() { return false; }));
	const RtpSendStats stats = tracker.take();
	EXPECT_EQ(stats.sentPackets, 0u);
	EXPECT_EQ(stats.sendFailures, 1u);
}

TEST(RtpSendTrackerTest, CountsSuccessAndResetsInterval)
{
	RtpSendTracker tracker;

	EXPECT_TRUE(tracker.send([]() { return true; }));
	EXPECT_TRUE(tracker.send([]() { return true; }));
	const RtpSendStats first = tracker.take();
	EXPECT_EQ(first.sentPackets, 2u);
	EXPECT_EQ(first.sendFailures, 0u);

	const RtpSendStats second = tracker.take();
	EXPECT_EQ(second.sentPackets, 0u);
	EXPECT_EQ(second.sendFailures, 0u);
}

TEST(RtpSendTrackerTest, CountsExceptionBeforeRethrowing)
{
	RtpSendTracker tracker;

	EXPECT_THROW(tracker.send([]() -> bool { throw std::runtime_error("transport failed"); }), std::runtime_error);
	const RtpSendStats stats = tracker.take();
	EXPECT_EQ(stats.sentPackets, 0u);
	EXPECT_EQ(stats.sendFailures, 1u);
}
