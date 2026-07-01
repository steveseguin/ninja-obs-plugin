/*
 * Unit tests for system CPU sampling helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "vdoninja-system-cpu.h"

using namespace vdoninja;

TEST(SystemCpuTest, ComputesBusyPercentFromIdleAndTotalDeltas)
{
	const std::optional<double> usage = computeSystemCpuPercent({100, 1000}, {150, 1100});

	ASSERT_TRUE(usage);
	EXPECT_DOUBLE_EQ(*usage, 50.0);
}

TEST(SystemCpuTest, ComputesZeroWhenAllDeltaIsIdle)
{
	const std::optional<double> usage = computeSystemCpuPercent({100, 1000}, {200, 1100});

	ASSERT_TRUE(usage);
	EXPECT_DOUBLE_EQ(*usage, 0.0);
}

TEST(SystemCpuTest, ComputesHundredWhenNoDeltaIsIdle)
{
	const std::optional<double> usage = computeSystemCpuPercent({100, 1000}, {100, 1100});

	ASSERT_TRUE(usage);
	EXPECT_DOUBLE_EQ(*usage, 100.0);
}

TEST(SystemCpuTest, RejectsZeroOrInvalidDeltas)
{
	EXPECT_FALSE(computeSystemCpuPercent({100, 1000}, {100, 1000}));
	EXPECT_FALSE(computeSystemCpuPercent({100, 1000}, {250, 1100}));
}

TEST(SystemCpuTest, RejectsBackwardsCounters)
{
	EXPECT_FALSE(computeSystemCpuPercent({100, 1000}, {90, 1100}));
	EXPECT_FALSE(computeSystemCpuPercent({100, 1000}, {100, 900}));
}

TEST(SystemCpuTest, MapsThresholdColors)
{
	EXPECT_STREQ(systemCpuStatusColor(69.9), "#cccccc");
	EXPECT_STREQ(systemCpuStatusColor(70.0), "#d6b600");
	EXPECT_STREQ(systemCpuStatusColor(84.9), "#d6b600");
	EXPECT_STREQ(systemCpuStatusColor(85.0), "#ff9900");
	EXPECT_STREQ(systemCpuStatusColor(89.9), "#ff9900");
	EXPECT_STREQ(systemCpuStatusColor(90.0), "#ff3333");
}

TEST(SystemCpuTest, ReadsMonotonicSystemTimesOnSupportedPlatforms)
{
	const std::optional<SystemCpuTimes> first = readSystemCpuTimes();
	if (!first) {
		GTEST_SKIP() << "System CPU sampling is not available on this platform";
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	const std::optional<SystemCpuTimes> second = readSystemCpuTimes();

	ASSERT_TRUE(second);
	EXPECT_GT(first->total, 0u);
	EXPECT_GE(first->total, first->idle);
	EXPECT_GE(second->total, first->total);
	EXPECT_GE(second->idle, first->idle);
	EXPECT_GE(second->total, second->idle);
}

TEST(SystemCpuTest, SamplerWarmsUpThenReturnsBoundedUsage)
{
	SystemCpuSampler sampler;
	const std::optional<double> warmup = sampler.query();
	if (!readSystemCpuTimes()) {
		GTEST_SKIP() << "System CPU sampling is not available on this platform";
	}

	EXPECT_FALSE(warmup);
	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	const std::optional<double> usage = sampler.query();
	ASSERT_TRUE(usage);
	EXPECT_GE(*usage, 0.0);
	EXPECT_LE(*usage, 100.0);
}
