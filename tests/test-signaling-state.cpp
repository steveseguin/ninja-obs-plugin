/*
 * Unit tests for signaling client state transitions
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "vdoninja-signaling.h"

using namespace vdoninja;

using ::testing::ElementsAre;
using ::testing::Pair;

TEST(SignalingStateTest, IncrementalRoomStreamEventsUpdateMemberSnapshot)
{
	VDONinjaSignaling signaling;
	std::vector<std::pair<std::string, std::string>> addedStreams;
	std::vector<std::pair<std::string, std::string>> removedStreams;

	signaling.setOnStreamAdded(
	    [&](const std::string &streamId, const std::string &uuid) { addedStreams.emplace_back(streamId, uuid); });
	signaling.setOnStreamRemoved(
	    [&](const std::string &streamId, const std::string &uuid) { removedStreams.emplace_back(streamId, uuid); });

	signaling.processIncomingMessage(R"({"request":"listing","list":[{"UUID":"peer-1","streamID":"cam_1"}]})");
	EXPECT_TRUE(signaling.isInRoom());
	EXPECT_THAT(signaling.getCurrentRoomMembers(), ElementsAre("cam_1"));

	signaling.processIncomingMessage(R"({"request":"someonejoined","UUID":"peer-2","streamID":"cam_2"})");
	EXPECT_THAT(addedStreams, ElementsAre(Pair("cam_2", "peer-2")));
	EXPECT_THAT(signaling.getCurrentRoomMembers(), ElementsAre("cam_1", "cam_2"));

	signaling.processIncomingMessage(R"({"request":"videoRemovedFromRoom","UUID":"peer-2","streamID":"cam_2"})");
	EXPECT_THAT(removedStreams, ElementsAre(Pair("cam_2", "peer-2")));
	EXPECT_THAT(signaling.getCurrentRoomMembers(), ElementsAre("cam_1"));
}

TEST(SignalingStateTest, OfficialIceRestartRequestDispatchesIceRestartRequest)
{
	VDONinjaSignaling signaling;
	std::string seenUuid;
	std::string seenSession;

	signaling.setOnIceRestartRequest([&](const std::string &uuid, const std::string &session) {
		seenUuid = uuid;
		seenSession = session;
	});

	signaling.processIncomingMessage(R"({"iceRestartRequest":true,"UUID":"viewer-3","session":"sess-3"})");

	EXPECT_EQ(seenUuid, "viewer-3");
	EXPECT_EQ(seenSession, "sess-3");
}

TEST(SignalingStateTest, NaturalTransportCloseNotifiesDisconnectedExactlyOnce)
{
	VDONinjaSignaling signaling;
	std::atomic<int> connectedCount{0};
	std::atomic<int> disconnectedCount{0};

	signaling.setAutoReconnect(false, 0);
	signaling.setOnConnected([&]() { connectedCount.fetch_add(1); });
	signaling.setOnDisconnected([&]() { disconnectedCount.fetch_add(1); });

	ASSERT_TRUE(signaling.connect("wss://unit.test"));
	EXPECT_EQ(connectedCount.load(), 1);
	ASSERT_TRUE(rtc::WebSocket::simulateRemoteClose());

	for (int attempt = 0; attempt < 50 && disconnectedCount.load() == 0; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	EXPECT_FALSE(signaling.isConnected());
	EXPECT_EQ(disconnectedCount.load(), 1);
	signaling.disconnect();
	EXPECT_EQ(disconnectedCount.load(), 1);
}

TEST(SignalingStateTest, EncryptionFailureDoesNotSendPlaintextOverDataChannel)
{
	VDONinjaSignaling signaling;
	std::string error;
	signaling.setOnError([&](const std::string &message) { error = message; });
	ASSERT_TRUE(signaling.connect("wss://unit.test"));
	ASSERT_TRUE(signaling.publishStream("stream-1", "secret"));

	auto channel = std::make_shared<rtc::DataChannel>();
	VDONinjaSignaling::setEncryptionFailureForTesting(true);
	signaling.sendAnswerViaDataChannel(channel, "peer-1", "v=0\r\n", "session-1");
	VDONinjaSignaling::setEncryptionFailureForTesting(false);

	EXPECT_TRUE(channel->lastMessage().empty());
	EXPECT_EQ(error, "Failed to encrypt answer SDP for datachannel");
	signaling.disconnect();
}

TEST(SignalingStateTest, OfferAndIceEncryptionFailuresAreReportedAndFailClosed)
{
	VDONinjaSignaling signaling;
	std::vector<std::string> errors;
	signaling.setOnError([&](const std::string &message) { errors.push_back(message); });
	ASSERT_TRUE(signaling.connect("wss://unit.test"));
	ASSERT_TRUE(signaling.publishStream("stream-1", "secret"));

	auto channel = std::make_shared<rtc::DataChannel>();
	VDONinjaSignaling::setEncryptionFailureForTesting(true);
	signaling.sendOffer("peer-1", "v=0\r\n", "session-1");
	signaling.sendIceCandidate("peer-1", "candidate:1 1 udp 1 127.0.0.1 5000 typ host", "video", "session-1");
	const bool sentViaDataChannel = signaling.sendIceCandidateViaDataChannel(
	    channel, "peer-1", "candidate:1 1 udp 1 127.0.0.1 5000 typ host", "video", "session-1");
	VDONinjaSignaling::setEncryptionFailureForTesting(false);

	EXPECT_FALSE(sentViaDataChannel);
	EXPECT_TRUE(channel->lastMessage().empty());
	EXPECT_THAT(errors, ElementsAre("Failed to encrypt offer SDP", "Failed to encrypt ICE candidate",
	                                "Failed to encrypt ICE candidate for datachannel"));
	signaling.disconnect();
}
