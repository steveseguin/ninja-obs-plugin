/*
 * Unit tests for passive RTCP feedback telemetry
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "vdoninja-rtcp-feedback.h"

using namespace vdoninja;

namespace
{

void appendU16(std::vector<uint8_t> &packet, uint16_t value)
{
	packet.push_back(static_cast<uint8_t>(value >> 8));
	packet.push_back(static_cast<uint8_t>(value));
}

void appendU32(std::vector<uint8_t> &packet, uint32_t value)
{
	packet.push_back(static_cast<uint8_t>(value >> 24));
	packet.push_back(static_cast<uint8_t>(value >> 16));
	packet.push_back(static_cast<uint8_t>(value >> 8));
	packet.push_back(static_cast<uint8_t>(value));
}

void setRtcpLength(std::vector<uint8_t> &packet, size_t start)
{
	const size_t packetBytes = packet.size() - start;
	const uint16_t wordsMinusOne = static_cast<uint16_t>(packetBytes / 4U - 1U);
	packet[start + 2] = static_cast<uint8_t>(wordsMinusOne >> 8);
	packet[start + 3] = static_cast<uint8_t>(wordsMinusOne);
}

size_t appendHeader(std::vector<uint8_t> &packet, uint8_t count, uint8_t payloadType)
{
	const size_t start = packet.size();
	packet.push_back(static_cast<uint8_t>(0x80U | count));
	packet.push_back(payloadType);
	appendU16(packet, 0);
	return start;
}

std::vector<uint8_t> makeNack(uint32_t mediaSsrc, uint16_t pid, uint16_t bitmask)
{
	std::vector<uint8_t> packet;
	const size_t start = appendHeader(packet, 1, 205);
	appendU32(packet, 0x11111111);
	appendU32(packet, mediaSsrc);
	appendU16(packet, pid);
	appendU16(packet, bitmask);
	setRtcpLength(packet, start);
	return packet;
}

std::vector<uint8_t> makePli(uint32_t mediaSsrc)
{
	std::vector<uint8_t> packet;
	const size_t start = appendHeader(packet, 1, 206);
	appendU32(packet, 0x11111111);
	appendU32(packet, mediaSsrc);
	setRtcpLength(packet, start);
	return packet;
}

std::vector<uint8_t> makeReceiverReport(uint32_t mediaSsrc, uint8_t fractionLost, uint32_t cumulativeLost,
                                        uint32_t jitter, uint32_t lastSenderReport, uint32_t delay)
{
	std::vector<uint8_t> packet;
	const size_t start = appendHeader(packet, 1, 201);
	appendU32(packet, 0x11111111);
	appendU32(packet, mediaSsrc);
	packet.push_back(fractionLost);
	packet.push_back(static_cast<uint8_t>(cumulativeLost >> 16));
	packet.push_back(static_cast<uint8_t>(cumulativeLost >> 8));
	packet.push_back(static_cast<uint8_t>(cumulativeLost));
	appendU32(packet, 1234);
	appendU32(packet, jitter);
	appendU32(packet, lastSenderReport);
	appendU32(packet, delay);
	setRtcpLength(packet, start);
	return packet;
}

std::vector<uint8_t> makeRemb(uint32_t mediaSsrc, uint32_t mantissa, uint8_t exponent)
{
	std::vector<uint8_t> packet;
	const size_t start = appendHeader(packet, 15, 206);
	appendU32(packet, 0x11111111);
	appendU32(packet, 0);
	packet.insert(packet.end(), {'R', 'E', 'M', 'B'});
	packet.push_back(1);
	packet.push_back(static_cast<uint8_t>((exponent << 2U) | ((mantissa >> 16U) & 0x03U)));
	packet.push_back(static_cast<uint8_t>(mantissa >> 8U));
	packet.push_back(static_cast<uint8_t>(mantissa));
	appendU32(packet, mediaSsrc);
	setRtcpLength(packet, start);
	return packet;
}

void addFourBytePadding(std::vector<uint8_t> &packet)
{
	packet[0] |= 0x20U;
	packet.insert(packet.end(), {0, 0, 0, 4});
	setRtcpLength(packet, 0);
}

} // namespace

TEST(RtcpFeedbackTrackerTest, CountsRequestedPacketsFromNackBitmask)
{
	constexpr uint32_t mediaSsrc = 0x22222222;
	RtcpFeedbackTracker tracker(mediaSsrc);
	const auto packet = makeNack(mediaSsrc, 100, 0b1000000000000101);

	tracker.observe(packet.data(), packet.size());
	const RtcpFeedbackStats stats = tracker.snapshot();

	EXPECT_EQ(stats.nackMessages, 1u);
	EXPECT_EQ(stats.nackRequestedPackets, 4u);
	EXPECT_EQ(stats.malformedPackets, 0u);
}

TEST(RtcpFeedbackTrackerTest, ExcludesRtcpPaddingFromNackFields)
{
	constexpr uint32_t mediaSsrc = 0x22222222;
	auto packet = makeNack(mediaSsrc, 100, 0);
	addFourBytePadding(packet);
	RtcpFeedbackTracker tracker(mediaSsrc);

	tracker.observe(packet.data(), packet.size());
	const RtcpFeedbackStats stats = tracker.snapshot();

	EXPECT_EQ(stats.nackMessages, 1u);
	EXPECT_EQ(stats.nackRequestedPackets, 1u);
	EXPECT_EQ(stats.malformedPackets, 0u);
}

TEST(RtcpFeedbackTrackerTest, RejectsPaddingBeforeAnotherCompoundPacket)
{
	auto packet = makeNack(0x22222222, 100, 0);
	addFourBytePadding(packet);
	const auto pli = makePli(0x22222222);
	packet.insert(packet.end(), pli.begin(), pli.end());
	RtcpFeedbackTracker tracker(0x22222222);

	tracker.observe(packet.data(), packet.size());

	EXPECT_EQ(tracker.snapshot().malformedPackets, 1u);
}

TEST(RtcpFeedbackTrackerTest, IgnoresFeedbackForAnotherMediaSsrc)
{
	RtcpFeedbackTracker tracker(0x22222222);
	const auto nack = makeNack(0x33333333, 100, 0xFFFF);
	const auto pli = makePli(0x33333333);

	tracker.observe(nack.data(), nack.size());
	tracker.observe(pli.data(), pli.size());
	const RtcpFeedbackStats stats = tracker.snapshot();

	EXPECT_EQ(stats.compoundPackets, 2u);
	EXPECT_EQ(stats.nackMessages, 0u);
	EXPECT_EQ(stats.pliMessages, 0u);
}

TEST(RtcpFeedbackTrackerTest, ParsesCompoundPliAndReceiverReport)
{
	constexpr uint32_t mediaSsrc = 0x22222222;
	constexpr uint32_t lastSenderReport = 0x10000000;
	constexpr uint32_t delay = 32768;    // 500 ms
	constexpr uint32_t roundTrip = 8192; // 125 ms
	std::vector<uint8_t> compound = makePli(mediaSsrc);
	const auto receiverReport = makeReceiverReport(mediaSsrc, 64, 7, 900, lastSenderReport, delay);
	compound.insert(compound.end(), receiverReport.begin(), receiverReport.end());

	RtcpFeedbackTracker tracker(mediaSsrc);
	tracker.observe(compound.data(), compound.size(), lastSenderReport + delay + roundTrip);
	const RtcpFeedbackStats stats = tracker.snapshot();

	EXPECT_EQ(stats.compoundPackets, 1u);
	EXPECT_EQ(stats.pliMessages, 1u);
	EXPECT_EQ(stats.receiverReports, 1u);
	EXPECT_EQ(stats.reportBlocks, 1u);
	EXPECT_EQ(stats.maxFractionLost, 64u);
	EXPECT_EQ(stats.maxCumulativeLost, 7);
	EXPECT_EQ(stats.maxJitterTicks, 900u);
	EXPECT_EQ(stats.maxRttMs, 125u);
	EXPECT_EQ(stats.malformedPackets, 0u);
}

TEST(RtcpFeedbackTrackerTest, RejectsTruncatedPacketWithoutReadingPastInput)
{
	std::vector<uint8_t> packet = makeNack(0x22222222, 100, 0);
	packet.pop_back();

	RtcpFeedbackTracker tracker(0x22222222);
	tracker.observe(packet.data(), packet.size());
	const RtcpFeedbackStats stats = tracker.snapshot();

	EXPECT_EQ(stats.compoundPackets, 1u);
	EXPECT_EQ(stats.malformedPackets, 1u);
	EXPECT_EQ(stats.nackMessages, 0u);
}

TEST(RtcpFeedbackTrackerTest, TakeReturnsAndResetsInterval)
{
	const auto packet = makePli(0x22222222);
	RtcpFeedbackTracker tracker(0x22222222);
	tracker.observe(packet.data(), packet.size());

	EXPECT_EQ(tracker.take().pliMessages, 1u);
	EXPECT_EQ(tracker.snapshot().pliMessages, 0u);
	EXPECT_EQ(tracker.snapshot().compoundPackets, 0u);
}

TEST(RtcpFeedbackTrackerTest, SeparatesExpiredAndFailedRetransmissions)
{
	RtcpFeedbackTracker tracker;

	tracker.noteRetransmissionExpired();
	tracker.noteRetransmissionCompleted(false);

	const RtcpFeedbackStats stats = tracker.snapshot();
	EXPECT_EQ(stats.retransmissionsExpired, 1u);
	EXPECT_EQ(stats.retransmissionSendFailures, 1u);
}

TEST(RtcpFeedbackTrackerTest, ParsesRembForTrackedMediaSsrc)
{
	constexpr uint32_t mediaSsrc = 0x22222222;
	const auto packet = makeRemb(mediaSsrc, 250000, 5);
	RtcpFeedbackTracker tracker(mediaSsrc);

	tracker.observe(packet.data(), packet.size());
	const RtcpFeedbackStats stats = tracker.snapshot();
	const auto latest = tracker.latestRemb(std::chrono::seconds(1));

	EXPECT_EQ(stats.rembMessages, 1u);
	EXPECT_EQ(stats.minRembBitrateBps, 8000000u);
	EXPECT_EQ(stats.maxRembBitrateBps, 8000000u);
	ASSERT_TRUE(latest.has_value());
	EXPECT_EQ(latest->bitrateBitsPerSecond, 8000000u);
}

TEST(RtcpFeedbackTrackerTest, IgnoresRembForAnotherMediaSsrc)
{
	const auto packet = makeRemb(0x33333333, 250000, 5);
	RtcpFeedbackTracker tracker(0x22222222);

	tracker.observe(packet.data(), packet.size());

	EXPECT_EQ(tracker.snapshot().rembMessages, 0u);
	EXPECT_FALSE(tracker.latestRemb(std::chrono::seconds(1)).has_value());
}

TEST(RtcpFeedbackTrackerTest, RejectsTruncatedRemb)
{
	auto packet = makeRemb(0x22222222, 250000, 5);
	packet.resize(18);
	packet[2] = 0;
	packet[3] = 4;
	RtcpFeedbackTracker tracker(0x22222222);

	tracker.observe(packet.data(), packet.size());

	EXPECT_EQ(tracker.snapshot().malformedPackets, 1u);
	EXPECT_EQ(tracker.snapshot().rembMessages, 0u);
}
