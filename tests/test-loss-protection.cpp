/*
 * Unit tests for opt-in video loss-protection policy
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-loss-protection.h"

using namespace vdoninja;

TEST(VideoProtectionPolicyTest, InvalidStoredValueFailsClosed)
{
	EXPECT_EQ(videoProtectionModeFromInt(-1), VideoProtectionMode::Off);
	EXPECT_EQ(videoProtectionModeFromInt(99), VideoProtectionMode::Off);
}

TEST(VideoProtectionPolicyTest, OffNeverDuplicates)
{
	EXPECT_FALSE(shouldDuplicateVideoPacket(VideoProtectionMode::Off, true, 100));
	EXPECT_FALSE(shouldDuplicateVideoPacket(VideoProtectionMode::Off, false, 100));
}

TEST(VideoProtectionPolicyTest, LowProtectsOnlyKeyframes)
{
	EXPECT_TRUE(shouldDuplicateVideoPacket(VideoProtectionMode::Low, true, 101));
	EXPECT_FALSE(shouldDuplicateVideoPacket(VideoProtectionMode::Low, false, 100));
	EXPECT_EQ(videoProtectionPolicy(VideoProtectionMode::Low).duplicateBudgetPercent, 20);
}

TEST(VideoProtectionPolicyTest, MediumProtectsKeyframesAndQuarterOfDeltaPackets)
{
	EXPECT_TRUE(shouldDuplicateVideoPacket(VideoProtectionMode::Medium, true, 101));
	EXPECT_TRUE(shouldDuplicateVideoPacket(VideoProtectionMode::Medium, false, 100));
	EXPECT_FALSE(shouldDuplicateVideoPacket(VideoProtectionMode::Medium, false, 101));
	EXPECT_EQ(videoProtectionPolicy(VideoProtectionMode::Medium).duplicateBudgetPercent, 50);
}

TEST(VideoProtectionPolicyTest, HighProtectsEveryPacket)
{
	EXPECT_TRUE(shouldDuplicateVideoPacket(VideoProtectionMode::High, true, 101));
	EXPECT_TRUE(shouldDuplicateVideoPacket(VideoProtectionMode::High, false, 101));
	EXPECT_EQ(videoProtectionPolicy(VideoProtectionMode::High).duplicateBudgetPercent, 100);
}

TEST(VideoProtectionPolicyTest, DuplicateBudgetIncludesBoundedHighModePacketizationHeadroom)
{
	EXPECT_EQ(videoProtectionBitrateForEncoderRate(8000000, VideoProtectionMode::Off), 0u);
	EXPECT_EQ(videoProtectionBitrateForEncoderRate(8000000, VideoProtectionMode::Low), 1600000u);
	EXPECT_EQ(videoProtectionBitrateForEncoderRate(8000000, VideoProtectionMode::Medium), 4000000u);
	EXPECT_EQ(videoProtectionBitrateForEncoderRate(8000000, VideoProtectionMode::High), 8400000u);
}
