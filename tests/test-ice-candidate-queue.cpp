/*
 * Unit tests for remote ICE candidate ordering and session isolation
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-ice-candidate-queue.h"

using namespace vdoninja;

namespace
{

PendingRemoteIceCandidate makeCandidate(const char *value, const char *session, int64_t queuedAtMs)
{
	return {value, "video", session, queuedAtMs};
}

} // namespace

TEST(PendingRemoteIceCandidateQueueTest, DrainsCandidatesThatArrivedBeforePeerCreation)
{
	PendingRemoteIceCandidateQueue queue;
	queue.push("peer-1", makeCandidate("candidate:1", "session-1", 100));
	queue.push("peer-1", makeCandidate("candidate:2", "", 101));

	const auto candidates = queue.takeCompatible("peer-1", "session-1", 200);

	ASSERT_EQ(candidates.size(), 2u);
	EXPECT_EQ(candidates[0].candidate, "candidate:1");
	EXPECT_EQ(candidates[1].candidate, "candidate:2");
	EXPECT_EQ(queue.size("peer-1"), 0u);
}

TEST(PendingRemoteIceCandidateQueueTest, KeepsCandidatesForAQueuedFutureSession)
{
	PendingRemoteIceCandidateQueue queue;
	queue.push("peer-1", makeCandidate("candidate:old", "session-1", 100));
	queue.push("peer-1", makeCandidate("candidate:new", "session-2", 101));

	const auto oldCandidates = queue.takeCompatible("peer-1", "session-1", 200);
	ASSERT_EQ(oldCandidates.size(), 1u);
	EXPECT_EQ(oldCandidates[0].candidate, "candidate:old");
	EXPECT_EQ(queue.size("peer-1"), 1u);

	const auto newCandidates = queue.takeCompatible("peer-1", "session-2", 201);
	ASSERT_EQ(newCandidates.size(), 1u);
	EXPECT_EQ(newCandidates[0].candidate, "candidate:new");
}

TEST(PendingRemoteIceCandidateQueueTest, DropsOldestCandidateAtPerPeerCap)
{
	PendingRemoteIceCandidateQueue queue(2, 30000);
	EXPECT_FALSE(queue.push("peer-1", makeCandidate("candidate:1", "", 100)).droppedQueuedData);
	EXPECT_FALSE(queue.push("peer-1", makeCandidate("candidate:2", "", 101)).droppedQueuedData);
	EXPECT_TRUE(queue.push("peer-1", makeCandidate("candidate:3", "", 102)).droppedQueuedData);

	const auto candidates = queue.takeCompatible("peer-1", "", 200);
	ASSERT_EQ(candidates.size(), 2u);
	EXPECT_EQ(candidates[0].candidate, "candidate:2");
	EXPECT_EQ(candidates[1].candidate, "candidate:3");
}

TEST(PendingRemoteIceCandidateQueueTest, ExpiresStaleCandidates)
{
	PendingRemoteIceCandidateQueue queue(100, 1000);
	queue.push("peer-1", makeCandidate("candidate:stale", "", 100));

	EXPECT_TRUE(queue.takeCompatible("peer-1", "", 1101).empty());
	EXPECT_EQ(queue.size("peer-1"), 0u);
}

TEST(PendingRemoteIceCandidateQueueTest, SweepsExpiredCandidatesAcrossPeerIds)
{
	PendingRemoteIceCandidateQueue queue(100, 1000);
	queue.push("stale-peer", makeCandidate("candidate:stale", "", 100));
	queue.push("fresh-peer", makeCandidate("candidate:fresh", "", 1101));

	EXPECT_EQ(queue.peerCount(), 1u);
	EXPECT_EQ(queue.size("stale-peer"), 0u);
	EXPECT_EQ(queue.size("fresh-peer"), 1u);
}

TEST(PendingRemoteIceCandidateQueueTest, EvictsOldestPeerAtGlobalPeerCap)
{
	PendingRemoteIceCandidateQueue queue(100, 30000, 2);
	queue.push("peer-1", makeCandidate("candidate:1", "", 100));
	queue.push("peer-2", makeCandidate("candidate:2", "", 101));
	const auto result = queue.push("peer-3", makeCandidate("candidate:3", "", 102));

	EXPECT_TRUE(result.accepted);
	EXPECT_TRUE(result.droppedQueuedData);
	EXPECT_EQ(queue.peerCount(), 2u);
	EXPECT_EQ(queue.size("peer-1"), 0u);
	EXPECT_EQ(queue.size("peer-3"), 1u);
}

TEST(PendingRemoteIceCandidateQueueTest, BoundsTotalQueuedBytes)
{
	PendingRemoteIceCandidateQueue queue(100, 30000, 100, 30, 100);
	queue.push("p1", makeCandidate("1234567890", "", 100));
	const auto result = queue.push("p2", makeCandidate("abcdefghij", "", 101));

	EXPECT_TRUE(result.accepted);
	EXPECT_TRUE(result.droppedQueuedData);
	EXPECT_LE(queue.queuedBytes(), 30u);
}

TEST(PendingRemoteIceCandidateQueueTest, RejectsOversizedCandidate)
{
	PendingRemoteIceCandidateQueue queue(100, 30000, 100, 1000, 8);
	const auto result = queue.push("peer-1", makeCandidate("candidate:too-large", "", 100));

	EXPECT_FALSE(result.accepted);
	EXPECT_EQ(queue.peerCount(), 0u);
	EXPECT_EQ(queue.queuedBytes(), 0u);
}

TEST(PendingRemoteIceCandidateQueueTest, ErasesAllCandidatesForPeer)
{
	PendingRemoteIceCandidateQueue queue;
	queue.push("peer-1", makeCandidate("candidate:1", "", 100));
	queue.erase("peer-1");

	EXPECT_EQ(queue.size("peer-1"), 0u);
	EXPECT_EQ(queue.peerCount(), 0u);
	EXPECT_EQ(queue.queuedBytes(), 0u);
}
