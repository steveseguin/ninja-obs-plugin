/*
 * Unit tests for RTP retransmission repair helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "vdoninja-rtp-repair.h"

using namespace std::chrono_literals;
using namespace vdoninja;

namespace
{

std::vector<uint8_t> rtpPacket(uint16_t sequenceNumber, size_t size = 32)
{
	std::vector<uint8_t> packet(std::max<size_t>(size, 12), 0);
	packet[0] = 0x80;
	packet[1] = 96;
	packet[2] = static_cast<uint8_t>(sequenceNumber >> 8);
	packet[3] = static_cast<uint8_t>(sequenceNumber);
	return packet;
}

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

std::vector<uint8_t> nackPacket(uint32_t mediaSsrc, uint16_t packetId, uint16_t bitmask)
{
	std::vector<uint8_t> packet = {0x81, 205, 0, 3};
	appendU32(packet, 0x11111111);
	appendU32(packet, mediaSsrc);
	appendU16(packet, packetId);
	appendU16(packet, bitmask);
	return packet;
}

void addFourBytePadding(std::vector<uint8_t> &packet)
{
	packet[0] |= 0x20U;
	packet.insert(packet.end(), {0, 0, 0, 4});
	const uint16_t wordsMinusOne = static_cast<uint16_t>(packet.size() / 4U - 1U);
	packet[2] = static_cast<uint8_t>(wordsMinusOne >> 8);
	packet[3] = static_cast<uint8_t>(wordsMinusOne);
}

} // namespace

TEST(RtpRetransmissionCacheTest, StoresAndFindsPacketBySequenceNumber)
{
	RtpRetransmissionCache cache;
	const auto packet = rtpPacket(321);

	ASSERT_TRUE(cache.store(packet.data(), packet.size()));
	const auto found = cache.find(321);

	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->size(), packet.size());
	EXPECT_EQ(cache.size(), 1u);
}

TEST(RtpRetransmissionCacheTest, EvictsOldestPacketAtCountLimit)
{
	RtpRetransmissionCache cache(2, 4096, 2s);
	const auto first = rtpPacket(1);
	const auto second = rtpPacket(2);
	const auto third = rtpPacket(3);

	ASSERT_TRUE(cache.store(first.data(), first.size()));
	ASSERT_TRUE(cache.store(second.data(), second.size()));
	ASSERT_TRUE(cache.store(third.data(), third.size()));

	EXPECT_FALSE(cache.find(1).has_value());
	EXPECT_TRUE(cache.find(2).has_value());
	EXPECT_TRUE(cache.find(3).has_value());
	EXPECT_EQ(cache.size(), 2u);
}

TEST(RtpRetransmissionCacheTest, ExpiresPacketAtAgeLimit)
{
	RtpRetransmissionCache cache(16, 4096, 200ms);
	const auto packet = rtpPacket(10);
	const auto start = RtpRetransmissionCache::Clock::time_point{};

	ASSERT_TRUE(cache.store(packet.data(), packet.size(), start));
	EXPECT_TRUE(cache.find(10, start + 199ms).has_value());
	EXPECT_FALSE(cache.find(10, start + 201ms).has_value());
}

TEST(RtpRetransmissionCacheTest, NewPacketWithWrappedSequenceReplacesLookup)
{
	RtpRetransmissionCache cache(16, 4096, 2s);
	auto first = rtpPacket(10);
	auto replacement = rtpPacket(10);
	replacement[11] = 99;

	ASSERT_TRUE(cache.store(first.data(), first.size()));
	ASSERT_TRUE(cache.store(replacement.data(), replacement.size()));
	const auto found = cache.find(10);

	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(static_cast<uint8_t>((*found)[11]), 99u);
	EXPECT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache.bytes(), replacement.size());
}

TEST(RtpRetransmissionCacheTest, DuplicateCopiesDoNotEvictUniqueHistory)
{
	RtpRetransmissionCache cache(2, 4096, 2s);
	auto first = rtpPacket(10);
	auto duplicate = rtpPacket(10);
	duplicate[11] = 99;
	const auto second = rtpPacket(11);

	ASSERT_TRUE(cache.store(first.data(), first.size()));
	ASSERT_TRUE(cache.store(duplicate.data(), duplicate.size()));
	ASSERT_TRUE(cache.store(second.data(), second.size()));

	EXPECT_EQ(cache.size(), 2u);
	EXPECT_EQ(cache.bytes(), duplicate.size() + second.size());
	ASSERT_TRUE(cache.find(10).has_value());
	EXPECT_EQ(static_cast<uint8_t>((*cache.find(10))[11]), 99u);
	EXPECT_TRUE(cache.find(11).has_value());
}

TEST(RtcpNackParserTest, ExpandsBitmaskAndDeduplicatesRequests)
{
	constexpr uint32_t mediaSsrc = 0x22222222;
	auto packet = nackPacket(mediaSsrc, 100, 0b0000000000000101);
	const auto duplicate = nackPacket(mediaSsrc, 100, 0);
	packet.insert(packet.end(), duplicate.begin(), duplicate.end());
	bool malformed = false;

	const auto requests = parseRtcpNackRequests(packet.data(), packet.size(), mediaSsrc, &malformed);

	EXPECT_FALSE(malformed);
	EXPECT_EQ(requests, (std::vector<uint16_t>{100, 101, 103}));
}

TEST(RtcpNackParserTest, ExcludesRtcpPaddingFromRequests)
{
	constexpr uint32_t mediaSsrc = 0x22222222;
	auto packet = nackPacket(mediaSsrc, 100, 0);
	addFourBytePadding(packet);
	bool malformed = false;

	const auto requests = parseRtcpNackRequests(packet.data(), packet.size(), mediaSsrc, &malformed);

	EXPECT_FALSE(malformed);
	EXPECT_EQ(requests, (std::vector<uint16_t>{100}));
}

TEST(RtcpNackParserTest, FiltersMediaSsrcAndRejectsTruncation)
{
	constexpr uint32_t mediaSsrc = 0x22222222;
	auto packet = nackPacket(0x33333333, 100, 0xFFFF);
	EXPECT_TRUE(parseRtcpNackRequests(packet.data(), packet.size(), mediaSsrc).empty());

	packet.pop_back();
	bool malformed = false;
	EXPECT_TRUE(parseRtcpNackRequests(packet.data(), packet.size(), 0, &malformed).empty());
	EXPECT_TRUE(malformed);
}

TEST(RtcpNackParserFuzzTest, RandomAndMutatedCompoundPacketsAreBoundedAndDeterministic)
{
	std::mt19937 rng(0x205F00D);
	std::uniform_int_distribution<size_t> rawSizeDist(0, 512);
	std::uniform_int_distribution<int> byteDist(0, 255);
	std::uniform_int_distribution<int> mutationCountDist(0, 8);

	for (int iteration = 0; iteration < 30000; ++iteration) {
		std::vector<uint8_t> packet;
		if (iteration % 2 == 0) {
			packet.resize(rawSizeDist(rng));
			for (uint8_t &byte : packet) {
				byte = static_cast<uint8_t>(byteDist(rng));
			}
		} else {
			packet = nackPacket(rng(), static_cast<uint16_t>(rng()), static_cast<uint16_t>(rng()));
			if (iteration % 3 == 0) {
				const auto second = nackPacket(rng(), static_cast<uint16_t>(rng()), static_cast<uint16_t>(rng()));
				packet.insert(packet.end(), second.begin(), second.end());
			}
			for (int mutation = 0; mutation < mutationCountDist(rng) && !packet.empty(); ++mutation) {
				packet[static_cast<size_t>(rng()) % packet.size()] = static_cast<uint8_t>(byteDist(rng));
			}
			if (!packet.empty() && iteration % 5 == 0) {
				packet.resize(static_cast<size_t>(rng()) % packet.size());
			}
		}

		const uint32_t mediaSsrc = iteration % 4 == 0 ? 0U : rng();
		const uint8_t *data = packet.empty() ? nullptr : packet.data();
		bool firstMalformed = false;
		bool secondMalformed = false;
		const auto first = parseRtcpNackRequests(data, packet.size(), mediaSsrc, &firstMalformed);
		const auto second = parseRtcpNackRequests(data, packet.size(), mediaSsrc, &secondMalformed);

		EXPECT_EQ(firstMalformed, secondMalformed) << "iteration=" << iteration << " size=" << packet.size();
		EXPECT_EQ(first, second) << "iteration=" << iteration << " size=" << packet.size();
		EXPECT_LE(first.size(), 4096U);
		const std::unordered_set<uint16_t> unique(first.begin(), first.end());
		EXPECT_EQ(unique.size(), first.size());
	}
}
