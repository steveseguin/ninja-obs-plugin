/*
 * Unit tests for the bounded RTP pacer
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
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

RtpPacketPacer::Packet rtpPacketWithSequence(size_t size, uint16_t sequenceNumber, uint8_t value)
{
	auto packet = packetWithValue(std::max<size_t>(size, 12), value);
	packet[0] = static_cast<std::byte>(0x80);
	packet[1] = static_cast<std::byte>(96);
	packet[2] = static_cast<std::byte>(sequenceNumber >> 8);
	packet[3] = static_cast<std::byte>(sequenceNumber);
	return packet;
}

} // namespace

TEST(RtpPacketPacerTest, ReclaimsUnsentTailSequenceNumbersAcrossWrapAround)
{
	EXPECT_EQ(rewindRtpSequenceNumber(100, 25), 75);
	EXPECT_EQ(rewindRtpSequenceNumber(5, 10), 65531);
	EXPECT_EQ(rewindRtpSequenceNumber(7, 65536), 7);
}

TEST(RtpPacketPacerTest, RejectsAnOversizedFrameWithoutSendingAnyPart)
{
	std::atomic<int> sent{0};
	auto sendPacket = [&sent](RtpPacketPacer::Packet &&) {
		sent.fetch_add(1);
		return true;
	};
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
		    return true;
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
		    return true;
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
	// The ideal token-bucket interval is 30 ms. Allow a small margin because
	// callback/instrumentation time between token debit and timestamp capture
	// is not included in this observed interval.
	EXPECT_GE(elapsed.count(), 25);
	EXPECT_LE(stats.maxBatchBytes, 200u);
	EXPECT_GE(stats.maxPacketDelayMs, 25u);
	EXPECT_EQ(stats.sentPackets, 5u);
}

TEST(RtpPacketPacerTest, RepaysPacketLargerThanBurstBudgetBeforeSendingNextPacket)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<std::chrono::steady_clock::time_point> sendTimes;

	// At 80 kbps a 2 ms bucket is only 20 bytes, smaller than one RTP
	// packet. The first packet is unavoidable, but the pacer must carry the
	// token debt forward instead of sending every oversized packet at 2 ms.
	RtpPacketPacer pacer(
	    80000, 2ms,
	    [&](RtpPacketPacer::Packet &&) {
		    {
			    std::lock_guard<std::mutex> lock(mutex);
			    sendTimes.push_back(std::chrono::steady_clock::now());
		    }
		    cv.notify_one();
		    return true;
	    },
	    4096);

	std::vector<RtpPacketPacer::Packet> frame;
	frame.push_back(packetWithValue(100, 1));
	frame.push_back(packetWithValue(100, 2));
	ASSERT_TRUE(pacer.enqueueFrame(std::move(frame)));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&sendTimes]() { return sendTimes.size() == 2; }));
	}
	pacer.stop();

	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(sendTimes[1] - sendTimes[0]);
	EXPECT_GE(elapsed.count(), 8);
}

TEST(RtpPacketPacerTest, SharedBudgetSpacesInitiallyAlignedViewerBursts)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<std::chrono::steady_clock::time_point> sendTimes;
	auto sharedBudget = std::make_shared<RtpSharedPacerBudget>(100);

	auto sendPacket = [&](RtpPacketPacer::Packet &&) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			sendTimes.push_back(std::chrono::steady_clock::now());
		}
		cv.notify_one();
		return true;
	};
	RtpPacketPacer first(80000, 20ms, sendPacket, 4096, sharedBudget);
	RtpPacketPacer second(80000, 20ms, sendPacket, 4096, sharedBudget);

	std::vector<RtpPacketPacer::Packet> firstFrame;
	firstFrame.push_back(packetWithValue(100, 1));
	std::vector<RtpPacketPacer::Packet> secondFrame;
	secondFrame.push_back(packetWithValue(100, 2));
	ASSERT_TRUE(first.enqueueFrame(std::move(firstFrame)));
	ASSERT_TRUE(second.enqueueFrame(std::move(secondFrame)));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&sendTimes]() { return sendTimes.size() == 2; }));
	}
	first.stop();
	second.stop();

	ASSERT_EQ(sendTimes.size(), 2u);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(sendTimes[1] - sendTimes[0]);
	// Two participants contribute 160 kbps in aggregate. A 100-byte shared
	// burst therefore takes about 5 ms to replenish.
	EXPECT_GE(elapsed.count(), 3);
	EXPECT_EQ(sharedBudget->participantCount(), 0u);
}

TEST(RtpPacketPacerTest, SharedBudgetWaitEndsEachReportedBurst)
{
	std::mutex mutex;
	std::condition_variable cv;
	size_t sentPackets = 0;
	auto sharedBudget = std::make_shared<RtpSharedPacerBudget>(100);

	auto sendPacket = [&](RtpPacketPacer::Packet &&) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			++sentPackets;
		}
		cv.notify_one();
		return true;
	};
	RtpPacketPacer first(80000, 20ms, sendPacket, 4096, sharedBudget);
	RtpPacketPacer second(80000, 20ms, sendPacket, 4096, sharedBudget);

	std::vector<RtpPacketPacer::Packet> firstFrame;
	std::vector<RtpPacketPacer::Packet> secondFrame;
	for (uint8_t value = 0; value < 4; ++value) {
		firstFrame.push_back(packetWithValue(100, value));
		secondFrame.push_back(packetWithValue(100, static_cast<uint8_t>(value + 4)));
	}
	ASSERT_TRUE(first.enqueueFrame(std::move(firstFrame)));
	ASSERT_TRUE(second.enqueueFrame(std::move(secondFrame)));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 2s, [&sentPackets]() { return sentPackets == 8; }));
	}
	first.stop();
	second.stop();

	EXPECT_LE(first.getStats().maxBatchBytes, 100u);
	EXPECT_LE(second.getStats().maxBatchBytes, 100u);
}

TEST(RtpPacketPacerTest, RuntimeBitrateUpdateChangesLocalAndAggregateRates)
{
	auto sharedBudget = std::make_shared<RtpSharedPacerBudget>(4096);
	RtpPacketPacer pacer(
	    8000000, 2ms, [](RtpPacketPacer::Packet &&) { return true; }, 4096, sharedBudget);

	EXPECT_EQ(pacer.bitrateBitsPerSecond(), 8000000u);
	EXPECT_EQ(pacer.batchBudgetBytes(), 2000u);
	EXPECT_EQ(sharedBudget->bitrateBitsPerSecond(), 8000000u);

	pacer.updateBitrate(4000000);

	EXPECT_EQ(pacer.bitrateBitsPerSecond(), 4000000u);
	EXPECT_EQ(pacer.batchBudgetBytes(), 1000u);
	EXPECT_EQ(sharedBudget->bitrateBitsPerSecond(), 4000000u);
	pacer.stop();
}

TEST(RtpPacketPacerTest, OffModeDoesNotCopyPackets)
{
	std::atomic<int> sent{0};
	std::mutex mutex;
	std::condition_variable cv;
	bool completed = false;
	RtpPacketPacer pacer(
	    8000000, 2ms,
	    [&](RtpPacketPacer::Packet &&) {
		    sent.fetch_add(1);
		    return true;
	    },
	    4096);

	std::vector<RtpPacketPacer::Packet> frame;
	frame.push_back(rtpPacketWithSequence(100, 10, 1));
	RtpPacerFrameInfo frameInfo;
	frameInfo.keyframe = true;
	ASSERT_TRUE(pacer.enqueueFrame(std::move(frame), frameInfo, [&](const RtpPacerFrameResult &) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			completed = true;
		}
		cv.notify_all();
	}));
	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&completed]() { return completed; }));
	}
	pacer.stop();

	EXPECT_EQ(sent.load(), 1);
	EXPECT_EQ(pacer.getStats().queuedDuplicates, 0u);
	EXPECT_EQ(pacer.getStats().sentDuplicates, 0u);
}

TEST(RtpPacketPacerTest, LowModeSendsDelayedKeyframeCopy)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<std::chrono::steady_clock::time_point> sendTimes;
	RtpPacketDuplicationConfig duplication;
	duplication.mode = VideoProtectionMode::Low;
	duplication.averageBitrateBitsPerSecond = 1600000;
	duplication.delay = 30ms;
	duplication.maxAge = 200ms;
	RtpPacketPacer pacer(
	    8000000, 2ms,
	    [&](RtpPacketPacer::Packet &&) {
		    {
			    std::lock_guard<std::mutex> lock(mutex);
			    sendTimes.push_back(std::chrono::steady_clock::now());
		    }
		    cv.notify_all();
		    return true;
	    },
	    4096, {}, duplication);

	std::vector<RtpPacketPacer::Packet> frame;
	frame.push_back(rtpPacketWithSequence(100, 10, 1));
	RtpPacerFrameInfo frameInfo;
	frameInfo.keyframe = true;
	ASSERT_TRUE(pacer.enqueueFrame(std::move(frame), frameInfo));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&sendTimes]() { return sendTimes.size() == 2; }));
	}
	pacer.stop();

	ASSERT_EQ(sendTimes.size(), 2u);
	const auto separation = std::chrono::duration_cast<std::chrono::milliseconds>(sendTimes[1] - sendTimes[0]);
	EXPECT_GE(separation.count(), 25);
	const RtpPacerStats stats = pacer.getStats();
	EXPECT_EQ(stats.queuedDuplicates, 1u);
	EXPECT_EQ(stats.sentDuplicates, 1u);
	EXPECT_EQ(stats.sentDuplicateBytes, 100u);
}

TEST(RtpPacketPacerTest, LowModeDoesNotCopyDeltaPackets)
{
	std::mutex mutex;
	std::condition_variable cv;
	bool completed = false;
	RtpPacketDuplicationConfig duplication;
	duplication.mode = VideoProtectionMode::Low;
	duplication.averageBitrateBitsPerSecond = 1600000;
	RtpPacketPacer pacer(
	    8000000, 2ms, [](RtpPacketPacer::Packet &&) { return true; }, 4096, {}, duplication);

	std::vector<RtpPacketPacer::Packet> frame;
	frame.push_back(rtpPacketWithSequence(100, 10, 1));
	ASSERT_TRUE(pacer.enqueueFrame(std::move(frame), {}, [&](const RtpPacerFrameResult &) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			completed = true;
		}
		cv.notify_all();
	}));
	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&completed]() { return completed; }));
	}
	pacer.stop();

	EXPECT_EQ(pacer.getStats().queuedDuplicates, 0u);
	EXPECT_EQ(pacer.getStats().sentDuplicates, 0u);
}

TEST(RtpPacketPacerTest, DuplicationYieldsWhenLiveMediaIsBacklogged)
{
	std::mutex mutex;
	std::condition_variable cv;
	size_t sendsAtFrameCompletion = 0;
	size_t totalSends = 0;
	bool completed = false;
	RtpPacketDuplicationConfig duplication;
	duplication.mode = VideoProtectionMode::High;
	duplication.averageBitrateBitsPerSecond = 80000;
	duplication.delay = 1ms;
	duplication.maxAge = 2s;
	RtpPacketPacer pacer(
	    80000, 10ms,
	    [&](RtpPacketPacer::Packet &&) {
		    std::lock_guard<std::mutex> lock(mutex);
		    ++totalSends;
		    return true;
	    },
	    8192, {}, duplication);

	std::vector<RtpPacketPacer::Packet> frame;
	for (uint8_t value = 0; value < 20; ++value) {
		frame.push_back(rtpPacketWithSequence(100, value, value));
	}
	RtpPacerFrameInfo frameInfo;
	frameInfo.keyframe = true;
	ASSERT_TRUE(pacer.enqueueFrame(std::move(frame), frameInfo, [&](const RtpPacerFrameResult &) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			sendsAtFrameCompletion = totalSends;
			completed = true;
		}
		cv.notify_all();
	}));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 2s, [&completed]() { return completed; }));
	}
	pacer.stop();

	EXPECT_GE(sendsAtFrameCompletion, 20u);
	EXPECT_LT(sendsAtFrameCompletion, 35u);
	EXPECT_GT(pacer.getStats().queuedDuplicates, 0u);
}

TEST(RtpPacketPacerTest, CountsFalseTransportReturnAsSendFailure)
{
	std::mutex mutex;
	std::condition_variable cv;
	bool attempted = false;
	RtpPacketPacer pacer(
	    8000000, 1ms,
	    [&](RtpPacketPacer::Packet &&) {
		    {
			    std::lock_guard<std::mutex> lock(mutex);
			    attempted = true;
		    }
		    cv.notify_one();
		    return false;
	    },
	    4096);

	std::vector<RtpPacketPacer::Packet> frame;
	frame.push_back(packetWithValue(20, 1));
	ASSERT_TRUE(pacer.enqueueFrame(std::move(frame)));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&attempted]() { return attempted; }));
	}
	pacer.stop();
	const RtpPacerStats stats = pacer.getStats();

	EXPECT_EQ(stats.sentPackets, 0u);
	EXPECT_EQ(stats.sendFailures, 1u);
	EXPECT_EQ(stats.failedFrames, 1u);
}

TEST(RtpPacketPacerTest, CompletesAFrameOnlyAfterItsLastPacketIsSent)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<uint8_t> events;
	RtpPacerFrameResult completion;
	bool completed = false;
	RtpPacketPacer pacer(
	    80000, 20ms,
	    [&](RtpPacketPacer::Packet &&packet) {
		    std::lock_guard<std::mutex> lock(mutex);
		    events.push_back(static_cast<uint8_t>(packet.front()));
		    return true;
	    },
	    4096);

	std::vector<RtpPacketPacer::Packet> frame;
	frame.push_back(packetWithValue(100, 1));
	frame.push_back(packetWithValue(100, 2));
	frame.push_back(packetWithValue(100, 3));
	RtpPacerFrameInfo frameInfo;
	frameInfo.keyframe = true;
	frameInfo.timestamp = 90000;
	ASSERT_TRUE(pacer.enqueueFrame(std::move(frame), frameInfo, [&](const RtpPacerFrameResult &result) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			completion = result;
			completed = true;
			events.push_back(9);
		}
		cv.notify_one();
	}));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 2s, [&completed]() { return completed; }));
	}
	pacer.stop();

	EXPECT_EQ(events, (std::vector<uint8_t>{1, 2, 3, 9}));
	EXPECT_TRUE(completion.success);
	EXPECT_TRUE(completion.info.keyframe);
	EXPECT_EQ(completion.info.timestamp, 90000u);
	EXPECT_EQ(completion.sentPackets, 3u);
	EXPECT_EQ(completion.sendFailures, 0u);
	EXPECT_GE(completion.sendDurationMs, 8u);
	EXPECT_EQ(pacer.getStats().sentFrames, 1u);
	EXPECT_EQ(pacer.getStats().sentKeyframes, 1u);
}

TEST(RtpPacketPacerTest, StopsSendingTheRestOfAFrameAfterTransportFailure)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<uint8_t> attempted;
	RtpPacerFrameResult completion;
	bool completed = false;
	RtpPacketPacer pacer(
	    8000000, 1ms,
	    [&](RtpPacketPacer::Packet &&packet) {
		    std::lock_guard<std::mutex> lock(mutex);
		    const uint8_t value = static_cast<uint8_t>(packet.front());
		    attempted.push_back(value);
		    return value != 2;
	    },
	    4096);

	std::vector<RtpPacketPacer::Packet> frame;
	frame.push_back(packetWithValue(20, 1));
	frame.push_back(packetWithValue(20, 2));
	frame.push_back(packetWithValue(20, 3));
	ASSERT_TRUE(pacer.enqueueFrame(std::move(frame), {}, [&](const RtpPacerFrameResult &result) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			completion = result;
			completed = true;
		}
		cv.notify_one();
	}));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&completed]() { return completed; }));
	}
	pacer.stop();

	EXPECT_EQ(attempted, (std::vector<uint8_t>{1, 2}));
	EXPECT_FALSE(completion.success);
	EXPECT_EQ(completion.sentPackets, 1u);
	EXPECT_EQ(completion.sendFailures, 1u);
	const RtpPacerStats stats = pacer.getStats();
	EXPECT_EQ(stats.sentPackets, 1u);
	EXPECT_EQ(stats.sendFailures, 1u);
	EXPECT_EQ(stats.failedFrames, 1u);
}

TEST(RtpPacketPacerTest, InterleavesRepairPacketsWithoutStarvingLiveFrame)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<uint8_t> sent;
	bool firstMediaStarted = false;
	bool releaseFirstMedia = false;
	RtpPacketPacer pacer(
	    8000000, 2ms,
	    [&](RtpPacketPacer::Packet &&packet) {
		    std::unique_lock<std::mutex> lock(mutex);
		    const uint8_t value = static_cast<uint8_t>(packet.front());
		    sent.push_back(value);
		    if (!firstMediaStarted) {
			    firstMediaStarted = true;
			    cv.notify_all();
			    cv.wait(lock, [&releaseFirstMedia]() { return releaseFirstMedia; });
		    }
		    cv.notify_all();
		    return true;
	    },
	    4096);

	std::vector<RtpPacketPacer::Packet> mediaFrame;
	for (uint8_t value = 10; value <= 15; ++value) {
		mediaFrame.push_back(packetWithValue(20, value));
	}
	ASSERT_TRUE(pacer.enqueueFrame(std::move(mediaFrame)));

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&firstMediaStarted]() { return firstMediaStarted; }));
	}

	for (uint8_t value = 1; value <= 5; ++value) {
		ASSERT_TRUE(pacer.enqueueRepair(packetWithValue(20, value), [&](RtpPacketPacer::Packet &&packet) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				sent.push_back(static_cast<uint8_t>(packet.front()));
			}
			cv.notify_all();
			return true;
		}));
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseFirstMedia = true;
	}
	cv.notify_all();
	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&sent]() { return sent.size() == 11; }));
	}
	pacer.stop();

	EXPECT_EQ(sent, (std::vector<uint8_t>{10, 1, 2, 3, 4, 11, 5, 12, 13, 14, 15}));
	const RtpPacerStats stats = pacer.getStats();
	EXPECT_EQ(stats.sentRepairs, 5u);
	EXPECT_EQ(stats.sentFrames, 1u);
}

TEST(RtpPacketPacerTest, RepairBudgetExpiresStaleNackWorkInsteadOfStarvingLiveMedia)
{
	std::mutex mutex;
	std::condition_variable cv;
	size_t completed = 0;
	size_t sent = 0;
	size_t expired = 0;
	RtpPacketPacer pacer(
	    80000, 20ms, [](RtpPacketPacer::Packet &&) { return true; }, 4096);

	for (uint8_t value = 1; value <= 20; ++value) {
		ASSERT_TRUE(pacer.enqueueRepair(
		    packetWithValue(100, value), [](RtpPacketPacer::Packet &&) { return true; },
		    [&](RtpPacerRepairOutcome outcome) {
			    {
				    std::lock_guard<std::mutex> lock(mutex);
				    ++completed;
				    if (outcome == RtpPacerRepairOutcome::Sent) {
					    ++sent;
				    } else if (outcome == RtpPacerRepairOutcome::Expired) {
					    ++expired;
				    }
			    }
			    cv.notify_all();
		    }));
	}

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 2s, [&completed]() { return completed == 20; }));
	}
	pacer.stop();

	EXPECT_GT(sent, 0u);
	EXPECT_GT(expired, 0u);
	const RtpPacerStats stats = pacer.getStats();
	EXPECT_EQ(stats.sentRepairs, sent);
	EXPECT_EQ(stats.expiredRepairs, expired);
	EXPECT_EQ(stats.failedRepairs, 0u);
}

TEST(RtpPacketPacerTest, DiscardsDependentDeltaFramesOnlyUntilNextKeyframe)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<uint8_t> sent;
	size_t discardedFrames = 0;
	bool firstPacketStarted = false;
	bool releaseFirstPacket = false;
	bool completed = false;
	RtpPacketPacer *pacerPtr = nullptr;
	auto pacer = std::make_unique<RtpPacketPacer>(
	    8000000, 2ms,
	    [&](RtpPacketPacer::Packet &&packet) {
		    std::unique_lock<std::mutex> lock(mutex);
		    const uint8_t value = static_cast<uint8_t>(packet.front());
		    sent.push_back(value);
		    if (value == 1) {
			    firstPacketStarted = true;
			    cv.notify_all();
			    cv.wait(lock, [&releaseFirstPacket]() { return releaseFirstPacket; });
		    }
		    lock.unlock();
		    cv.notify_all();
		    return true;
	    },
	    4096);
	pacerPtr = pacer.get();

	RtpPacerFrameInfo keyframeInfo;
	keyframeInfo.keyframe = true;
	std::vector<RtpPacketPacer::Packet> firstKeyframe;
	firstKeyframe.push_back(packetWithValue(20, 1));
	ASSERT_TRUE(pacer->enqueueFrame(std::move(firstKeyframe), keyframeInfo, [&](const RtpPacerFrameResult &) {
		discardedFrames = pacerPtr->discardQueuedDeltaFramesUntilKeyframe();
		{
			std::lock_guard<std::mutex> lock(mutex);
			completed = true;
		}
		cv.notify_all();
	}));
	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&firstPacketStarted]() { return firstPacketStarted; }));
	}

	for (uint8_t value = 2; value <= 3; ++value) {
		std::vector<RtpPacketPacer::Packet> delta;
		delta.push_back(packetWithValue(20, value));
		ASSERT_TRUE(pacer->enqueueFrame(std::move(delta)));
	}
	std::vector<RtpPacketPacer::Packet> nextKeyframe;
	nextKeyframe.push_back(packetWithValue(20, 4));
	ASSERT_TRUE(pacer->enqueueFrame(std::move(nextKeyframe), keyframeInfo));
	std::vector<RtpPacketPacer::Packet> followingDelta;
	followingDelta.push_back(packetWithValue(20, 5));
	ASSERT_TRUE(pacer->enqueueFrame(std::move(followingDelta)));
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseFirstPacket = true;
	}
	cv.notify_all();

	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&completed]() { return completed; }));
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&sent]() { return sent.size() == 3; }));
	}
	pacer->stop();

	EXPECT_EQ(discardedFrames, 2u);
	EXPECT_EQ(sent, (std::vector<uint8_t>{1, 4, 5}));
	EXPECT_EQ(pacer->getStats().droppedFrames, 2u);
}

TEST(RtpPacketPacerTest, RecoveryPurgeLetsAnAlreadyStartedFrameFinishAtomically)
{
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<uint8_t> sent;
	bool firstPacketStarted = false;
	bool releaseFirstPacket = false;
	RtpPacketPacer pacer(
	    8000000, 2ms,
	    [&](RtpPacketPacer::Packet &&packet) {
		    std::unique_lock<std::mutex> lock(mutex);
		    const uint8_t value = static_cast<uint8_t>(packet.front());
		    sent.push_back(value);
		    if (value == 1) {
			    firstPacketStarted = true;
			    cv.notify_all();
			    cv.wait(lock, [&releaseFirstPacket]() { return releaseFirstPacket; });
		    }
		    lock.unlock();
		    cv.notify_all();
		    return true;
	    },
	    4096);

	std::vector<RtpPacketPacer::Packet> activeFrame;
	for (uint8_t value = 1; value <= 3; ++value) {
		activeFrame.push_back(packetWithValue(20, value));
	}
	ASSERT_TRUE(pacer.enqueueFrame(std::move(activeFrame)));
	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&firstPacketStarted]() { return firstPacketStarted; }));
	}

	for (uint8_t value = 4; value <= 5; ++value) {
		std::vector<RtpPacketPacer::Packet> staleFrame;
		staleFrame.push_back(packetWithValue(20, value));
		ASSERT_TRUE(pacer.enqueueFrame(std::move(staleFrame)));
	}
	size_t discardedPackets = 0;
	EXPECT_EQ(pacer.discardQueuedMediaFramesAfterCurrent(&discardedPackets), 2u);
	EXPECT_EQ(discardedPackets, 2u);
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseFirstPacket = true;
	}
	cv.notify_all();
	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&sent]() { return sent.size() == 3; }));
	}
	pacer.stop();

	EXPECT_EQ(sent, (std::vector<uint8_t>{1, 2, 3}));
	const RtpPacerStats stats = pacer.getStats();
	EXPECT_EQ(stats.sentFrames, 1u);
	EXPECT_EQ(stats.droppedFrames, 2u);
}

TEST(RtpPacketPacerTest, RecoveryPurgeInvalidatesAFrameSelectedDuringSharedBudgetWait)
{
	std::mutex mutex;
	std::condition_variable cv;
	size_t firstSent = 0;
	size_t secondSent = 0;
	auto sharedBudget = std::make_shared<RtpSharedPacerBudget>(100);
	RtpPacketPacer first(
	    800, 1s,
	    [&](RtpPacketPacer::Packet &&) {
		    {
			    std::lock_guard<std::mutex> lock(mutex);
			    ++firstSent;
		    }
		    cv.notify_all();
		    return true;
	    },
	    4096, sharedBudget);
	RtpPacketPacer second(
	    800, 1s,
	    [&](RtpPacketPacer::Packet &&) {
		    {
			    std::lock_guard<std::mutex> lock(mutex);
			    ++secondSent;
		    }
		    cv.notify_all();
		    return true;
	    },
	    4096, sharedBudget);

	std::vector<RtpPacketPacer::Packet> firstFrame;
	firstFrame.push_back(packetWithValue(100, 1));
	ASSERT_TRUE(first.enqueueFrame(std::move(firstFrame)));
	{
		std::unique_lock<std::mutex> lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, 1s, [&firstSent]() { return firstSent == 1; }));
	}

	std::vector<RtpPacketPacer::Packet> secondFrame;
	secondFrame.push_back(packetWithValue(100, 2));
	ASSERT_TRUE(second.enqueueFrame(std::move(secondFrame)));
	{
		std::unique_lock<std::mutex> lock(mutex);
		EXPECT_FALSE(cv.wait_for(lock, 50ms, [&secondSent]() { return secondSent != 0; }));
	}
	size_t discardedPackets = 0;
	EXPECT_EQ(second.discardQueuedMediaFramesAfterCurrent(&discardedPackets), 1u);
	EXPECT_EQ(discardedPackets, 1u);
	{
		std::unique_lock<std::mutex> lock(mutex);
		EXPECT_FALSE(cv.wait_for(lock, 700ms, [&secondSent]() { return secondSent != 0; }));
	}
	first.stop();
	second.stop();

	EXPECT_EQ(secondSent, 0u);
	EXPECT_EQ(second.getStats().droppedFrames, 1u);
}
