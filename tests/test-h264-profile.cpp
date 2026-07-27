/*
 * Unit tests for H.264 profile-level-id extraction
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "vdoninja-h264-profile.h"

using namespace vdoninja;

TEST(H264ProfileTest, ExtractsFromAnnexBSps)
{
	const std::vector<uint8_t> data = {0, 0, 0, 1, 0x09, 0x10, 0, 0, 1, 0x67, 0x64, 0x00, 0x2a, 0xaa};

	EXPECT_EQ(deriveH264ProfileLevelId(data.data(), data.size()), "64002a");
}

TEST(H264ProfileTest, ExtractsFromLengthPrefixedSps)
{
	const std::vector<uint8_t> data = {0, 0, 0, 5, 0x67, 0x42, 0xe0, 0x2a, 0xaa};

	EXPECT_EQ(deriveH264ProfileLevelId(data.data(), data.size()), "42e02a");
}

TEST(H264ProfileTest, ExtractsFromDecoderConfigurationRecord)
{
	const std::vector<uint8_t> data = {1, 0x64, 0x00, 0x2a, 0xff, 0xe1, 0};

	EXPECT_EQ(deriveH264ProfileLevelId(data.data(), data.size()), "64002a");
}

TEST(H264ProfileTest, RejectsMalformedOrNonSpsInput)
{
	const std::vector<uint8_t> data = {0x65, 0x64, 0x00, 0x2a};

	EXPECT_FALSE(deriveH264ProfileLevelId(nullptr, 0).has_value());
	EXPECT_FALSE(deriveH264ProfileLevelId(data.data(), data.size()).has_value());
}

TEST(H264ProfileTest, FallbackCoversCommonFrameRates)
{
	EXPECT_EQ(fallbackH264ProfileLevelId(1280, 720, 30), "42e01f");
	EXPECT_EQ(fallbackH264ProfileLevelId(1280, 720, 60), "42e020");
	EXPECT_EQ(fallbackH264ProfileLevelId(1920, 1080, 30), "42e028");
	EXPECT_EQ(fallbackH264ProfileLevelId(1920, 1080, 60), "42e02a");
}

TEST(H264ProfileTest, BuildsValidatedFmtp)
{
	EXPECT_EQ(h264FmtpForProfileLevelId("64002a"),
	          "profile-level-id=64002a;packetization-mode=1;level-asymmetry-allowed=1");
	EXPECT_EQ(h264FmtpForProfileLevelId("invalid"),
	          "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1");
}
