#include <gtest/gtest.h>

#include "obs-clock-source/counter.h"

TEST(ObsClockFixtureTest, AdvancesFromTheVideoClock)
{
	EXPECT_EQ(obsClockMarker(0, 60, 1), 0u);
	EXPECT_EQ(obsClockMarker(16666667, 60, 1), 1u);
	EXPECT_EQ(obsClockMarker(1000000000, 60, 1), 60u);
}
TEST(ObsClockFixtureTest, SupportsFractionalFrameRates)
{
	EXPECT_EQ(obsClockMarker(1001000000000ULL, 60000, 1001), 60000u);
}
TEST(ObsClockFixtureTest, WrapsLongRunningCountersBeforeNarrowing)
{
	EXPECT_EQ(obsClockMarker(1092250000000ULL, 60, 1), 65535u);
	EXPECT_EQ(obsClockMarker(1092266666667ULL, 60, 1), 0u);
	EXPECT_EQ(obsClockMarker(18000000000000ULL, 60, 1), static_cast<uint16_t>(1080000u));
}
TEST(ObsClockFixtureTest, RejectsInvalidFrameRates)
{
	EXPECT_EQ(obsClockMarker(1000000000, 60, 0), 0u);
	EXPECT_EQ(obsClockMarker(1000000000, 0, 1), 0u);
}
