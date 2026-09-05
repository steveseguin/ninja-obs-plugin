/*
 * Unit tests for conservative REMB-driven bitrate adaptation
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <chrono>

#include <gtest/gtest.h>

#include "vdoninja-bitrate-controller.h"

using namespace std::chrono_literals;
using namespace vdoninja;

namespace
{

BitrateControllerConfig controllerConfig()
{
	BitrateControllerConfig config;
	config.minimumBitrateBitsPerSecond = 500000;
	config.maximumBitrateBitsPerSecond = 8000000;
	config.maximumDecreaseStepPercent = 100;
	config.decreaseCooldown = 1s;
	config.increaseCooldown = 5s;
	return config;
}

} // namespace

TEST(BitrateControllerTest, MissingFeedbackNeverChangesBitrate)
{
	BitrateController controller(controllerConfig());

	EXPECT_FALSE(controller.observe(std::nullopt).has_value());
	EXPECT_EQ(controller.currentBitrateBitsPerSecond(), 8000000u);
}

TEST(BitrateControllerTest, FloorSamplesDoNotReportChangesOrDelayRecovery)
{
	BitrateController controller(controllerConfig());
	const auto start = BitrateController::Clock::time_point{};
	ASSERT_FALSE(controller.observe(100000, start).has_value());
	ASSERT_EQ(controller.observe(100000, start + 1s), 500000u);

	EXPECT_FALSE(controller.observe(100000, start + 4s).has_value());
	EXPECT_FALSE(controller.observe(100000, start + 5s).has_value());
	EXPECT_EQ(controller.currentBitrateBitsPerSecond(), 500000u);
	EXPECT_FALSE(controller.observe(8000000, start + 6s).has_value());
	EXPECT_FALSE(controller.observe(8000000, start + 7s).has_value());
	EXPECT_EQ(controller.observe(8000000, start + 8s), 575000u);
}

TEST(BitrateControllerTest, FixedBitrateRangeNeverReportsAChange)
{
	auto config = controllerConfig();
	config.maximumBitrateBitsPerSecond = config.minimumBitrateBitsPerSecond;
	BitrateController controller(config);
	const auto start = BitrateController::Clock::time_point{};
	for (int second = 0; second < 10; ++second) {
		EXPECT_FALSE(controller.observe(100000, start + std::chrono::seconds(second)).has_value());
	}
	EXPECT_EQ(controller.currentBitrateBitsPerSecond(), config.minimumBitrateBitsPerSecond);
}

TEST(BitrateControllerTest, DropsQuicklyWithSafetyMarginAndFloor)
{
	BitrateController controller(controllerConfig());
	const auto start = BitrateController::Clock::time_point{};

	EXPECT_FALSE(controller.observe(4000000, start).has_value());
	EXPECT_EQ(controller.observe(4000000, start + 1s), 3600000u);
	EXPECT_FALSE(controller.observe(1000000, start + 1500ms).has_value());
	EXPECT_EQ(controller.observe(100000, start + 2s), 500000u);
}

TEST(BitrateControllerTest, IgnoresOneTransientLowStartupEstimate)
{
	BitrateController controller(controllerConfig());
	const auto start = BitrateController::Clock::time_point{};

	EXPECT_FALSE(controller.observe(1000000, start).has_value());
	EXPECT_FALSE(controller.observe(8000000, start + 1s).has_value());
	EXPECT_EQ(controller.currentBitrateBitsPerSecond(), 8000000u);
}

TEST(BitrateControllerTest, LimitsEachDecreaseSoTheEncoderAndPacerCanSettle)
{
	BitrateControllerConfig config = controllerConfig();
	config.maximumDecreaseStepPercent = 50;
	BitrateController controller(config);
	const auto start = BitrateController::Clock::time_point{};

	EXPECT_FALSE(controller.observe(800000, start).has_value());
	EXPECT_EQ(controller.observe(800000, start + 1s), 4000000u);
	EXPECT_FALSE(controller.observe(800000, start + 2s).has_value());
	EXPECT_EQ(controller.observe(800000, start + 3s), 2000000u);
	EXPECT_FALSE(controller.observe(800000, start + 4s).has_value());
	EXPECT_EQ(controller.observe(800000, start + 5s), 1000000u);
	EXPECT_FALSE(controller.observe(800000, start + 6s).has_value());
	EXPECT_EQ(controller.observe(800000, start + 7s), 720000u);
}

TEST(BitrateControllerTest, IgnoresSmallEstimateNoise)
{
	BitrateController controller(controllerConfig());
	const auto start = BitrateController::Clock::time_point{};

	EXPECT_FALSE(controller.observe(7900000, start).has_value());
	EXPECT_EQ(controller.currentBitrateBitsPerSecond(), 8000000u);
}

TEST(BitrateControllerTest, RequiresStableSamplesAndCooldownBeforeIncreasing)
{
	BitrateController controller(controllerConfig());
	const auto start = BitrateController::Clock::time_point{};
	ASSERT_FALSE(controller.observe(2000000, start).has_value());
	ASSERT_EQ(controller.observe(2000000, start + 1s), 1800000u);

	EXPECT_FALSE(controller.observe(8000000, start + 2s).has_value());
	EXPECT_FALSE(controller.observe(8000000, start + 3s).has_value());
	EXPECT_FALSE(controller.observe(8000000, start + 4s).has_value());
	EXPECT_EQ(controller.observe(8000000, start + 6s), 2070000u);
}

TEST(BitrateControllerTest, IncreaseIsStepLimited)
{
	BitrateController controller(controllerConfig());
	const auto start = BitrateController::Clock::time_point{};
	ASSERT_FALSE(controller.observe(2000000, start).has_value());
	ASSERT_EQ(controller.observe(2000000, start + 1s), 1800000u);

	EXPECT_FALSE(controller.observe(8000000, start + 6s).has_value());
	EXPECT_FALSE(controller.observe(8000000, start + 7s).has_value());
	EXPECT_EQ(controller.observe(8000000, start + 8s), 2070000u);
}

TEST(BitrateControllerTest, MissingFeedbackBreaksDecreaseConfirmation)
{
	BitrateController controller(controllerConfig());
	const auto start = BitrateController::Clock::time_point{};

	EXPECT_FALSE(controller.observe(2000000, start).has_value());
	EXPECT_FALSE(controller.observe(std::nullopt, start + 1s).has_value());
	EXPECT_FALSE(controller.observe(2000000, start + 2s).has_value());
	EXPECT_EQ(controller.currentBitrateBitsPerSecond(), 8000000u);
}
