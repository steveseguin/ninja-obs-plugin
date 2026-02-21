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

TEST(SignalingProtocolTest, ParsesPlayRequestAsRequestKind)
{
	const std::string raw = R"({"request":"play","UUID":"viewer-2","session":"sess-2","streamID":"abc123"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Request);
	EXPECT_EQ(parsed.request, "play");
	EXPECT_EQ(parsed.uuid, "viewer-2");
	EXPECT_EQ(parsed.session, "sess-2");
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

TEST(SignalingProtocolTest, ParsesListingRequestWithListArray)
{
	const std::string raw = R"({
		"request":"listing",
		"UUID":"room-host",
		"list":[
			{"streamID":"cam_1"},
			{"whepUrl":"https://example.com/whep/streamB"}
		]
	})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Listing);
	ASSERT_EQ(parsed.listingMembers.size(), 2u);
	EXPECT_EQ(parsed.listingMembers[0], "cam_1");
	EXPECT_EQ(parsed.listingMembers[1], "https://example.com/whep/streamB");
}

TEST(SignalingProtocolTest, ParsesVideoAddedToRoomRequestVariant)
{
	const std::string raw = R"({"request":"videoaddedtoroom","UUID":"peer-a","streamID":"cam_2"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::VideoAddedToRoom);
	EXPECT_EQ(parsed.streamId, "cam_2");
}

TEST(SignalingProtocolTest, ParsesAlertRequestMessageField)
{
	const std::string raw = R"({"request":"alert","message":"Room already claimed"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Alert);
	EXPECT_EQ(parsed.alert, "Room already claimed");
}

TEST(SignalingProtocolTest, ParsesCandidateObjectPayload)
{
	const std::string raw = R"({
		"UUID":"peer-c",
		"session":"sess-c",
		"candidate":{"candidate":"candidate:1 1 UDP 2122260223 192.0.2.1 54400 typ host","sdpMid":"0"}
	})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Candidate);
	EXPECT_NE(parsed.candidate.find("candidate:1 1 UDP"), std::string::npos);
	EXPECT_EQ(parsed.mid, "0");
}
