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

TEST(RedPayloadTest, SkipsAllRedundantBlocksBeforePrimary)
{
	const std::vector<uint8_t> packet{0xE0, 0, 0, 2, 0xE0, 0, 0, 3, 96, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x67, 0x12};
	const auto primary = extractRedPrimaryPayload(packet.data(), packet.size());
	ASSERT_TRUE(primary);
	EXPECT_EQ(*primary, (std::vector<uint8_t>{0x67, 0x12}));
}

TEST(RedPayloadTest, RejectsTruncatedRedundantDataAndMissingPrimary)
{
	for (const auto &packet : std::vector<std::vector<uint8_t>>{{0xE0, 0, 0, 3, 96, 0xAA}, {0xE0, 0, 0, 1, 96, 0xAA}}) {
		EXPECT_FALSE(extractRedPrimaryPayload(packet.data(), packet.size()));
	}
}

TEST(RedPayloadTest, PreservesPrimaryOnlyPacketsAndRejectsIncompleteHeaders)
{
	const std::vector<uint8_t> packet{96, 0x67, 0x12};
	EXPECT_EQ(extractRedPrimaryPayload(packet.data(), packet.size()), (std::vector<uint8_t>{0x67, 0x12}));
	for (const auto &invalid : std::vector<std::vector<uint8_t>>{{}, {96}, {0xE0}, {0xE0, 0, 0}, {0xE0, 0, 0, 0}}) {
		EXPECT_FALSE(extractRedPrimaryPayload(invalid.data(), invalid.size()));
	}
}

TEST(RtxPacketTest, PreservesExtensionsCsrcMarkerAndPadding)
{
	// One CSRC, one extension word, OSN, media, and four padding bytes.
	std::vector<uint8_t> packet{0xB1, 0xE1, 0, 9, 1,    2,    3, 4, 5,    6,    7,    8,    9, 10, 11, 12,
	                            0xBE, 0xDE, 0, 1, 0x10, 0x42, 0, 0, 0x12, 0x34, 0x67, 0x89, 0, 0,  0,  4};
	auto expected = packet;
	expected[1] = 0xE0;
	expected[2] = 0x12;
	expected[3] = 0x34;
	expected.erase(expected.begin() + 24, expected.begin() + 26);
	const auto normalizedSize = normalizeRtxPacket(packet.data(), packet.size(), 96);
	ASSERT_TRUE(normalizedSize);
	packet.resize(*normalizedSize);
	EXPECT_EQ(packet, expected);
	const auto payload = parseRtpPayloadView(packet.data(), packet.size());
	ASSERT_TRUE(payload);
	EXPECT_EQ(payload->offset, 24u);
	EXPECT_EQ(payload->size, 2u);
}

TEST(RtxPacketTest, NormalizesOrdinaryPacket)
{
	std::vector<uint8_t> packet{0x80, 97, 0, 9, 1, 2, 3, 4, 5, 6, 7, 8, 0x12, 0x34, 0x67};
	ASSERT_EQ(normalizeRtxPacket(packet.data(), packet.size(), 96), 13u);
	packet.resize(13);
	EXPECT_EQ(packet, (std::vector<uint8_t>{0x80, 96, 0x12, 0x34, 1, 2, 3, 4, 5, 6, 7, 8, 0x67}));
}

TEST(RtxPacketTest, RejectsMalformedPacketsWithoutMutation)
{
	std::vector<std::vector<uint8_t>> packets;
	for (size_t size = 0; size < 15; ++size) {
		std::vector<uint8_t> packet(size, 0);
		if (size)
			packet[0] = 0x80;
		packets.push_back(packet);
	}
	for (uint8_t first : {0x90, 0x9F, 0x81, 0x40}) {
		std::vector<uint8_t> packet(15, 0);
		packet[0] = first;
		packets.push_back(packet);
	}
	// Truncated extension body; padding claiming the original sequence or all media.
	packets.push_back({0x90, 97, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xBE, 0xDE, 0, 2, 0, 0, 0});
	for (uint8_t padding : {0, 2, 3, 4, 255}) {
		std::vector<uint8_t> packet(16, 0);
		packet[0] = 0xA0;
		packet.back() = padding;
		packets.push_back(packet);
	}
	for (auto packet : packets) {
		const auto original = packet;
		EXPECT_FALSE(normalizeRtxPacket(packet.data(), packet.size(), 96));
		EXPECT_EQ(packet, original);
	}
	EXPECT_FALSE(normalizeRtxPacket(nullptr, 20, 96));
}
