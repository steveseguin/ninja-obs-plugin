/*
 * Unit tests for signaling protocol normalization
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <array>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "vdoninja-signaling-protocol.h"
#include "vdoninja-utils.h"

using namespace vdoninja;

namespace
{

std::string fuzzSignalJsonString(std::mt19937 &rng, size_t maxLen)
{
	static constexpr char kChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_:- ./\\\"{}[]";
	std::uniform_int_distribution<size_t> lenDist(0, maxLen);
	std::uniform_int_distribution<size_t> charDist(0, sizeof(kChars) - 2);

	std::string value;
	const size_t len = lenDist(rng);
	value.reserve(len + 2);
	value.push_back('"');
	for (size_t i = 0; i < len; ++i) {
		const char c = kChars[charDist(rng)];
		if (c == '"' || c == '\\') {
			value.push_back('\\');
		}
		value.push_back(c);
	}
	value.push_back('"');
	return value;
}

std::string fuzzSignalScalar(std::mt19937 &rng)
{
	std::uniform_int_distribution<int> kindDist(0, 7);
	std::uniform_int_distribution<int> intDist(-10000, 10000);

	switch (kindDist(rng)) {
	case 0:
		return "true";
	case 1:
		return "false";
	case 2:
		return "null";
	case 3:
		return std::to_string(intDist(rng));
	case 4:
		return fuzzSignalJsonString(rng, 28);
	case 5:
		return R"({"type":"offer","sdp":"v=0"})";
	case 6:
		return R"([{"streamID":"cam-a"},"cam-b",false])";
	default:
		return R"({"candidate":"candidate:1 1 UDP 1 192.0.2.1 9 typ host","sdpMid":"0"})";
	}
}

std::string fuzzSignalingMessage(std::mt19937 &rng)
{
	static constexpr std::array<const char *, 21> kKeys = {
	    "request",  "Request",  "UUID",      "uuid",       "from",    "session", "Session",
	    "streamID", "streamId", "whep",      "whepUrl",    "url",     "URL",     "description",
	    "sdp",      "type",     "candidate", "candidates", "listing", "list",    "bye",
	};

	std::uniform_int_distribution<size_t> keyDist(0, kKeys.size() - 1);
	std::uniform_int_distribution<int> fieldCountDist(1, 8);

	std::ostringstream out;
	out << "{";
	const int fieldCount = fieldCountDist(rng);
	for (int i = 0; i < fieldCount; ++i) {
		if (i != 0) {
			out << ",";
		}
		out << "\"" << kKeys[keyDist(rng)] << "\":" << fuzzSignalScalar(rng);
	}
	out << "}";
	return out.str();
}

} // namespace

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
	EXPECT_EQ(parsed.streamId, "abc123");
}

TEST(SignalingProtocolTest, PlayRequestDefaultHashMapsToViewWithoutPassword)
{
	const std::string raw = R"({"request":"play","streamID":"HLBKRuy808d64"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	ASSERT_EQ(parsed.streamId, "HLBKRuy808d64");

	const std::string viewUrl = buildInboundViewUrl("https://vdo.ninja", parsed.streamId, "", "", DEFAULT_SALT);
	EXPECT_EQ(viewUrl, "https://vdo.ninja/?view=HLBKRuy");
}

TEST(SignalingProtocolTest, PlayRequestDisabledPasswordUsesPasswordFalseInViewUrl)
{
	const std::string raw = R"({"request":"play","streamID":"HLBKRuy"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	ASSERT_EQ(parsed.streamId, "HLBKRuy");

	const std::string viewUrl = buildInboundViewUrl("https://vdo.ninja", parsed.streamId, "false", "", DEFAULT_SALT);
	EXPECT_EQ(viewUrl, "https://vdo.ninja/?view=HLBKRuy&password=false");
}

TEST(SignalingProtocolTest, PlayRequestPasswordHashMapsToPasswordProtectedViewUrl)
{
	const std::string raw = R"({"request":"play","streamID":"HLBKRuyfb3179"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	ASSERT_EQ(parsed.streamId, "HLBKRuyfb3179");

	const std::string viewUrl = buildInboundViewUrl("https://vdo.ninja", parsed.streamId, "123", "", DEFAULT_SALT);
	EXPECT_EQ(viewUrl, "https://vdo.ninja/?view=HLBKRuy&password=123");
}

TEST(SignalingProtocolTest, ParsesJoinRoomRequestKeepsRoomAdmissionSemantics)
{
	const std::string raw = R"({"request":"joinroom","roomid":"myroom"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Request);
	EXPECT_EQ(parsed.request, "joinroom");
	EXPECT_TRUE(parsed.uuid.empty());
	EXPECT_TRUE(parsed.streamId.empty());
}

TEST(SignalingProtocolTest, ParsesOfferRequestCaseInsensitive)
{
	const std::string raw = R"({"request":"OfferSDP","UUID":"viewer-2","session":"sess-2"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Request);
	EXPECT_EQ(parsed.request, "OfferSDP");
	EXPECT_EQ(parsed.uuid, "viewer-2");
}

TEST(SignalingProtocolTest, ParsesOfficialIceRestartRequest)
{
	const std::string raw = R"({"iceRestartRequest":true,"UUID":"viewer-3","session":"sess-3"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Request);
	EXPECT_EQ(parsed.request, "iceRestartRequest");
	EXPECT_EQ(parsed.uuid, "viewer-3");
	EXPECT_EQ(parsed.session, "sess-3");
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

TEST(SignalingProtocolTest, ParsesDescriptionTypeCaseInsensitive)
{
	const std::string raw =
	    R"({"UUID":"peer-8","session":"sess-8","description":{"type":"Offer","sdp":"v=0\r\na=mid:0"}})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Offer);
	EXPECT_EQ(parsed.type, "Offer");
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

TEST(SignalingProtocolTest, ParsesOfficialRemoteCandidateBundle)
{
	const std::string raw = R"({
		"UUID":"publisher-1",
		"type":"remote",
		"session":"sess-r",
		"candidates":[
			{"candidate":"candidate:3 1 UDP 2122260223 198.51.100.2 54400 typ host","sdpMid":"0"}
		]
	})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::CandidatesBundle);
	EXPECT_EQ(parsed.uuid, "publisher-1");
	EXPECT_EQ(parsed.session, "sess-r");
	ASSERT_EQ(parsed.candidates.size(), 1u);
	EXPECT_NE(parsed.candidates[0].candidate.find("candidate:3 1 UDP"), std::string::npos);
	EXPECT_EQ(parsed.candidates[0].mid, "0");
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

TEST(SignalingProtocolTest, DirectPlaybackUrlsCanBeWrappedAsBrowserSourceTargets)
{
	const std::string raw = R"({"videoAddedToRoom":true,"UUID":"peer-w","whepUrl":"https://example.com/whep/streamA"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(buildInboundViewUrl("https://vdo.ninja", parsed.streamId, "", "", DEFAULT_SALT),
	          "https://vdo.ninja/?whepplay=https%3a%2f%2fexample.com%2fwhep%2fstreamA");
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

TEST(SignalingProtocolTest, ParsesTransferredRequestAsListing)
{
	const std::string raw = R"({
		"request":"transferred",
		"director":"director-1",
		"list":[
			{"streamID":"cam_1"},
			{"streamID":"cam_2"}
		]
	})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Listing);
	ASSERT_EQ(parsed.listingMembers.size(), 2u);
	EXPECT_EQ(parsed.listingMembers[0], "cam_1");
	EXPECT_EQ(parsed.listingMembers[1], "cam_2");
}

TEST(SignalingProtocolTest, ParsesSomeoneJoinedWithStreamAsVideoAdded)
{
	const std::string raw = R"({"request":"someonejoined","UUID":"peer-new","streamID":"cam_5"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::VideoAddedToRoom);
	EXPECT_EQ(parsed.uuid, "peer-new");
	EXPECT_EQ(parsed.streamId, "cam_5");
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

TEST(SignalingProtocolTest, ParsesVideoAddedToRoomMixedCaseRequestVariant)
{
	const std::string raw = R"({"request":"videoAddedToRoom","UUID":"peer-a","streamID":"cam_3"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::VideoAddedToRoom);
	EXPECT_EQ(parsed.streamId, "cam_3");
}

TEST(SignalingProtocolTest, ParsesVideoRemovedFromRoomRequestVariant)
{
	const std::string raw = R"({"request":"videoRemovedFromRoom","UUID":"peer-a","streamID":"cam_3"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::VideoRemovedFromRoom);
	EXPECT_EQ(parsed.streamId, "cam_3");
}

TEST(SignalingProtocolTest, ParsesVideoRemovedFromRoomBooleanVariant)
{
	const std::string raw = R"({"videoRemovedFromRoom":true,"UUID":"peer-a","streamID":"cam_4"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::VideoRemovedFromRoom);
	EXPECT_EQ(parsed.streamId, "cam_4");
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

TEST(SignalingProtocolTest, ParsesCleanupRequestMessage)
{
	const std::string raw = R"({"request":"cleanup","UUID":"peer-clean"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::PeerCleanup);
	EXPECT_EQ(parsed.uuid, "peer-clean");
}

TEST(SignalingProtocolTest, ParsesByeMessageAsPeerCleanup)
{
	const std::string raw = R"({"bye":true,"UUID":"peer-bye"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::PeerCleanup);
	EXPECT_EQ(parsed.uuid, "peer-bye");
}

TEST(SignalingProtocolTest, IgnoresUnofficialByeRequestAsPeerCleanup)
{
	const std::string raw = R"({"request":"bye","UUID":"peer-bye-request"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Request);
	EXPECT_EQ(parsed.request, "bye");
	EXPECT_EQ(parsed.uuid, "peer-bye-request");
}

TEST(SignalingProtocolTest, IgnoresFalseByeMessageAsPeerCleanup)
{
	const std::string raw = R"({"bye":false,"UUID":"peer-still-active"})";
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(raw, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Unknown);
	EXPECT_EQ(parsed.uuid, "peer-still-active");
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

TEST(SignalingProtocolTest, FuzzesSignalingNormalizationWithoutCrashing)
{
	std::mt19937 rng(0x5151A11);
	std::vector<std::string> messages = {
	    "",
	    "{",
	    R"({"request":)",
	    R"({"description":{"type":"offer","sdp":"v=0"},"candidate":"ignored"})",
	    R"({"request":"listing","list":[{"streamID":"cam-a"},"cam-b",null]})",
	    R"({"candidate":{"candidate":"candidate:1 1 UDP 1 192.0.2.1 9 typ host","sdpMid":"0"}})",
	    R"({"candidates":[{"candidate":"candidate:1 1 UDP 1 192.0.2.1 9 typ host","sdpMid":"0"}]})",
	    R"({"bye":true,"request":"bye","UUID":"peer-a"})",
	    R"({"request":"videoaddedtoroom","streamID":"cam-1","UUID":"peer-b"})",
	};
	for (int i = 0; i < 512; ++i) {
		messages.push_back(fuzzSignalingMessage(rng));
	}

	for (const std::string &message : messages) {
		SCOPED_TRACE(message);
		ParsedSignalMessage parsed;
		std::string error;
		const bool ok = parseSignalingMessage(message, parsed, &error);
		if (!ok) {
			EXPECT_FALSE(error.empty());
			continue;
		}

		if (parsed.kind == ParsedSignalKind::Offer || parsed.kind == ParsedSignalKind::Answer) {
			EXPECT_FALSE(parsed.sdp.empty());
		}
		if (parsed.kind == ParsedSignalKind::CandidatesBundle) {
			for (const ParsedCandidate &candidate : parsed.candidates) {
				EXPECT_FALSE(candidate.candidate.empty());
			}
		}
	}
}
