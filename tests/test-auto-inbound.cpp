/*
 * Unit tests for auto-inbound room listing reconciliation
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-auto-inbound-state.h"

using namespace vdoninja;

TEST(AutoInboundListingTest, ReconcilesAddsAndMissedRemovals)
{
	const auto delta = reconcileAutoInboundListing({"stale", "steady"}, {}, {"steady", "new"});

	ASSERT_EQ(delta.added.size(), 1u);
	EXPECT_EQ(delta.added[0], "new");
	ASSERT_EQ(delta.removed.size(), 1u);
	EXPECT_EQ(delta.removed[0], "stale");
}

TEST(AutoInboundListingTest, IgnoresOwnEmptyAndDuplicateStreams)
{
	const auto delta = reconcileAutoInboundListing({}, {"self"}, {"", "self", "guest", "guest"});

	ASSERT_EQ(delta.added.size(), 1u);
	EXPECT_EQ(delta.added[0], "guest");
	EXPECT_TRUE(delta.removed.empty());
}

TEST(AutoInboundRemovalGraceStateTest, DoesNotRemoveBeforeGraceDeadline)
{
	AutoInboundRemovalGraceState state(10000);
	state.schedule("guest", 1000);

	EXPECT_TRUE(state.takeDue(10999).empty());
	EXPECT_TRUE(state.contains("guest"));
	EXPECT_EQ(state.nextDeadlineMs(), 11000);
}

TEST(AutoInboundRemovalGraceStateTest, ReturnsStreamAtGraceDeadline)
{
	AutoInboundRemovalGraceState state(10000);
	state.schedule("guest", 1000);

	const auto due = state.takeDue(11000);
	ASSERT_EQ(due.size(), 1u);
	EXPECT_EQ(due[0], "guest");
	EXPECT_FALSE(state.contains("guest"));
}

TEST(AutoInboundRemovalGraceStateTest, ReappearanceCancelsPendingRemoval)
{
	AutoInboundRemovalGraceState state(10000);
	state.schedule("guest", 1000);
	state.cancel("guest");

	EXPECT_TRUE(state.takeDue(20000).empty());
}

TEST(AutoInboundRemovalGraceStateTest, RepeatedMissingListingDoesNotExtendDeadline)
{
	AutoInboundRemovalGraceState state(10000);
	state.schedule("guest", 1000);
	state.schedule("guest", 9000);

	EXPECT_EQ(state.nextDeadlineMs(), 11000);
}
