/* SPDX-License-Identifier: AGPL-3.0-only */
#include <algorithm>

#include <gtest/gtest.h>

#include "vdoninja-common.h"
#include "vdoninja-rtp-utils.h"

using namespace vdoninja;

TEST(VideoCodecs, ColorExtensionPreservesRtpIdentityAndPayload)
{
	const uint8_t payload[]{0x65, 0x21, 0x43};
	const auto raw = buildOpusRtpPacket(payload, sizeof(payload), 96, 65535, 0xfffffff0, 0x11223344);
	std::vector<std::byte> packet(raw.size());
	std::transform(raw.begin(), raw.end(), packet.begin(), [](uint8_t b) { return std::byte{b}; });
	const auto unmarked = packet;
	EXPECT_FALSE(addRtpColorSpaceExtension(packet, 7, {1, 1, 1, 0x10}));
	EXPECT_EQ(packet, unmarked);
	packet[1] |= std::byte{0x80};
	const auto original = packet;
	EXPECT_FALSE(addRtpColorSpaceExtension(packet, 0, {1, 1, 1, 0x10}));
	EXPECT_FALSE(addRtpColorSpaceExtension(packet, 15, {1, 1, 1, 0x10}));
	EXPECT_EQ(packet, original);
	ASSERT_TRUE(addRtpColorSpaceExtension(packet, 7, {1, 1, 1, 0x10}));
	EXPECT_TRUE(std::equal(original.begin() + 1, original.begin() + 12, packet.begin() + 1));
	EXPECT_EQ(packet[16], std::byte{0x73});
	EXPECT_EQ(packet[20], std::byte{0x10});
	const auto view = parseRtpPayloadView(reinterpret_cast<const uint8_t *>(packet.data()), packet.size());
	ASSERT_TRUE(view);
	EXPECT_EQ(view->offset, 24u);
	EXPECT_EQ(view->size, sizeof(payload));
	EXPECT_TRUE(std::equal(original.begin() + 12, original.end(), packet.begin() + view->offset));
	const auto extended = packet;
	EXPECT_FALSE(addRtpColorSpaceExtension(packet, 7, {6, 6, 6, 0x20}));
	EXPECT_EQ(packet, extended);
}

TEST(VideoCodecs, SequenceHeadersDescribeHevcRangeExtensionsAndAv1Profiles)
{
	// SPS with emulation-prevention bytes in profile_tier_level.
	const uint8_t hevc[]{0, 0, 0, 1, 0x42, 1, 1, 4, 0x08, 0, 0, 3, 0, 0x9e, 0x08, 0, 0, 3, 0, 0, 120, 0x80};
	EXPECT_EQ(encodedVideoFormat(ExtendedVideoCodec::HEVC, hevc, sizeof(hevc)),
	          "profile-space=0;profile-id=4;tier-flag=0;level-id=120;interop-constraints=9e0800000000");
	// Normal sequence header, profile 0, no timing, one operating point, level 8/tier 1.
	const uint8_t av1[]{0x0a, 4, 0, 0, 0, 0x44};
	EXPECT_EQ(encodedVideoFormat(ExtendedVideoCodec::AV1, av1, sizeof(av1)), "profile=0;level-idx=8;tier=1");
	const uint8_t reduced[]{0x0a, 2, 0x38, 0};
	EXPECT_EQ(encodedVideoFormat(ExtendedVideoCodec::AV1, reduced, sizeof(reduced)), "profile=1;level-idx=0;tier=0");
	EXPECT_FALSE(encodedVideoFormat(ExtendedVideoCodec::AV1, av1, sizeof(av1) - 1));
	EXPECT_FALSE(encodedVideoFormat(ExtendedVideoCodec::HEVC, hevc, 10));
}

TEST(VideoCodecs, ViewerMustSupportEncodedProfileLevelAndPacketization)
{
	EXPECT_FALSE(offeredVideoFormatSupports(ExtendedVideoCodec::HEVC, "sprop-max-don-diff=garbage", ""));
	EXPECT_FALSE(offeredVideoFormatSupports(ExtendedVideoCodec::HEVC, "profile-id=4;interop-constraints=9e0800000000",
	                                        "profile-id=4;interop-constraints=9c0800000000"));
	EXPECT_TRUE(
	    offeredVideoFormatSupports(ExtendedVideoCodec::HEVC, "profile-id=1;level-id=153", "profile-id=1;level-id=120"));
	EXPECT_FALSE(
	    offeredVideoFormatSupports(ExtendedVideoCodec::HEVC, "profile-id=1;level-id=153", "profile-id=4;level-id=120"));
	EXPECT_FALSE(offeredVideoFormatSupports(ExtendedVideoCodec::HEVC, "sprop-max-don-diff=1", ""));
	EXPECT_FALSE(offeredVideoFormatSupports(ExtendedVideoCodec::AV1, "profile=0;level-idx=8", "profile=1;level-idx=8"));
	EXPECT_FALSE(offeredVideoFormatSupports(ExtendedVideoCodec::AV1, "profile=0;level-idx=5", "profile=0;level-idx=8"));
	EXPECT_TRUE(offeredVideoFormatSupports(ExtendedVideoCodec::AV1, "profile=0;level-idx=8", "profile=0;level-idx=5"));
}

TEST(VideoCodecs, EncoderIdentityUsesObsHevcNameAndRejectsUnimplementedCodecs)
{
	EXPECT_EQ(publishingCodecForEncoder("hevc"), VideoCodec::H265);
	EXPECT_EQ(publishingCodecForEncoder("av1"), VideoCodec::AV1);
	EXPECT_EQ(publishingCodecForEncoder("h264"), VideoCodec::H264);
	EXPECT_FALSE(publishingCodecForEncoder("vp9"));
	EXPECT_FALSE(publishingCodecForEncoder(""));
}

TEST(VideoCodecs, HevcFragmentationPreservesNalHeaderAndAccessUnit)
{
	const std::vector<uint8_t> au{0, 0, 0, 1, 0x40, 1, 0xab, 0, 0, 0, 1, 0x26, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9};
	const auto packets = packetizeHevc(au.data(), au.size(), 7);
	ASSERT_TRUE(packets);
	ASSERT_EQ(packets->size(), 4u);
	EXPECT_EQ((*packets)[0], (std::vector<uint8_t>{0x40, 1, 0xab}));
	EXPECT_EQ((*packets)[1], (std::vector<uint8_t>{0x62, 1, 0x93, 1, 2, 3, 4}));
	EXPECT_EQ((*packets)[3], (std::vector<uint8_t>{0x62, 1, 0x53, 9}));
	VideoRtpAssembler assembler;
	std::optional<std::vector<uint8_t>> result;
	for (size_t i = 0; i < packets->size(); ++i) {
		const auto &packet = (*packets)[i];
		result = assembler.push(ExtendedVideoCodec::HEVC, packet.data(), packet.size(), 9000, i + 1 == packets->size());
		EXPECT_EQ(result.has_value(), i + 1 == packets->size());
	}
	ASSERT_TRUE(result);
	EXPECT_EQ(*result, au);
	EXPECT_TRUE(isHevcKeyframe(result->data(), result->size()));
}

TEST(VideoCodecs, HevcAggregationAndLengthPrefix)
{
	const uint8_t packet[]{0x60, 1, 0, 3, 0x40, 1, 0xab, 0, 3, 0x26, 1, 0xcd};
	VideoRtpAssembler assembler;
	const auto result = assembler.push(ExtendedVideoCodec::HEVC, packet, sizeof(packet), 1, true);
	ASSERT_TRUE(result);
	EXPECT_EQ(*result, (std::vector<uint8_t>{0, 0, 0, 1, 0x40, 1, 0xab, 0, 0, 0, 1, 0x26, 1, 0xcd}));
	const uint8_t lengths[]{0, 0, 0, 3, 0x40, 1, 0xab, 0, 0, 0, 3, 0x26, 1, 0xcd};
	const auto packets = packetizeHevc(lengths, sizeof(lengths), 1000);
	ASSERT_TRUE(packets);
	EXPECT_EQ(packets->size(), 2u);
}

TEST(VideoCodecs, LostHevcFragmentDoesNotDeliverPartialPicture)
{
	VideoRtpAssembler assembler;
	const uint8_t start[]{0x62, 1, 0x93, 4, 5};
	const uint8_t end[]{0x62, 1, 0x53, 6, 7};
	EXPECT_FALSE(assembler.push(ExtendedVideoCodec::HEVC, start, sizeof(start), 1, false));
	assembler.reset(); // Receive queue loss recovery.
	EXPECT_FALSE(assembler.push(ExtendedVideoCodec::HEVC, end, sizeof(end), 1, true));
	EXPECT_FALSE(assembler.push(ExtendedVideoCodec::HEVC, start, sizeof(start), 2, false));
	EXPECT_FALSE(assembler.push(ExtendedVideoCodec::HEVC, end, sizeof(end), 3, true));
	const uint8_t idr[]{0x26, 1, 0xff};
	EXPECT_TRUE(assembler.push(ExtendedVideoCodec::HEVC, idr, sizeof(idr), 4, true));
}

TEST(VideoCodecs, Av1StripsObuSizesForRtpAndRestoresThemForDecoder)
{
	const std::vector<uint8_t> au{0x12, 0, 0x0a, 2, 0x55, 0x66, 0x32, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9};
	const auto packets = packetizeAv1(au.data(), au.size(), 5);
	ASSERT_TRUE(packets);
	ASSERT_EQ(packets->size(), 4u);
	EXPECT_EQ((*packets)[0], (std::vector<uint8_t>{0x18, 0x08, 0x55, 0x66}));
	EXPECT_EQ((*packets)[1], (std::vector<uint8_t>{0x50, 0x30, 1, 2, 3}));
	EXPECT_EQ((*packets)[2], (std::vector<uint8_t>{0xd0, 4, 5, 6, 7}));
	EXPECT_EQ((*packets)[3], (std::vector<uint8_t>{0x90, 8, 9}));
	VideoRtpAssembler assembler;
	std::optional<std::vector<uint8_t>> result;
	for (size_t i = 0; i < packets->size(); ++i) {
		const auto &packet = (*packets)[i];
		result = assembler.push(ExtendedVideoCodec::AV1, packet.data(), packet.size(), 100, i + 1 == packets->size());
	}
	ASSERT_TRUE(result);
	EXPECT_EQ(*result, (std::vector<uint8_t>(au.begin() + 2, au.end())));
	EXPECT_TRUE(av1HasSequenceHeader(result->data(), result->size()));
}

TEST(VideoCodecs, Av1AcceptsBothAggregationLengthModesAndObuExtensions)
{
	for (const auto &packet :
	     std::vector<std::vector<uint8_t>>{{0x28, 3, 0x0c, 0, 0xaa, 0x30, 0xbb},       // W=2, last size implicit.
	                                       {0x08, 3, 0x0c, 0, 0xaa, 2, 0x30, 0xbb}}) { // W=0, every size explicit.
		VideoRtpAssembler assembler;
		const auto result = assembler.push(ExtendedVideoCodec::AV1, packet.data(), packet.size(), 1, true);
		ASSERT_TRUE(result);
		EXPECT_EQ(*result, (std::vector<uint8_t>{0x0e, 0, 1, 0xaa, 0x32, 1, 0xbb}));
	}
}

TEST(VideoCodecs, RejectsMalformedPacketsAndBoundedLengths)
{
	for (const auto &packet : std::vector<std::vector<uint8_t>>{{},
	                                                            {0x18},
	                                                            {0x98, 0x08, 0xaa},
	                                                            {0x90, 0xaa},
	                                                            {0x28, 99, 0x08},
	                                                            {0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
	                                                            {0x10, 0x88, 0xaa},
	                                                            {0x10, 0x0c}}) {
		VideoRtpAssembler assembler;
		EXPECT_FALSE(assembler.push(ExtendedVideoCodec::AV1, packet.data(), packet.size(), 1, true));
	}
	for (const auto &packet : std::vector<std::vector<uint8_t>>{
	         {}, {0x26}, {0x26, 0, 3}, {0x60, 1, 0, 10, 0x26, 1}, {0x62, 1, 0xd3, 2}, {0x62, 1, 0x53, 2}}) {
		VideoRtpAssembler assembler;
		EXPECT_FALSE(assembler.push(ExtendedVideoCodec::HEVC, packet.data(), packet.size(), 1, true));
	}
	const uint8_t badObu[]{0x0a, 0xff, 0xff, 0xff, 0x7f};
	EXPECT_FALSE(packetizeAv1(badObu, sizeof(badObu), 1200));
}

TEST(VideoCodecs, Av1LargeObuReassemblesAcrossLeb128Boundaries)
{
	for (size_t bytes : {127u, 128u, 16383u, 16384u}) {
		std::vector<uint8_t> au{0x32};
		size_t length = bytes;
		do {
			const uint8_t byte = length & 127;
			length >>= 7;
			au.push_back(byte | (length ? 128 : 0));
		} while (length);
		au.insert(au.end(), bytes, 0xab);
		const auto packets = packetizeAv1(au.data(), au.size(), 300);
		ASSERT_TRUE(packets);
		VideoRtpAssembler assembler;
		std::optional<std::vector<uint8_t>> result;
		for (size_t i = 0; i < packets->size(); ++i) {
			const auto &packet = (*packets)[i];
			result = assembler.push(ExtendedVideoCodec::AV1, packet.data(), packet.size(), 1, i + 1 == packets->size());
		}
		ASSERT_TRUE(result);
		EXPECT_EQ(*result, au);
	}
}

TEST(VideoCodecs, MalformedPacketPoisonsOnlyItsOwnTimestamp)
{
	VideoRtpAssembler assembler;
	const uint8_t malformed[]{0x08, 10, 0x30};
	const uint8_t complete[]{0x10, 0x30, 0xab};
	EXPECT_FALSE(assembler.push(ExtendedVideoCodec::AV1, malformed, sizeof(malformed), 1, false));
	EXPECT_FALSE(assembler.push(ExtendedVideoCodec::AV1, complete, sizeof(complete), 1, true));
	EXPECT_TRUE(assembler.push(ExtendedVideoCodec::AV1, complete, sizeof(complete), 2, true));
}
