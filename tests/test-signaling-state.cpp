/*
 * Unit tests for signaling client state transitions
 * SPDX-License-Identifier: AGPL-3.0-only
 */

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

	signaling.setOnStreamAdded([&](const std::string &streamId, const std::string &uuid) {
		addedStreams.emplace_back(streamId, uuid);
	});
	signaling.setOnStreamRemoved([&](const std::string &streamId, const std::string &uuid) {
		removedStreams.emplace_back(streamId, uuid);
	});

	signaling.processIncomingMessage(
	    R"({"request":"listing","list":[{"UUID":"peer-1","streamID":"cam_1"}]})");
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
