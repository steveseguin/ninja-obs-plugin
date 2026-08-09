/*
 * Unit tests for VP9 alpha frame timestamp pairing helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <condition_variable>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

#include "vdoninja-alpha-sync.h"

using namespace vdoninja;

namespace
{

PendingPrimaryFrame makePrimary(uint32_t rtpTimestamp, int width = 640, int height = 360, uint64_t mediaEpoch = 1)
{
	PendingPrimaryFrame frame;
	frame.rtpTimestamp = rtpTimestamp;
	frame.width = width;
	frame.height = height;
	frame.mediaEpoch = mediaEpoch;
	return frame;
}

PendingAlphaFrame makeAlpha(uint32_t rtpTimestamp, int width = 640, int height = 360, int yLinesize = 640,
                            uint8_t fill = 0x7F, uint64_t mediaEpoch = 1)
{
	PendingAlphaFrame frame;
	frame.rtpTimestamp = rtpTimestamp;
	frame.width = width;
	frame.height = height;
	frame.yLinesize = yLinesize;
	frame.mediaEpoch = mediaEpoch;
	frame.yData.assign(static_cast<size_t>(yLinesize) * static_cast<size_t>(height), fill);
	return frame;
}

} // namespace

TEST(AlphaSyncTest, RtpTimestampOrderingHandlesWrapAround)
{
	EXPECT_TRUE(isRtpTimestampBefore(0xFFFFFFF0u, 0x00000010u));
	EXPECT_FALSE(isRtpTimestampBefore(0x00000010u, 0xFFFFFFF0u));
}

TEST(AlphaSyncTest, ResolvesDecoderPreservedRtpTimestampWithoutFifoGuessing)
{
	EXPECT_EQ(resolveDecodedRtpTimestamp(1234, 5678), 1234u);
	EXPECT_EQ(resolveDecodedRtpTimestamp(-1, 5678), 5678u);
	EXPECT_EQ(resolveDecodedRtpTimestamp(static_cast<int64_t>(UINT32_MAX), -1), UINT32_MAX);
	EXPECT_FALSE(resolveDecodedRtpTimestamp(-1, -1).has_value());
	EXPECT_FALSE(resolveDecodedRtpTimestamp(static_cast<int64_t>(UINT32_MAX) + 1, -1).has_value());
}

TEST(AlphaSyncTest, PairsExactTimestampWhenAlphaArrivesFirst)
{
	AlphaFrameSynchronizer sync;
	const auto alphaResult = sync.pushAlpha(makeAlpha(1000, 640, 360, 640, 0x33));
	ASSERT_FALSE(alphaResult.pair.has_value());
	EXPECT_TRUE(alphaResult.queued);

	const auto primaryResult = sync.pushPrimary(makePrimary(1000));
	ASSERT_TRUE(primaryResult.pair.has_value());
	EXPECT_EQ(primaryResult.pair->mediaEpoch, 1u);
	EXPECT_EQ(primaryResult.pair->alpha.yData.front(), 0x33);
	EXPECT_TRUE(primaryResult.pair->dimensionsMatch());
	EXPECT_EQ(sync.pendingAlphaCount(), 0u);
	EXPECT_EQ(sync.pendingPrimaryCount(), 0u);
}

TEST(AlphaSyncTest, PairsExactTimestampWhenPrimaryArrivesFirst)
{
	AlphaFrameSynchronizer sync;
	const auto primaryResult = sync.pushPrimary(makePrimary(2000));
	ASSERT_FALSE(primaryResult.pair.has_value());
	EXPECT_TRUE(primaryResult.queued);

	const auto alphaResult = sync.pushAlpha(makeAlpha(2000));
	ASSERT_TRUE(alphaResult.pair.has_value());
	EXPECT_EQ(alphaResult.pair->primary.rtpTimestamp, 2000u);
	EXPECT_EQ(alphaResult.pair->alpha.rtpTimestamp, 2000u);
}

TEST(AlphaSyncTest, NeverUsesPastOrFutureAlphaForMissingExactTimestamp)
{
	AlphaFrameSynchronizer sync(8, 1000);
	sync.pushAlpha(makeAlpha(100, 640, 360, 640, 0x11));
	sync.pushAlpha(makeAlpha(300, 640, 360, 640, 0x33));

	const auto result = sync.pushPrimary(makePrimary(200));
	EXPECT_FALSE(result.pair.has_value());
	EXPECT_TRUE(result.queued);
	EXPECT_EQ(sync.pendingPrimaryCount(), 1u);
	EXPECT_EQ(sync.pendingAlphaCount(), 2u);
}

TEST(AlphaSyncTest, TimestampAgeEvictsMissingMateAndRejectsLateArrival)
{
	AlphaFrameSynchronizer sync(8, 50);
	sync.pushPrimary(makePrimary(100));
	const auto advanceResult = sync.pushPrimary(makePrimary(200));
	EXPECT_EQ(advanceResult.droppedPrimaryFrames, 1u);
	EXPECT_EQ(sync.pendingPrimaryCount(), 1u);

	const auto lateAlpha = sync.pushAlpha(makeAlpha(100));
	EXPECT_TRUE(lateAlpha.rejectedIncomingFrame);
	EXPECT_FALSE(lateAlpha.pair.has_value());
	EXPECT_EQ(sync.pendingAlphaCount(), 0u);
}

TEST(AlphaSyncTest, DuplicatePendingFramesReplaceDataWithoutGrowingQueues)
{
	AlphaFrameSynchronizer sync;
	sync.pushAlpha(makeAlpha(1000, 640, 360, 640, 0x11));
	sync.pushAlpha(makeAlpha(1000, 640, 360, 640, 0x99));
	EXPECT_EQ(sync.pendingAlphaCount(), 1u);

	sync.pushPrimary(makePrimary(2000, 640, 360));
	sync.pushPrimary(makePrimary(2000, 999, 360));
	EXPECT_EQ(sync.pendingPrimaryCount(), 1u);

	const auto alphaPair = sync.pushPrimary(makePrimary(1000));
	ASSERT_TRUE(alphaPair.pair.has_value());
	EXPECT_EQ(alphaPair.pair->alpha.yData.front(), 0x99);

	// Emitting timestamp 1000 leaves the newer duplicate primary queued.
	const auto primaryPair = sync.pushAlpha(makeAlpha(2000));
	ASSERT_TRUE(primaryPair.pair.has_value());
	EXPECT_EQ(primaryPair.pair->primary.width, 999);
}

TEST(AlphaSyncTest, PausedCallbackEpochIsRejectedAfterTrackTransition)
{
	MediaEpochGate gate;
	const uint64_t pausedCallbackEpoch = gate.capture();
	EXPECT_TRUE(gate.isCurrent(pausedCallbackEpoch));

	const uint64_t replacementEpoch = gate.advance();
	EXPECT_NE(replacementEpoch, pausedCallbackEpoch);
	EXPECT_FALSE(gate.isCurrent(pausedCallbackEpoch));
	EXPECT_TRUE(gate.isCurrent(replacementEpoch));
}

TEST(AlphaSyncTest, ExactTimestampNeverPairsAcrossMediaEpochs)
{
	AlphaFrameSynchronizer sync;
	sync.pushAlpha(makeAlpha(100, 640, 360, 640, 0x11, 1));
	const auto crossEpochPrimary = sync.pushPrimary(makePrimary(100, 640, 360, 2));
	EXPECT_FALSE(crossEpochPrimary.pair.has_value());

	const auto currentAlpha = sync.pushAlpha(makeAlpha(100, 640, 360, 640, 0x22, 2));
	ASSERT_TRUE(currentAlpha.pair.has_value());
	EXPECT_EQ(currentAlpha.pair->mediaEpoch, 2u);
	EXPECT_EQ(currentAlpha.pair->alpha.yData.front(), 0x22);
}

TEST(AlphaSyncTest, MediaPeerOwnershipIsSymmetricForAlphaFirstAndPrimaryFirst)
{
	EXPECT_TRUE(mediaTrackPeerCanOwn("peer-a", "", ""));
	EXPECT_TRUE(mediaTrackPeerCanOwn("peer-a", "peer-a", ""));
	EXPECT_TRUE(mediaTrackPeerCanOwn("peer-a", "", "peer-a"));
	EXPECT_FALSE(mediaTrackPeerCanOwn("peer-b", "peer-a", ""));
	EXPECT_FALSE(mediaTrackPeerCanOwn("peer-b", "", "peer-a"));
	EXPECT_FALSE(mediaTrackPeerCanOwn("", "", ""));

	// Once the old peer is removed, either arrival order may adopt the new peer.
	EXPECT_TRUE(mediaTrackPeerCanOwn("peer-b", "", ""));
	EXPECT_TRUE(mediaTrackPeerCanOwn("peer-b", "peer-b", ""));
	EXPECT_TRUE(mediaTrackPeerCanOwn("peer-b", "", "peer-b"));
}

TEST(AlphaSyncTest, ConcurrentOldAlphaRemovalCannotRetireNewReplacement)
{
	std::mutex slotMutex;
	std::mutex latchMutex;
	std::condition_variable latch;
	bool removalCaptured = false;
	bool replacementInstalled = false;
	auto oldAlpha = std::make_shared<int>(1);
	auto newAlpha = std::make_shared<int>(2);
	std::shared_ptr<int> sourceSlot = oldAlpha;
	bool staleRemovalApplied = true;

	std::thread remover([&]() {
		const auto retiredIdentity = oldAlpha;
		{
			std::lock_guard<std::mutex> lock(latchMutex);
			removalCaptured = true;
		}
		latch.notify_all();
		{
			std::unique_lock<std::mutex> lock(latchMutex);
			latch.wait(lock, [&]() { return replacementInstalled; });
		}
		std::lock_guard<std::mutex> lock(slotMutex);
		staleRemovalApplied = clearSharedSlotIfMatches(sourceSlot, retiredIdentity);
	});

	std::thread installer([&]() {
		{
			std::unique_lock<std::mutex> lock(latchMutex);
			latch.wait(lock, [&]() { return removalCaptured; });
		}
		{
			std::lock_guard<std::mutex> lock(slotMutex);
			sourceSlot = newAlpha;
		}
		{
			std::lock_guard<std::mutex> lock(latchMutex);
			replacementInstalled = true;
		}
		latch.notify_all();
	});

	remover.join();
	installer.join();
	EXPECT_FALSE(staleRemovalApplied);
	EXPECT_EQ(sourceSlot, newAlpha);
}

TEST(AlphaSyncTest, FinalOutputTimestampMappingRejectsOutOfOrderAndHandlesWrap)
{
	RtpOutputTimestampMapper mapper;
	const auto beforeWrap = mapper.map(0xFFFFFFF0u, 1000000);
	ASSERT_TRUE(beforeWrap.has_value());
	const auto afterWrap = mapper.map(0x00000010u, 1000001);
	ASSERT_TRUE(afterWrap.has_value());
	EXPECT_GT(*afterWrap, *beforeWrap);
	EXPECT_FALSE(mapper.map(0x00000008u, 2000000).has_value());
	EXPECT_FALSE(mapper.map(0x00000010u, 2000000).has_value());
	const auto later = mapper.map(0x00000020u, 1000002);
	ASSERT_TRUE(later.has_value());
	EXPECT_GT(*later, *afterWrap);
}

TEST(AlphaSyncTest, MapperResetAllowsSameRtpTimestampInNewTrackEpoch)
{
	RtpOutputTimestampMapper mapper;
	ASSERT_TRUE(mapper.map(9000, 100).has_value());
	EXPECT_FALSE(mapper.map(9000, 200).has_value());
	mapper.reset();
	EXPECT_EQ(mapper.map(9000, 300), 300u);
}

TEST(AlphaSyncTest, EveryIndependentTrackTransitionInvalidatesSeededQueuesAndExtractedPair)
{
	for (const bool replacingAlpha : {false, true}) {
		SCOPED_TRACE(replacingAlpha ? "alpha replacement" : "primary replacement");
		MediaEpochGate gate;
		AlphaFrameSynchronizer sync;
		const uint64_t oldEpoch = gate.capture();
		sync.pushPrimary(makePrimary(100, 640, 360, oldEpoch));
		sync.pushAlpha(makeAlpha(200, 640, 360, 640, 0x22, oldEpoch));
		const uint64_t extractedGeneration = sync.generation();

		gate.advance();
		sync.reset();
		EXPECT_FALSE(gate.isCurrent(oldEpoch));
		EXPECT_FALSE(sync.isCurrentGeneration(extractedGeneration));
		EXPECT_EQ(sync.pendingPrimaryCount(), 0u);
		EXPECT_EQ(sync.pendingAlphaCount(), 0u);
	}
}

TEST(AlphaSyncTest, AlphaEnableDisableCloseAndReenableCannotReuseOldState)
{
	MediaEpochGate gate;
	AlphaFrameSynchronizer sync;
	for (int transition = 0; transition < 3; ++transition) {
		const uint64_t oldEpoch = gate.capture();
		sync.pushPrimary(makePrimary(100, 640, 360, oldEpoch));
		sync.pushAlpha(makeAlpha(200, 640, 360, 640, 0x33, oldEpoch));
		gate.advance();
		sync.reset();
		EXPECT_FALSE(gate.isCurrent(oldEpoch));
		EXPECT_EQ(sync.pendingPrimaryCount(), 0u);
		EXPECT_EQ(sync.pendingAlphaCount(), 0u);
	}

	const uint64_t reenabledEpoch = gate.capture();
	sync.pushPrimary(makePrimary(300, 640, 360, reenabledEpoch));
	const auto pair = sync.pushAlpha(makeAlpha(300, 640, 360, 640, 0x44, reenabledEpoch));
	ASSERT_TRUE(pair.pair.has_value());
	EXPECT_EQ(pair.pair->mediaEpoch, reenabledEpoch);
}

TEST(AlphaSyncTest, RetainedPrimaryFrameHasExactlyOnceLifetimeAcrossResetAndExtractedPair)
{
	int queuedDeletes = 0;
	int extractedDeletes = 0;
	{
		AlphaFrameSynchronizer sync;
		auto *queuedStorage = new int(1);
		auto queued = makePrimary(100);
		queued.frame = std::shared_ptr<AVFrame>(reinterpret_cast<AVFrame *>(queuedStorage), [&](AVFrame *frame) {
			delete reinterpret_cast<int *>(frame);
			queuedDeletes++;
		});
		sync.pushPrimary(std::move(queued));
		sync.reset();
		EXPECT_EQ(queuedDeletes, 1);

		auto *extractedStorage = new int(2);
		auto extracted = makePrimary(200);
		extracted.frame = std::shared_ptr<AVFrame>(reinterpret_cast<AVFrame *>(extractedStorage), [&](AVFrame *frame) {
			delete reinterpret_cast<int *>(frame);
			extractedDeletes++;
		});
		sync.pushPrimary(std::move(extracted));
		auto result = sync.pushAlpha(makeAlpha(200));
		ASSERT_TRUE(result.pair.has_value());
		const uint64_t extractedGeneration = result.pair->generation;
		sync.reset();
		EXPECT_FALSE(sync.isCurrentGeneration(extractedGeneration));
		EXPECT_EQ(extractedDeletes, 0);
		result.pair.reset();
		EXPECT_EQ(extractedDeletes, 1);
	}
	EXPECT_EQ(queuedDeletes, 1);
	EXPECT_EQ(extractedDeletes, 1);
}

TEST(AlphaSyncTest, DuplicateAfterEmissionCannotReuseConsumedMask)
{
	AlphaFrameSynchronizer sync;
	sync.pushAlpha(makeAlpha(100));
	ASSERT_TRUE(sync.pushPrimary(makePrimary(100)).pair.has_value());

	const auto duplicatePrimary = sync.pushPrimary(makePrimary(100));
	const auto duplicateAlpha = sync.pushAlpha(makeAlpha(100));
	EXPECT_TRUE(duplicatePrimary.rejectedIncomingFrame);
	EXPECT_TRUE(duplicateAlpha.rejectedIncomingFrame);
	EXPECT_EQ(sync.pendingPrimaryCount(), 0u);
	EXPECT_EQ(sync.pendingAlphaCount(), 0u);
}

TEST(AlphaSyncTest, FutureAlphaRemainsQueuedForItsOwnOutOfOrderPrimary)
{
	AlphaFrameSynchronizer sync;
	sync.pushAlpha(makeAlpha(300, 640, 360, 640, 0x30));
	sync.pushAlpha(makeAlpha(200, 640, 360, 640, 0x20));

	const auto earlierPair = sync.pushPrimary(makePrimary(200));
	ASSERT_TRUE(earlierPair.pair.has_value());
	EXPECT_EQ(earlierPair.pair->alpha.yData.front(), 0x20);
	EXPECT_EQ(sync.pendingAlphaCount(), 1u);

	const auto laterPair = sync.pushPrimary(makePrimary(300));
	ASSERT_TRUE(laterPair.pair.has_value());
	EXPECT_EQ(laterPair.pair->alpha.yData.front(), 0x30);
}

TEST(AlphaSyncTest, LateOlderPairIsDroppedAfterNewerPairWasEmitted)
{
	AlphaFrameSynchronizer sync;
	sync.pushPrimary(makePrimary(100));
	sync.pushPrimary(makePrimary(200));
	ASSERT_TRUE(sync.pushAlpha(makeAlpha(200)).pair.has_value());
	EXPECT_EQ(sync.pendingPrimaryCount(), 0u);

	const auto lateAlpha = sync.pushAlpha(makeAlpha(100));
	EXPECT_TRUE(lateAlpha.rejectedIncomingFrame);
	EXPECT_FALSE(lateAlpha.pair.has_value());
}

TEST(AlphaSyncTest, ExactPairingContinuesAcrossRtpTimestampWrap)
{
	AlphaFrameSynchronizer sync(8, 100);
	sync.pushPrimary(makePrimary(0xFFFFFFF0u));
	ASSERT_TRUE(sync.pushAlpha(makeAlpha(0xFFFFFFF0u)).pair.has_value());

	sync.pushAlpha(makeAlpha(0x00000010u));
	const auto wrappedPair = sync.pushPrimary(makePrimary(0x00000010u));
	ASSERT_TRUE(wrappedPair.pair.has_value());
	EXPECT_EQ(wrappedPair.pair->primary.rtpTimestamp, 0x00000010u);
}

TEST(AlphaSyncTest, TimestampAgeEvictionContinuesAcrossRtpWrap)
{
	AlphaFrameSynchronizer sync(8, 100);
	sync.pushPrimary(makePrimary(0xFFFFFFF0u));
	const auto advanced = sync.pushPrimary(makePrimary(0x00000100u));
	EXPECT_EQ(advanced.droppedPrimaryFrames, 1u);
	EXPECT_EQ(sync.pendingPrimaryCount(), 1u);

	const auto lateOldAlpha = sync.pushAlpha(makeAlpha(0xFFFFFFF0u));
	EXPECT_TRUE(lateOldAlpha.rejectedIncomingFrame);
	EXPECT_FALSE(lateOldAlpha.pair.has_value());
}

TEST(AlphaSyncTest, ResetClearsBothQueuesAndTimestampHistory)
{
	AlphaFrameSynchronizer sync;
	sync.pushPrimary(makePrimary(100));
	sync.pushAlpha(makeAlpha(200));
	const uint64_t oldGeneration = sync.generation();
	sync.reset();

	EXPECT_EQ(sync.pendingPrimaryCount(), 0u);
	EXPECT_EQ(sync.pendingAlphaCount(), 0u);
	EXPECT_FALSE(sync.isCurrentGeneration(oldGeneration));

	// The same timestamp is valid in a new transport/track generation.
	sync.pushPrimary(makePrimary(100));
	const auto pair = sync.pushAlpha(makeAlpha(100));
	ASSERT_TRUE(pair.pair.has_value());
	EXPECT_TRUE(sync.isCurrentGeneration(pair.pair->generation));
}

TEST(AlphaSyncTest, CapacityEvictsOldestArrivalFromEachBoundedQueue)
{
	AlphaFrameSynchronizer sync(2, 10000);
	sync.pushPrimary(makePrimary(100));
	sync.pushPrimary(makePrimary(200));
	const auto primaryOverflow = sync.pushPrimary(makePrimary(300));
	EXPECT_EQ(primaryOverflow.droppedPrimaryFrames, 1u);
	EXPECT_EQ(sync.pendingPrimaryCount(), 2u);

	sync.pushAlpha(makeAlpha(400));
	sync.pushAlpha(makeAlpha(500));
	const auto alphaOverflow = sync.pushAlpha(makeAlpha(600));
	EXPECT_EQ(alphaOverflow.droppedAlphaFrames, 1u);
	EXPECT_EQ(sync.pendingAlphaCount(), 2u);

	// Timestamp 100 was evicted; it cannot be resurrected by a late mask.
	EXPECT_FALSE(sync.pushAlpha(makeAlpha(100)).pair.has_value());
}

TEST(AlphaSyncTest, DimensionMismatchStillPairsOnlyTheExactTimestamp)
{
	AlphaFrameSynchronizer sync;
	sync.pushPrimary(makePrimary(100, 640, 360));
	const auto result = sync.pushAlpha(makeAlpha(100, 320, 180, 320));
	ASSERT_TRUE(result.pair.has_value());
	EXPECT_FALSE(result.pair->dimensionsMatch());
}

TEST(AlphaSyncTest, NoStaleAlphaReuseAcrossConsecutivePrimaryFrames)
{
	AlphaFrameSynchronizer sync;
	sync.pushAlpha(makeAlpha(100, 640, 360, 640, 0x11));
	ASSERT_TRUE(sync.pushPrimary(makePrimary(100)).pair.has_value());

	const auto missingAlpha = sync.pushPrimary(makePrimary(200));
	EXPECT_FALSE(missingAlpha.pair.has_value());
	const auto unrelatedAlpha = sync.pushAlpha(makeAlpha(300, 640, 360, 640, 0x33));
	EXPECT_FALSE(unrelatedAlpha.pair.has_value());
	EXPECT_EQ(sync.pendingPrimaryCount(), 1u);
	EXPECT_EQ(sync.pendingAlphaCount(), 1u);
}

TEST(AlphaSyncTest, ScalesDimensionMismatchedAlphaPlaneWithStride)
{
	const std::vector<uint8_t> source = {1, 2, 0xEE, 3, 4, 0xEE};
	std::vector<uint8_t> scaled;
	ASSERT_TRUE(scaleAlphaPlaneNearest(source, 2, 2, 3, 4, 4, scaled));
	const std::vector<uint8_t> expected = {1, 1, 2, 2, 1, 1, 2, 2, 3, 3, 4, 4, 3, 3, 4, 4};
	EXPECT_EQ(scaled, expected);
}

TEST(AlphaSyncTest, RejectsInvalidAlphaPlaneDimensionsAndStride)
{
	std::vector<uint8_t> output;
	EXPECT_FALSE(scaleAlphaPlaneNearest({1, 2, 3, 4}, 2, 2, 1, 4, 4, output));
	EXPECT_FALSE(scaleAlphaPlaneNearest({1, 2, 3}, 2, 2, 2, 4, 4, output));
}
