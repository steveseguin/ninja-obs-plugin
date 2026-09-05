/*
 * Unit tests for negotiated RFC 2198 audio RED
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <algorithm>
#include <limits>
#include <random>

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

TEST(AudioRedTest, RejectedAudioDoesNotNegotiateRedFromRetainedCodecAttributes)
{
	const std::string codecs = "a=rtpmap:63 red/48000/2\r\n"
	                           "a=fmtp:63 111/111\r\n"
	                           "a=rtpmap:111 opus/48000/2\r\n";
	EXPECT_FALSE(answerSelectsAudioRed("v=0\r\nm=audio 0 UDP/TLS/RTP/SAVPF 63 111\r\n" + codecs));
	EXPECT_TRUE(answerSelectsAudioRed("v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 63 111\r\n" + codecs));
}

TEST(AudioRedTest, RejectsEmptyEntriesAnywhereInRedFormatParameters)
{
	const std::string prefix = "v=0\r\n"
	                           "m=audio 9 UDP/TLS/RTP/SAVPF 63 111\r\n"
	                           "a=rtpmap:63 red/48000/2\r\n"
	                           "a=rtpmap:111 opus/48000/2\r\n"
	                           "a=fmtp:63 ";
	for (const char *parameters : {"/111/111", "111//111", "111/111/", "111/111//"}) {
		EXPECT_FALSE(answerSelectsAudioRed(prefix + parameters + "\r\n")) << parameters;
	}
	EXPECT_TRUE(answerSelectsAudioRed(prefix + "111/111\r\n"));
	EXPECT_TRUE(answerSelectsAudioRed(prefix + "111/111/111\r\n"));
}

TEST(AudioRedFuzzTest, RandomPayloadsPreservePrimaryAndBoundRedundancy)
{
	std::mt19937 rng(0x2198F00D);
	std::uniform_int_distribution<size_t> payloadSizeDist(0, 1400);
	std::uniform_int_distribution<size_t> maximumSizeDist(0, 1800);
	std::uniform_int_distribution<int> byteDist(0, 255);
	std::uniform_int_distribution<int> payloadTypeDist(0, 255);
	std::uniform_int_distribution<uint32_t> offsetDist(0, 20000);

	for (int iteration = 0; iteration < 30000; ++iteration) {
		std::vector<uint8_t> current(payloadSizeDist(rng));
		std::vector<uint8_t> previous(payloadSizeDist(rng));
		for (uint8_t &byte : current) {
			byte = static_cast<uint8_t>(byteDist(rng));
		}
		for (uint8_t &byte : previous) {
			byte = static_cast<uint8_t>(byteDist(rng));
		}

		const uint8_t payloadType = static_cast<uint8_t>(payloadTypeDist(rng));
		const size_t maximumSize = maximumSizeDist(rng);
		const uint32_t currentTimestamp = rng();
		const uint32_t previousTimestamp =
		    iteration % 2 == 0 ? currentTimestamp - offsetDist(rng) : static_cast<uint32_t>(rng());
		const AudioRedPayload payload = buildAudioRedPayload(
		    current.empty() ? nullptr : current.data(), current.size(), currentTimestamp,
		    previous.empty() ? nullptr : previous.data(), previous.size(), previousTimestamp, payloadType, maximumSize);

		if (payloadType > 127 || maximumSize == 0) {
			EXPECT_TRUE(payload.bytes.empty()) << "iteration=" << iteration;
			EXPECT_FALSE(payload.includesRedundantBlock);
			EXPECT_EQ(payload.redundantBytes, 0u);
			continue;
		}

		ASSERT_FALSE(payload.bytes.empty()) << "iteration=" << iteration;
		if (!payload.includesRedundantBlock) {
			EXPECT_EQ(payload.redundantBytes, 0u);
			ASSERT_EQ(payload.bytes.size(), current.size() + 1U);
			EXPECT_EQ(payload.bytes.front(), payloadType);
			EXPECT_TRUE(std::equal(current.begin(), current.end(), payload.bytes.begin() + 1));
			continue;
		}

		ASSERT_EQ(payload.redundantBytes, previous.size());
		ASSERT_EQ(payload.bytes.size(), current.size() + previous.size() + 5U);
		EXPECT_EQ(payload.bytes[0], static_cast<uint8_t>(0x80U | payloadType));
		EXPECT_EQ(payload.bytes[4], payloadType);
		const uint32_t encodedOffset =
		    (static_cast<uint32_t>(payload.bytes[1]) << 6U) | (static_cast<uint32_t>(payload.bytes[2]) >> 2U);
		const size_t encodedLength =
		    (static_cast<size_t>(payload.bytes[2] & 0x03U) << 8U) | static_cast<size_t>(payload.bytes[3]);
		EXPECT_EQ(encodedOffset, currentTimestamp - previousTimestamp);
		EXPECT_EQ(encodedLength, previous.size());
		EXPECT_LE(payload.bytes.size(), maximumSize);
		EXPECT_TRUE(std::equal(previous.begin(), previous.end(), payload.bytes.begin() + 5));
		EXPECT_TRUE(std::equal(current.begin(), current.end(), payload.bytes.begin() + 5 + previous.size()));
	}
}
