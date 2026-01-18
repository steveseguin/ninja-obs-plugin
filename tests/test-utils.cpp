/*
 * Unit tests for vdoninja-utils
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <regex>
#include <set>

#include <gtest/gtest.h>

#include "vdoninja-utils.h"

using namespace vdoninja;

// UUID Generation Tests
class UUIDTest : public ::testing::Test
{
};

TEST_F(UUIDTest, GeneratesValidUUIDFormat)
{
	std::string uuid = generateUUID();

	// UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
	std::regex uuidRegex("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", std::regex::icase);

	EXPECT_TRUE(std::regex_match(uuid, uuidRegex)) << "UUID '" << uuid << "' does not match expected format";
}

TEST_F(UUIDTest, GeneratesUniqueUUIDs)
{
	std::set<std::string> uuids;
	const int numUuids = 1000;

	for (int i = 0; i < numUuids; i++) {
		uuids.insert(generateUUID());
	}

	EXPECT_EQ(uuids.size(), numUuids) << "Expected " << numUuids << " unique UUIDs, got " << uuids.size();
}

TEST_F(UUIDTest, UUIDHasCorrectLength)
{
	std::string uuid = generateUUID();
	EXPECT_EQ(uuid.length(), 36u); // 32 hex chars + 4 dashes
}

// Session ID Generation Tests
class SessionIdTest : public ::testing::Test
{
};

TEST_F(SessionIdTest, GeneratesCorrectLength)
{
	std::string sessionId = generateSessionId();
	EXPECT_EQ(sessionId.length(), 8u);
}

TEST_F(SessionIdTest, ContainsOnlyAlphanumeric)
{
	std::string sessionId = generateSessionId();

	for (char c : sessionId) {
		bool isValid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
		EXPECT_TRUE(isValid) << "Character '" << c << "' is not alphanumeric lowercase";
	}
}

TEST_F(SessionIdTest, GeneratesUniqueSessionIds)
{
	std::set<std::string> sessionIds;
	const int numIds = 1000;

	for (int i = 0; i < numIds; i++) {
		sessionIds.insert(generateSessionId());
	}

	// Allow for some collisions since it's only 8 chars, but should be rare
	EXPECT_GT(sessionIds.size(), numIds - 10);
}

// SHA256 Hashing Tests
class SHA256Test : public ::testing::Test
{
};

TEST_F(SHA256Test, HashesEmptyString)
{
	std::string hash = sha256("");
	// Known SHA256 of empty string
	EXPECT_EQ(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(SHA256Test, HashesHelloWorld)
{
	std::string hash = sha256("hello world");
	// Known SHA256 of "hello world"
	EXPECT_EQ(hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_F(SHA256Test, DifferentInputsProduceDifferentHashes)
{
	std::string hash1 = sha256("input1");
	std::string hash2 = sha256("input2");

	EXPECT_NE(hash1, hash2);
}

TEST_F(SHA256Test, SameInputProducesSameHash)
{
	std::string hash1 = sha256("consistent");
	std::string hash2 = sha256("consistent");

	EXPECT_EQ(hash1, hash2);
}

TEST_F(SHA256Test, HashHasCorrectLength)
{
	std::string hash = sha256("test");
	EXPECT_EQ(hash.length(), 64u); // 256 bits = 64 hex chars
}

// Stream ID Sanitization Tests
class SanitizeStreamIdTest : public ::testing::Test
{
};

TEST_F(SanitizeStreamIdTest, LowercasesInput)
{
	EXPECT_EQ(sanitizeStreamId("HELLO"), "hello");
	EXPECT_EQ(sanitizeStreamId("HeLLo"), "hello");
}

TEST_F(SanitizeStreamIdTest, PreservesAlphanumeric)
{
	EXPECT_EQ(sanitizeStreamId("abc123"), "abc123");
}

TEST_F(SanitizeStreamIdTest, PreservesUnderscores)
{
	EXPECT_EQ(sanitizeStreamId("test_stream"), "test_stream");
}

TEST_F(SanitizeStreamIdTest, ReplacesSpecialChars)
{
	EXPECT_EQ(sanitizeStreamId("test-stream"), "test_stream");
	EXPECT_EQ(sanitizeStreamId("test.stream"), "test_stream");
	EXPECT_EQ(sanitizeStreamId("test stream"), "test_stream");
	EXPECT_EQ(sanitizeStreamId("test@stream!"), "test_stream_");
}

TEST_F(SanitizeStreamIdTest, HandlesEmptyString)
{
	EXPECT_EQ(sanitizeStreamId(""), "");
}

// Stream ID Hashing Tests
class HashStreamIdTest : public ::testing::Test
{
};

TEST_F(HashStreamIdTest, ReturnsRawIdWhenNoPassword)
{
	std::string result = hashStreamId("mystream", "", "salt");
	EXPECT_EQ(result, "mystream");
}

TEST_F(HashStreamIdTest, HashesWithPassword)
{
	std::string result = hashStreamId("mystream", "password", "salt");
	// Should be first 16 chars of sha256(mystream + password + salt)
	EXPECT_EQ(result.length(), 16u);
	EXPECT_NE(result, "mystream");
}

TEST_F(HashStreamIdTest, SanitizesBeforeHashing)
{
	std::string result = hashStreamId("My-Stream", "password", "salt");
	// Input should be sanitized to "my_stream" before hashing
	std::string expected = hashStreamId("my_stream", "password", "salt");
	EXPECT_EQ(result, expected);
}

TEST_F(HashStreamIdTest, DifferentPasswordsProduceDifferentHashes)
{
	std::string hash1 = hashStreamId("stream", "pass1", "salt");
	std::string hash2 = hashStreamId("stream", "pass2", "salt");

	EXPECT_NE(hash1, hash2);
}

// Room ID Hashing Tests
class HashRoomIdTest : public ::testing::Test
{
};

TEST_F(HashRoomIdTest, ReturnsRawIdWhenNoPassword)
{
	std::string result = hashRoomId("myroom", "", "salt");
	EXPECT_EQ(result, "myroom");
}

TEST_F(HashRoomIdTest, HashesWithPassword)
{
	std::string result = hashRoomId("myroom", "password", "salt");
	EXPECT_EQ(result.length(), 16u);
}

// Base64 Encoding/Decoding Tests
class Base64Test : public ::testing::Test
{
};

TEST_F(Base64Test, EncodesEmptyVector)
{
	std::vector<uint8_t> data;
	EXPECT_EQ(base64Encode(data), "");
}

TEST_F(Base64Test, EncodesHelloWorld)
{
	std::string input = "Hello, World!";
	std::vector<uint8_t> data(input.begin(), input.end());

	EXPECT_EQ(base64Encode(data), "SGVsbG8sIFdvcmxkIQ==");
}

TEST_F(Base64Test, EncodesSingleByte)
{
	std::vector<uint8_t> data = {0x4D}; // 'M'
	EXPECT_EQ(base64Encode(data), "TQ==");
}

TEST_F(Base64Test, EncodesTwoBytes)
{
	std::vector<uint8_t> data = {0x4D, 0x61}; // 'Ma'
	EXPECT_EQ(base64Encode(data), "TWE=");
}

TEST_F(Base64Test, EncodesThreeBytes)
{
	std::vector<uint8_t> data = {0x4D, 0x61, 0x6E}; // 'Man'
	EXPECT_EQ(base64Encode(data), "TWFu");
}

TEST_F(Base64Test, DecodesEmptyString)
{
	auto result = base64Decode("");
	EXPECT_TRUE(result.empty());
}

TEST_F(Base64Test, DecodesHelloWorld)
{
	auto result = base64Decode("SGVsbG8sIFdvcmxkIQ==");
	std::string decoded(result.begin(), result.end());

	EXPECT_EQ(decoded, "Hello, World!");
}

TEST_F(Base64Test, RoundTrip)
{
	std::string original = "Test data for round-trip encoding!";
	std::vector<uint8_t> data(original.begin(), original.end());

	std::string encoded = base64Encode(data);
	auto decoded = base64Decode(encoded);
	std::string result(decoded.begin(), decoded.end());

	EXPECT_EQ(result, original);
}

TEST_F(Base64Test, RoundTripBinaryData)
{
	std::vector<uint8_t> data = {0x00, 0xFF, 0x7F, 0x80, 0x01, 0xFE};

	std::string encoded = base64Encode(data);
	auto decoded = base64Decode(encoded);

	EXPECT_EQ(decoded, data);
}

// URL Encoding Tests
class URLEncodeTest : public ::testing::Test
{
};

TEST_F(URLEncodeTest, PreservesAlphanumeric)
{
	EXPECT_EQ(urlEncode("abc123"), "abc123");
	EXPECT_EQ(urlEncode("ABC"), "ABC");
}

TEST_F(URLEncodeTest, PreservesUnreservedChars)
{
	EXPECT_EQ(urlEncode("-_.~"), "-_.~");
}

TEST_F(URLEncodeTest, EncodesSpaces)
{
	EXPECT_EQ(urlEncode("hello world"), "hello%20world");
}

TEST_F(URLEncodeTest, EncodesSpecialChars)
{
	EXPECT_EQ(urlEncode("foo=bar"), "foo%3dbar");
	EXPECT_EQ(urlEncode("foo&bar"), "foo%26bar");
	EXPECT_EQ(urlEncode("foo?bar"), "foo%3fbar");
}

TEST_F(URLEncodeTest, EncodesSlashes)
{
	EXPECT_EQ(urlEncode("path/to/file"), "path%2fto%2ffile");
}

// String Trim Tests
class TrimTest : public ::testing::Test
{
};

TEST_F(TrimTest, TrimsLeadingSpaces)
{
	EXPECT_EQ(trim("   hello"), "hello");
}

TEST_F(TrimTest, TrimsTrailingSpaces)
{
	EXPECT_EQ(trim("hello   "), "hello");
}

TEST_F(TrimTest, TrimsBothEnds)
{
	EXPECT_EQ(trim("   hello   "), "hello");
}

TEST_F(TrimTest, TrimsTabsAndNewlines)
{
	EXPECT_EQ(trim("\t\nhello\r\n"), "hello");
}

TEST_F(TrimTest, HandlesEmptyString)
{
	EXPECT_EQ(trim(""), "");
}

TEST_F(TrimTest, HandlesOnlyWhitespace)
{
	EXPECT_EQ(trim("   \t\n   "), "");
}

TEST_F(TrimTest, PreservesInternalSpaces)
{
	EXPECT_EQ(trim("  hello world  "), "hello world");
}

// String Split Tests
class SplitTest : public ::testing::Test
{
};

TEST_F(SplitTest, SplitsOnComma)
{
	auto result = split("a,b,c", ',');
	ASSERT_EQ(result.size(), 3u);
	EXPECT_EQ(result[0], "a");
	EXPECT_EQ(result[1], "b");
	EXPECT_EQ(result[2], "c");
}

TEST_F(SplitTest, HandlesEmptyString)
{
	auto result = split("", ',');
	ASSERT_EQ(result.size(), 1u);
	EXPECT_EQ(result[0], "");
}

TEST_F(SplitTest, HandlesSingleElement)
{
	auto result = split("single", ',');
	ASSERT_EQ(result.size(), 1u);
	EXPECT_EQ(result[0], "single");
}

TEST_F(SplitTest, HandlesEmptySegments)
{
	auto result = split("a,,b", ',');
	ASSERT_EQ(result.size(), 3u);
	EXPECT_EQ(result[0], "a");
	EXPECT_EQ(result[1], "");
	EXPECT_EQ(result[2], "b");
}

TEST_F(SplitTest, SplitsOnDifferentDelimiters)
{
	auto result = split("a:b:c", ':');
	ASSERT_EQ(result.size(), 3u);
	EXPECT_EQ(result[0], "a");
}

// Time Utilities Tests
class TimeUtilsTest : public ::testing::Test
{
};

TEST_F(TimeUtilsTest, CurrentTimeMsReturnsPositive)
{
	int64_t time = currentTimeMs();
	EXPECT_GT(time, 0);
}

TEST_F(TimeUtilsTest, CurrentTimeMsIncreases)
{
	int64_t time1 = currentTimeMs();
	// Small delay
	for (volatile int i = 0; i < 100000; i++) {}
	int64_t time2 = currentTimeMs();

	EXPECT_GE(time2, time1);
}

TEST_F(TimeUtilsTest, FormatTimestampReturnsNonEmpty)
{
	std::string formatted = formatTimestamp(currentTimeMs());
	EXPECT_FALSE(formatted.empty());
}

TEST_F(TimeUtilsTest, FormatTimestampHasCorrectFormat)
{
	// Timestamp for 2024-01-15 12:30:45 UTC (example)
	int64_t timestamp = 1705321845000;
	std::string formatted = formatTimestamp(timestamp);

	// Should match YYYY-MM-DD HH:MM:SS format
	std::regex formatRegex("^\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}$");
	EXPECT_TRUE(std::regex_match(formatted, formatRegex))
	    << "Formatted timestamp '" << formatted << "' does not match expected format";
}

// SDP Manipulation Tests
class SDPTest : public ::testing::Test
{
};

TEST_F(SDPTest, ModifySdpBitrateAddsBandwidth)
{
	std::string sdp = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 VP8/90000\r\n";
	std::string result = modifySdpBitrate(sdp, 4000000);

	EXPECT_NE(result.find("b=AS:4000"), std::string::npos) << "Expected b=AS line in: " << result;
}

TEST_F(SDPTest, ExtractMidFindsVideoMid)
{
	std::string sdp = "v=0\r\n"
	                  "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
	                  "a=mid:0\r\n"
	                  "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
	                  "a=mid:1\r\n";

	EXPECT_EQ(extractMid(sdp, "video"), "1");
	EXPECT_EQ(extractMid(sdp, "audio"), "0");
}

TEST_F(SDPTest, ExtractMidReturnsEmptyForMissing)
{
	std::string sdp = "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\n";

	EXPECT_EQ(extractMid(sdp, "video"), "");
}
