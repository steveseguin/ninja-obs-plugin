/*
 * Unit tests for negotiated RFC 2198 audio RED
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <limits>

#include <gtest/gtest.h>

#include "vdoninja-audio-red.h"

using namespace vdoninja;

TEST(AudioRedTest, FirstFrameUsesAPrimaryOnlyRedPayload)
{
	const std::vector<uint8_t> current{0x11, 0x22, 0x33};
	const AudioRedPayload payload =
	    buildAudioRedPayload(current.data(), current.size(), 960, nullptr, 0, 0, kDefaultOpusPayloadType);

	EXPECT_FALSE(payload.includesRedundantBlock);
	EXPECT_EQ(payload.redundantBytes, 0u);
	EXPECT_EQ(payload.bytes, (std::vector<uint8_t>{111, 0x11, 0x22, 0x33}));
}

TEST(AudioRedTest, WrapsThePreviousOpusFrameUsingRfc2198Headers)
{
	const std::vector<uint8_t> previous{0xAA, 0xBB, 0xCC};
	const std::vector<uint8_t> current{0x11, 0x22};
	const AudioRedPayload payload =
	    buildAudioRedPayload(current.data(), current.size(), 1920, previous.data(), previous.size(), 960);

	ASSERT_TRUE(payload.includesRedundantBlock);
	EXPECT_EQ(payload.redundantBytes, previous.size());
	EXPECT_EQ(payload.bytes, (std::vector<uint8_t>{0xEF, 0x0F, 0x00, 0x03, 0x6F, 0xAA, 0xBB, 0xCC, 0x11, 0x22}));
}

TEST(AudioRedTest, TimestampSubtractionSupportsRtpWrapAround)
{
	const std::vector<uint8_t> previous{0xAA};
	const std::vector<uint8_t> current{0x11};
	const AudioRedPayload payload = buildAudioRedPayload(current.data(), current.size(), 0x000002C0U, previous.data(),
	                                                     previous.size(), 0xFFFFFF00U);

	ASSERT_TRUE(payload.includesRedundantBlock);
	EXPECT_EQ(payload.bytes[1], 0x0FU);
	EXPECT_EQ(payload.bytes[2] & 0xFCU, 0x00U);
}

TEST(AudioRedTest, OmitsRedundancyRatherThanExceedingTheRtpPayloadBudget)
{
	const std::vector<uint8_t> previous(600, 0xAA);
	const std::vector<uint8_t> current(600, 0x11);
	const AudioRedPayload payload =
	    buildAudioRedPayload(current.data(), current.size(), 1920, previous.data(), previous.size(), 960);

	EXPECT_FALSE(payload.includesRedundantBlock);
	ASSERT_EQ(payload.bytes.size(), current.size() + 1);
	EXPECT_EQ(payload.bytes.front(), 111);
}

TEST(AudioRedTest, PreservesAnOversizedPrimaryFrameInsteadOfDroppingAudio)
{
	const std::vector<uint8_t> previous(100, 0xAA);
	const std::vector<uint8_t> current(1200, 0x11);
	const AudioRedPayload payload =
	    buildAudioRedPayload(current.data(), current.size(), 1920, previous.data(), previous.size(), 960);

	EXPECT_FALSE(payload.includesRedundantBlock);
	ASSERT_EQ(payload.bytes.size(), current.size() + 1);
	EXPECT_EQ(payload.bytes.front(), 111);
}

TEST(AudioRedTest, RejectsInvalidCurrentPayload)
{
	EXPECT_TRUE(buildAudioRedPayload(nullptr, 4, 960, nullptr, 0, 0).bytes.empty());
}

TEST(AudioRedTest, RejectsImpossiblePayloadSizeWithoutOverflowingHeaderArithmetic)
{
	const uint8_t current = 0x11;
	EXPECT_TRUE(
	    buildAudioRedPayload(&current, std::numeric_limits<size_t>::max() - 1, 960, nullptr, 0, 0).bytes.empty());
}

TEST(AudioRedTest, SelectsRedOnlyWhenAnswerPrefersAValidMapping)
{
	const std::string redFirst = "v=0\r\n"
	                             "m=audio 9 UDP/TLS/RTP/SAVPF 63 111\r\n"
	                             "a=mid:audio\r\n"
	                             "a=rtpmap:63 red/48000/2\r\n"
	                             "a=fmtp:63 111/111\r\n"
	                             "a=rtpmap:111 opus/48000/2\r\n";
	const std::string opusFirst = "v=0\r\n"
	                              "m=audio 9 UDP/TLS/RTP/SAVPF 111 63\r\n"
	                              "a=rtpmap:63 red/48000/2\r\n"
	                              "a=fmtp:63 111/111\r\n"
	                              "a=rtpmap:111 opus/48000/2\r\n";
	const std::string invalidMapping = "v=0\r\n"
	                                   "m=audio 9 UDP/TLS/RTP/SAVPF 63 111\r\n"
	                                   "a=rtpmap:63 red/48000/2\r\n"
	                                   "a=fmtp:63 109/109\r\n"
	                                   "a=rtpmap:111 opus/48000/2\r\n";

	EXPECT_TRUE(answerSelectsAudioRed(redFirst));
	EXPECT_FALSE(answerSelectsAudioRed(opusFirst));
	EXPECT_FALSE(answerSelectsAudioRed(invalidMapping));
	EXPECT_FALSE(answerSelectsAudioRed("v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
	                                   "a=rtpmap:111 opus/48000/2\r\n"));
}
