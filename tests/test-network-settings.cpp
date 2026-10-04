#include <gtest/gtest.h>

#include "vdoninja-network-settings.h"

using namespace vdoninja;

TEST(NetworkSettingsTest, AutomaticPreservesExistingPortConfiguration)
{
	for (const auto *text : {"", " ", "Auto", " AUTO "}) {
		const auto range = parseUdpPortRange(text);
		ASSERT_TRUE(range.has_value());
		EXPECT_TRUE(range->automatic);
		EXPECT_EQ(range->first, 1024);
		EXPECT_EQ(range->last, 65535);
	}
}

TEST(NetworkSettingsTest, SinglePortAndInclusiveRange)
{
	const auto single = parseUdpPortRange(" 50000 ");
	ASSERT_TRUE(single.has_value());
	EXPECT_FALSE(single->automatic);
	EXPECT_EQ(single->first, 50000);
	EXPECT_EQ(single->last, 50000);
	const auto range = parseUdpPortRange(" 50000 - 50100 ");
	ASSERT_TRUE(range.has_value());
	EXPECT_EQ(range->first, 50000);
	EXPECT_EQ(range->last, 50100);
	EXPECT_FALSE(range->automatic);
}

TEST(NetworkSettingsTest, AcceptsUdpPortBoundaries)
{
	const auto range = parseUdpPortRange("1-65535");
	ASSERT_TRUE(range.has_value());
	EXPECT_EQ(range->first, 1);
	EXPECT_EQ(range->last, 65535);
	ASSERT_TRUE(parseUdpPortRange("65535").has_value());
	ASSERT_TRUE(parseUdpPortRange("50000-50000").has_value());
}

TEST(NetworkSettingsTest, InvalidRangeNeverFallsBackToAutomatic)
{
	for (const auto *text :
	     {"0", "65536", "-1", "1-0", "0-65535", "50100-50000", "50000-", "-50000", "50000-50001-50002", "50000,50001",
	      "50000junk", "1e4", "+50000", "five", "655350000000000000000000"}) {
		EXPECT_FALSE(parseUdpPortRange(text).has_value()) << text;
	}
}
