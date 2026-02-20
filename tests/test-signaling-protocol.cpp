/*
 * Unit tests for signaling protocol normalization
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-signaling-protocol.h"

using namespace vdoninja;

TEST(SignalingProtocolTest, ParsesOfferRequest)
{
	const std::string raw = R"({"request":"offerSDP","UUID":"viewer-1","session":"abc123"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Request);
	EXPECT_EQ(parsed.request, "offerSDP");
	EXPECT_EQ(parsed.uuid, "viewer-1");
	EXPECT_EQ(parsed.session, "abc123");
}

TEST(SignalingProtocolTest, ParsesDescriptionOfferEnvelope)
{
	const std::string raw =
	    R"({"UUID":"peer-7","session":"sess-1","description":{"type":"offer","sdp":"v=0\r\na=mid:0"}})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Offer);
	EXPECT_EQ(parsed.type, "offer");
	EXPECT_EQ(parsed.uuid, "peer-7");
	EXPECT_EQ(parsed.session, "sess-1");
	EXPECT_NE(parsed.sdp.find("a=mid:0"), std::string::npos);
}

TEST(SignalingProtocolTest, ParsesCandidateBundle)
{
	const std::string raw = R"({
		"UUID":"peer-9",
		"session":"sess-2",
		"candidates":[
			{"candidate":"cand-1","mid":"0"},
			{"candidate":"cand-2","mid":"1"}
		]
	})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::CandidatesBundle);
	ASSERT_EQ(parsed.candidates.size(), 2u);
	EXPECT_EQ(parsed.candidates[0].candidate, "cand-1");
	EXPECT_EQ(parsed.candidates[0].mid, "0");
	EXPECT_EQ(parsed.candidates[1].candidate, "cand-2");
	EXPECT_EQ(parsed.candidates[1].mid, "1");
}

TEST(SignalingProtocolTest, ParsesWhepUrlAsStreamIdentifier)
{
	const std::string raw = R"({"videoAddedToRoom":true,"UUID":"peer-w","whepUrl":"https://example.com/whep/streamA"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::VideoAddedToRoom);
	EXPECT_EQ(parsed.streamId, "https://example.com/whep/streamA");
}
