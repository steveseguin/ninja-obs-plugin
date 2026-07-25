/*
 * Unit tests for the bounded RTP pacer
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <gtest/gtest.h>

#include "vdoninja-rtp-pacer.h"

using namespace std::chrono_literals;
using namespace vdoninja;

namespace
{

RtpPacketPacer::Packet packetWithValue(size_t size, uint8_t value)
{
	return RtpPacketPacer::Packet(size, static_cast<std::byte>(value));
}

} // namespace

TEST(RtpPacketPacerTest, RejectsAnOversizedFrameWithoutSendingAnyPart)
{
	std::atomic<int> sent{0};
	auto sendPacket = [&sent](RtpPacketPacer::Packet &&) { sent.fetch_add(1); };
	RtpPacketPacer pacer(80000, 20ms, sendPacket, 250);

	std::vector<RtpPacketPacer::Packet> frame;
	frame.push_back(packetWithValue(100, 1));
	frame.push_back(packetWithValue(100, 2));
	frame.push_back(packetWithValue(100, 3));

	EXPECT_FALSE(pacer.enqueueFrame(std::move(frame)));
	pacer.stop();
	const RtpPacerStats stats = pacer.getStats();

	EXPECT_EQ(sent.load(), 0);
	EXPECT_EQ(stats.queuedBytes, 0u);
	EXPECT_EQ(stats.droppedFrames, 1u);
}

TEST(RtpPacketPacerTest, PreservesPacketOrder)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<uint8_t> received;
	RtpPacketPacer pacer(
	    8000000, 1ms,
	    [&](RtpPacketPacer::Packet &&packet) {
		    {
			    std::lock_guard<std::mutex> lock(mutex);
			    received.push_back(static_cast<uint8_t>(packet.front()));
		    }
		    cv.notify_one();
	    },
	    4096);

	std::vector<RtpPacketPacer::Packet> frame;
	for (uint8_t value = 1; value <= 6; ++value) {
		frame.push_back(packetWithValue(20, value));
	}
	ASSERT_TRUE(pacer.enqueueFrame(std::move(frame)));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&received]() { return received.size() == 6; }));
	}
	pacer.stop();

	EXPECT_EQ(received, (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
	EXPECT_EQ(pacer.getStats().sentPackets, 6u);
}

TEST(RtpPacketPacerTest, SpreadsLargeFrameAcrossBitrateSizedBatches)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<std::chrono::steady_clock::time_point> sendTimes;

	// 80 kbps over 20 ms permits 200 bytes per batch. Five 100-byte
	// packets therefore require three batches and at least two intervals.
	RtpPacketPacer pacer(
	    80000, 20ms,
	    [&](RtpPacketPacer::Packet &&) {
		    {
			    std::lock_guard<std::mutex> lock(mutex);
			    sendTimes.push_back(std::chrono::steady_clock::now());
		    }
		    cv.notify_one();
	    },
	    4096);

	std::vector<RtpPacketPacer::Packet> frame;
	for (uint8_t value = 1; value <= 5; ++value) {
		frame.push_back(packetWithValue(100, value));
	}
	ASSERT_TRUE(pacer.enqueueFrame(std::move(frame)));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 2s, [&sendTimes]() { return sendTimes.size() == 5; }));
	}
	pacer.stop();
	const RtpPacerStats stats = pacer.getStats();

	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(sendTimes.back() - sendTimes.front());
	EXPECT_GE(elapsed.count(), 30);
	EXPECT_LE(stats.maxBatchBytes, 200u);
	EXPECT_GE(stats.maxPacketDelayMs, 30u);
	EXPECT_EQ(stats.sentPackets, 5u);
}
