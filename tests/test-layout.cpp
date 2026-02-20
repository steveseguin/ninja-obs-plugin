/*
 * Unit tests for layout helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-layout.h"

using namespace vdoninja;

TEST(LayoutTest, ReturnsEmptyForZeroItems)
{
	auto layout = buildGridLayout(0, 1920, 1080);
	EXPECT_TRUE(layout.empty());
}

TEST(LayoutTest, SingleItemFillsCanvas)
{
	auto layout = buildGridLayout(1, 1920, 1080);
	ASSERT_EQ(layout.size(), 1u);
	EXPECT_FLOAT_EQ(layout[0].x, 0.0f);
	EXPECT_FLOAT_EQ(layout[0].y, 0.0f);
	EXPECT_FLOAT_EQ(layout[0].width, 1920.0f);
	EXPECT_FLOAT_EQ(layout[0].height, 1080.0f);
}

TEST(LayoutTest, FourItemsCreateTwoByTwoGrid)
{
	auto layout = buildGridLayout(4, 1920, 1080);
	ASSERT_EQ(layout.size(), 4u);

	EXPECT_FLOAT_EQ(layout[0].x, 0.0f);
	EXPECT_FLOAT_EQ(layout[0].y, 0.0f);
	EXPECT_FLOAT_EQ(layout[0].width, 960.0f);
	EXPECT_FLOAT_EQ(layout[0].height, 540.0f);

	EXPECT_FLOAT_EQ(layout[1].x, 960.0f);
	EXPECT_FLOAT_EQ(layout[1].y, 0.0f);
	EXPECT_FLOAT_EQ(layout[2].x, 0.0f);
	EXPECT_FLOAT_EQ(layout[2].y, 540.0f);
	EXPECT_FLOAT_EQ(layout[3].x, 960.0f);
	EXPECT_FLOAT_EQ(layout[3].y, 540.0f);
}

TEST(LayoutTest, FiveItemsCreateThreeColumnGrid)
{
	auto layout = buildGridLayout(5, 1920, 1080);
	ASSERT_EQ(layout.size(), 5u);

	EXPECT_FLOAT_EQ(layout[0].width, 640.0f);
	EXPECT_FLOAT_EQ(layout[0].height, 540.0f);
	EXPECT_FLOAT_EQ(layout[3].y, 540.0f);
	EXPECT_FLOAT_EQ(layout[4].x, 640.0f);
	EXPECT_FLOAT_EQ(layout[4].y, 540.0f);
}
