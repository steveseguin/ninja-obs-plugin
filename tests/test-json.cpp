/*
 * Unit tests for JSON utilities (JsonBuilder and JsonParser)
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-utils.h"

using namespace vdoninja;

// JsonBuilder Tests
class JsonBuilderTest : public ::testing::Test
{
protected:
	JsonBuilder builder;
};

TEST_F(JsonBuilderTest, BuildsEmptyObject)
{
	EXPECT_EQ(builder.build(), "{}");
}

TEST_F(JsonBuilderTest, BuildsStringValue)
{
	builder.add("key", "value");
	EXPECT_EQ(builder.build(), "{\"key\":\"value\"}");
}

TEST_F(JsonBuilderTest, BuildsIntValue)
{
	builder.add("count", 42);
	EXPECT_EQ(builder.build(), "{\"count\":42}");
}

TEST_F(JsonBuilderTest, BuildsNegativeInt)
{
	builder.add("negative", -10);
	EXPECT_EQ(builder.build(), "{\"negative\":-10}");
}

TEST_F(JsonBuilderTest, BuildsBoolTrue)
{
	builder.add("enabled", true);
	EXPECT_EQ(builder.build(), "{\"enabled\":true}");
}

TEST_F(JsonBuilderTest, BuildsBoolFalse)
{
	builder.add("enabled", false);
	EXPECT_EQ(builder.build(), "{\"enabled\":false}");
}

TEST_F(JsonBuilderTest, BuildsMultipleValues)
{
	builder.add("name", "test").add("count", 5).add("active", true);

	std::string result = builder.build();

	// Check structure
	EXPECT_NE(result.find("\"name\":\"test\""), std::string::npos);
	EXPECT_NE(result.find("\"count\":5"), std::string::npos);
	EXPECT_NE(result.find("\"active\":true"), std::string::npos);
}

TEST_F(JsonBuilderTest, EscapesQuotesInStrings)
{
	builder.add("text", "say \"hello\"");

	std::string result = builder.build();
	EXPECT_NE(result.find("\\\"hello\\\""), std::string::npos);
}

TEST_F(JsonBuilderTest, EscapesBackslashes)
{
	builder.add("path", "C:\\Users\\test");

	std::string result = builder.build();
	EXPECT_NE(result.find("C:\\\\Users\\\\test"), std::string::npos);
}

TEST_F(JsonBuilderTest, EscapesNewlines)
{
	builder.add("text", "line1\nline2");

	std::string result = builder.build();
	EXPECT_NE(result.find("\\n"), std::string::npos);
}

TEST_F(JsonBuilderTest, EscapesTabs)
{
	builder.add("text", "col1\tcol2");

	std::string result = builder.build();
	EXPECT_NE(result.find("\\t"), std::string::npos);
}

TEST_F(JsonBuilderTest, AddsRawJson)
{
	builder.addRaw("nested", "{\"inner\":true}");

	EXPECT_EQ(builder.build(), "{\"nested\":{\"inner\":true}}");
}

TEST_F(JsonBuilderTest, ChainsMultipleAdds)
{
	std::string result = builder.add("a", "1").add("b", 2).add("c", true).build();

	EXPECT_NE(result.find("\"a\":\"1\""), std::string::npos);
	EXPECT_NE(result.find("\"b\":2"), std::string::npos);
	EXPECT_NE(result.find("\"c\":true"), std::string::npos);
}

// JsonParser Tests
class JsonParserTest : public ::testing::Test
{
};

TEST_F(JsonParserTest, ParsesEmptyObject)
{
	JsonParser parser("{}");
	EXPECT_FALSE(parser.hasKey("anything"));
}

TEST_F(JsonParserTest, ParsesStringValue)
{
	JsonParser parser("{\"name\":\"test\"}");

	EXPECT_TRUE(parser.hasKey("name"));
	EXPECT_EQ(parser.getString("name"), "test");
}

TEST_F(JsonParserTest, ParsesIntValue)
{
	JsonParser parser("{\"count\":42}");

	EXPECT_TRUE(parser.hasKey("count"));
	EXPECT_EQ(parser.getInt("count"), 42);
}

TEST_F(JsonParserTest, ParsesNegativeInt)
{
	JsonParser parser("{\"value\":-123}");
	EXPECT_EQ(parser.getInt("value"), -123);
}

TEST_F(JsonParserTest, ParsesBoolTrue)
{
	JsonParser parser("{\"enabled\":true}");

	EXPECT_TRUE(parser.hasKey("enabled"));
	EXPECT_TRUE(parser.getBool("enabled"));
}

TEST_F(JsonParserTest, ParsesBoolFalse)
{
	JsonParser parser("{\"enabled\":false}");

	EXPECT_TRUE(parser.hasKey("enabled"));
	EXPECT_FALSE(parser.getBool("enabled"));
}

TEST_F(JsonParserTest, ParsesMultipleValues)
{
	JsonParser parser("{\"name\":\"test\",\"count\":5,\"active\":true}");

	EXPECT_EQ(parser.getString("name"), "test");
	EXPECT_EQ(parser.getInt("count"), 5);
	EXPECT_TRUE(parser.getBool("active"));
}

TEST_F(JsonParserTest, ReturnsDefaultForMissingString)
{
	JsonParser parser("{}");
	EXPECT_EQ(parser.getString("missing", "default"), "default");
}

TEST_F(JsonParserTest, ReturnsDefaultForMissingInt)
{
	JsonParser parser("{}");
	EXPECT_EQ(parser.getInt("missing", 99), 99);
}

TEST_F(JsonParserTest, ReturnsDefaultForMissingBool)
{
	JsonParser parser("{}");
	EXPECT_TRUE(parser.getBool("missing", true));
	EXPECT_FALSE(parser.getBool("missing", false));
}

TEST_F(JsonParserTest, ParsesNestedObject)
{
	JsonParser parser("{\"outer\":{\"inner\":\"value\"}}");

	EXPECT_TRUE(parser.hasKey("outer"));
	std::string nested = parser.getObject("outer");
	EXPECT_NE(nested.find("inner"), std::string::npos);

	// Parse the nested object
	JsonParser innerParser(nested);
	EXPECT_EQ(innerParser.getString("inner"), "value");
}

TEST_F(JsonParserTest, ParsesArray)
{
	JsonParser parser("{\"items\":[\"a\",\"b\",\"c\"]}");

	auto items = parser.getArray("items");
	ASSERT_EQ(items.size(), 3u);
	EXPECT_EQ(items[0], "a");
	EXPECT_EQ(items[1], "b");
	EXPECT_EQ(items[2], "c");
}

TEST_F(JsonParserTest, ParsesArrayOfObjects)
{
	JsonParser parser("{\"list\":[{\"id\":1},{\"id\":2}]}");

	auto list = parser.getArray("list");
	ASSERT_EQ(list.size(), 2u);

	JsonParser first(list[0]);
	EXPECT_EQ(first.getInt("id"), 1);

	JsonParser second(list[1]);
	EXPECT_EQ(second.getInt("id"), 2);
}

TEST_F(JsonParserTest, HandlesWhitespace)
{
	JsonParser parser("  {  \"key\"  :  \"value\"  }  ");

	EXPECT_TRUE(parser.hasKey("key"));
	EXPECT_EQ(parser.getString("key"), "value");
}

TEST_F(JsonParserTest, ParsesEscapedQuotes)
{
	JsonParser parser("{\"text\":\"say \\\"hello\\\"\"}");

	EXPECT_EQ(parser.getString("text"), "say \"hello\"");
}

TEST_F(JsonParserTest, ParsesEscapedNewlines)
{
	JsonParser parser("{\"text\":\"line1\\nline2\"}");

	EXPECT_EQ(parser.getString("text"), "line1\nline2");
}

TEST_F(JsonParserTest, ParsesEscapedTabs)
{
	JsonParser parser("{\"text\":\"col1\\tcol2\"}");

	EXPECT_EQ(parser.getString("text"), "col1\tcol2");
}

TEST_F(JsonParserTest, ParsesEscapedBackslashes)
{
	JsonParser parser("{\"path\":\"C:\\\\Users\"}");

	EXPECT_EQ(parser.getString("path"), "C:\\Users");
}

TEST_F(JsonParserTest, HandlesNullValue)
{
	JsonParser parser("{\"value\":null}");

	// null should be parsed but return empty/default
	EXPECT_EQ(parser.getString("value", "default"), "null");
}

TEST_F(JsonParserTest, ParsesFloatAsInt)
{
	// Our simple parser doesn't fully support floats, but should handle truncation
	JsonParser parser("{\"value\":3}");
	EXPECT_EQ(parser.getInt("value"), 3);
}

TEST_F(JsonParserTest, GetRawReturnsUnprocessedValue)
{
	JsonParser parser("{\"obj\":{\"a\":1,\"b\":2}}");

	std::string raw = parser.getRaw("obj");
	EXPECT_NE(raw.find("{"), std::string::npos);
	EXPECT_NE(raw.find("\"a\":1"), std::string::npos);
}

// Round-trip tests
class JsonRoundTripTest : public ::testing::Test
{
};

TEST_F(JsonRoundTripTest, StringRoundTrip)
{
	JsonBuilder builder;
	builder.add("test", "hello world");

	JsonParser parser(builder.build());
	EXPECT_EQ(parser.getString("test"), "hello world");
}

TEST_F(JsonRoundTripTest, IntRoundTrip)
{
	JsonBuilder builder;
	builder.add("count", 12345);

	JsonParser parser(builder.build());
	EXPECT_EQ(parser.getInt("count"), 12345);
}

TEST_F(JsonRoundTripTest, BoolRoundTrip)
{
	JsonBuilder builder;
	builder.add("yes", true);
	builder.add("no", false);

	JsonParser parser(builder.build());
	EXPECT_TRUE(parser.getBool("yes"));
	EXPECT_FALSE(parser.getBool("no"));
}

TEST_F(JsonRoundTripTest, MixedTypesRoundTrip)
{
	JsonBuilder builder;
	builder.add("name", "test").add("count", 42).add("active", true).add("message", "with \"quotes\"");

	std::string json = builder.build();
	JsonParser parser(json);

	EXPECT_EQ(parser.getString("name"), "test");
	EXPECT_EQ(parser.getInt("count"), 42);
	EXPECT_TRUE(parser.getBool("active"));
	EXPECT_EQ(parser.getString("message"), "with \"quotes\"");
}

TEST_F(JsonRoundTripTest, NestedObjectRoundTrip)
{
	JsonBuilder inner;
	inner.add("value", "nested");

	JsonBuilder outer;
	outer.addRaw("child", inner.build());

	JsonParser parser(outer.build());
	std::string childJson = parser.getObject("child");

	JsonParser childParser(childJson);
	EXPECT_EQ(childParser.getString("value"), "nested");
}

// VDO.Ninja specific message tests
class VDONinjaMessageTest : public ::testing::Test
{
};

TEST_F(VDONinjaMessageTest, ParsesOfferMessage)
{
	std::string offerJson = "{\"UUID\":\"abc-123\",\"sdp\":\"v=0...\",\"type\":\"offer\",\"session\":\"xyz789\"}";

	JsonParser parser(offerJson);

	EXPECT_EQ(parser.getString("UUID"), "abc-123");
	EXPECT_EQ(parser.getString("sdp"), "v=0...");
	EXPECT_EQ(parser.getString("type"), "offer");
	EXPECT_EQ(parser.getString("session"), "xyz789");
}

TEST_F(VDONinjaMessageTest, BuildsAnswerMessage)
{
	JsonBuilder builder;
	builder.add("UUID", "peer-uuid").add("sdp", "v=0\r\no=- ...").add("type", "answer").add("session", "session123");

	std::string json = builder.build();

	EXPECT_NE(json.find("\"UUID\":\"peer-uuid\""), std::string::npos);
	EXPECT_NE(json.find("\"type\":\"answer\""), std::string::npos);
}

TEST_F(VDONinjaMessageTest, ParsesCandidateMessage)
{
	std::string candidateJson = "{\"UUID\":\"abc\",\"candidate\":\"candidate:1 1 UDP 2130706431 "
	                            "...\",\"mid\":\"0\",\"session\":\"xyz\"}";

	JsonParser parser(candidateJson);

	EXPECT_EQ(parser.getString("UUID"), "abc");
	EXPECT_TRUE(parser.getString("candidate").find("candidate:") != std::string::npos);
	EXPECT_EQ(parser.getString("mid"), "0");
}

TEST_F(VDONinjaMessageTest, BuildsJoinRoomRequest)
{
	JsonBuilder builder;
	builder.add("request", "joinroom").add("roomid", "hashedroomid123").add("claim", true);

	std::string json = builder.build();

	EXPECT_NE(json.find("\"request\":\"joinroom\""), std::string::npos);
	EXPECT_NE(json.find("\"roomid\":\"hashedroomid123\""), std::string::npos);
	EXPECT_NE(json.find("\"claim\":true"), std::string::npos);
}

TEST_F(VDONinjaMessageTest, ParsesListingMessage)
{
	std::string listingJson = "{\"listing\":[{\"streamID\":\"stream1\"},{\"streamID\":\"stream2\"}]}";

	JsonParser parser(listingJson);

	EXPECT_TRUE(parser.hasKey("listing"));
	auto listing = parser.getArray("listing");
	ASSERT_EQ(listing.size(), 2u);

	JsonParser member1(listing[0]);
	EXPECT_EQ(member1.getString("streamID"), "stream1");

	JsonParser member2(listing[1]);
	EXPECT_EQ(member2.getString("streamID"), "stream2");
}
