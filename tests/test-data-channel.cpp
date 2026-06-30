/*
 * Unit tests for VDONinjaDataChannel
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "vdoninja-data-channel.h"
#include "vdoninja-signaling-protocol.h"

using namespace vdoninja;
using ::testing::_;

namespace
{

std::string fuzzJsonString(std::mt19937 &rng, size_t maxLen)
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

std::string fuzzJsonScalar(std::mt19937 &rng)
{
	std::uniform_int_distribution<int> kindDist(0, 7);
	std::uniform_int_distribution<int> intDist(-5000, 5000);

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
		return fuzzJsonString(rng, 24);
	case 5:
		return R"({"nested":true})";
	case 6:
		return R"([true,false,"x"])";
	default:
		return "{}";
	}
}

std::string fuzzDataChannelMessage(std::mt19937 &rng)
{
	static constexpr std::array<const char *, 47> kKeys = {
	    "description",
	    "candidate",
	    "candidates",
	    "ping",
	    "pong",
	    "bye",
	    "iceRestartRequest",
	    "hangup",
	    "requestStats",
	    "requestStatsContinuous",
	    "remoteStats",
	    "chat",
	    "chatMessage",
	    "tallyOn",
	    "tallyPreview",
	    "tallyOff",
	    "requestKeyframe",
	    "keyframe",
	    "refreshVideo",
	    "refreshMicrophone",
	    "refreshConnection",
	    "refreshAll",
	    "restartWhip",
	    "getAudioSettings",
	    "muteState",
	    "muted",
	    "audioMuted",
	    "videoMuted",
	    "obsState",
	    "sceneDisplay",
	    "sceneMute",
	    "bitrate",
	    "audioBitrate",
	    "targetBitrate",
	    "targetAudioBitrate",
	    "optimizedBitrate",
	    "requestResolution",
	    "screenShareState",
	    "screenStopped",
	    "directVideoMuted",
	    "virtualHangup",
	    "remoteVideoMuted",
	    "speakerMute",
	    "displayMute",
	    "rotate_video",
	    "mirrorGuestState",
	    "mirrorGuestTarget",
	};

	std::uniform_int_distribution<size_t> keyDist(0, kKeys.size() - 1);
	std::uniform_int_distribution<int> fieldCountDist(1, 9);
	std::uniform_int_distribution<int> includeInfoDist(0, 3);

	std::ostringstream out;
	out << "{";
	bool first = true;
	const int fieldCount = fieldCountDist(rng);
	for (int i = 0; i < fieldCount; ++i) {
		if (!first) {
			out << ",";
		}
		first = false;
		out << "\"" << kKeys[keyDist(rng)] << "\":" << fuzzJsonScalar(rng);
	}
	if (includeInfoDist(rng) == 0) {
		if (!first) {
			out << ",";
		}
		out << R"("info":{)"
		    << R"("muted":)" << fuzzJsonScalar(rng) << ","
		    << R"("video_muted_init":)" << fuzzJsonScalar(rng) << ","
		    << R"("screenShareState":)" << fuzzJsonScalar(rng) << ","
		    << R"("directorVideoMuted":)" << fuzzJsonScalar(rng) << ","
		    << R"("directorSpeakerMuted":)" << fuzzJsonScalar(rng) << ","
		    << R"("directorDisplayMuted":)" << fuzzJsonScalar(rng) << ","
		    << R"("directorMirror":)" << fuzzJsonScalar(rng) << ","
		    << R"("directorFlip":)" << fuzzJsonScalar(rng) << ","
		    << R"("rotate_video":)" << fuzzJsonScalar(rng) << "}";
	}
	out << "}";
	return out.str();
}

} // namespace

// DataChannel Tests
class DataChannelTest : public ::testing::Test
{
protected:
	VDONinjaDataChannel dataChannel;
};

// Message Parsing Tests
TEST_F(DataChannelTest, ParsesChatMessage)
{
	std::string raw = "{\"chat\":\"Hello world\"}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Chat);
	EXPECT_EQ(msg.data, "Hello world");
}

TEST_F(DataChannelTest, ParsesChatMessageAlternateKey)
{
	std::string raw = "{\"chatMessage\":\"Hello\"}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Chat);
}

TEST_F(DataChannelTest, ParsesTallyOnMessage)
{
	std::string raw = "{\"tallyOn\":true}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Tally);
}

TEST_F(DataChannelTest, ParsesTallyOffMessage)
{
	std::string raw = "{\"tallyOff\":true}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Tally);
}

TEST_F(DataChannelTest, ParsesKeyframeRequest)
{
	std::string raw = "{\"requestKeyframe\":true}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RequestKeyframe);
}

TEST_F(DataChannelTest, ParsesKeyframeRequestAlternate)
{
	std::string raw = "{\"keyframe\":true}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RequestKeyframe);
}

TEST_F(DataChannelTest, ParsesMuteMessage)
{
	std::string raw = "{\"audioMuted\":true,\"videoMuted\":false}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Mute);
}

TEST_F(DataChannelTest, ParsesMutedMessage)
{
	std::string raw = "{\"muted\":true}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Mute);
}

TEST_F(DataChannelTest, ParsesOfficialMuteStateMessage)
{
	std::string raw = R"({"muteState":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Mute);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ExtractsOfficialMuteStateAsAudioOnly)
{
	MuteStateUpdate update = dataChannel.parseMuteState(R"({"muteState":true})");

	EXPECT_TRUE(update.hasAudioMuted);
	EXPECT_TRUE(update.audioMuted);
	EXPECT_FALSE(update.hasVideoMuted);
	EXPECT_FALSE(update.videoMuted);
}

TEST_F(DataChannelTest, ExtractsOfficialVideoMutedAsVideoOnly)
{
	MuteStateUpdate update = dataChannel.parseMuteState(R"({"videoMuted":true})");

	EXPECT_FALSE(update.hasAudioMuted);
	EXPECT_FALSE(update.audioMuted);
	EXPECT_TRUE(update.hasVideoMuted);
	EXPECT_TRUE(update.videoMuted);
}

TEST_F(DataChannelTest, ExtractsPluginAudioVideoMuteState)
{
	MuteStateUpdate update = dataChannel.parseMuteState(R"({"audioMuted":true,"videoMuted":false})");

	EXPECT_TRUE(update.hasAudioMuted);
	EXPECT_TRUE(update.audioMuted);
	EXPECT_TRUE(update.hasVideoMuted);
	EXPECT_FALSE(update.videoMuted);
}

TEST_F(DataChannelTest, ParsesOfficialInitialInfoMuteStateMessage)
{
	std::string raw = R"({"info":{"muted":true,"video_muted_init":true}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Mute);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ExtractsOfficialInitialInfoMuteState)
{
	MuteStateUpdate update = dataChannel.parseMuteState(R"({"info":{"muted":true,"video_muted_init":true}})");

	EXPECT_TRUE(update.hasAudioMuted);
	EXPECT_TRUE(update.audioMuted);
	EXPECT_TRUE(update.hasVideoMuted);
	EXPECT_TRUE(update.videoMuted);
}

TEST_F(DataChannelTest, ParsesOfficialObsStateMessage)
{
	std::string raw =
	    R"({"obsState":{"visibility":false,"sourceActive":true,"streaming":true,"recording":false,"virtualcam":false,"details":{"controlLevel":4}}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::ObsState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialSceneDisplayStateMessage)
{
	std::string raw = R"({"sceneDisplay":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::ObsState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialSceneMuteStateMessage)
{
	std::string raw = R"({"sceneMute":false})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::ObsState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialVideoBitrateControlMessage)
{
	std::string raw = R"({"bitrate":0})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::MediaControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialAudioBitrateControlMessage)
{
	std::string raw = R"({"audioBitrate":-1})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::MediaControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialResolutionControlMessage)
{
	std::string raw = R"({"requestResolution":{"w":1280,"h":720,"s":100,"c":true}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::MediaControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ExtractsOfficialBitrateMediaControl)
{
	MediaControlUpdate update = dataChannel.parseMediaControl(R"({"bitrate":0,"audioBitrate":16})");

	EXPECT_TRUE(update.hasVideoBitrate);
	EXPECT_EQ(update.videoBitrateKbps, 0);
	EXPECT_TRUE(update.hasAudioBitrate);
	EXPECT_EQ(update.audioBitrateKbps, 16);
}

TEST_F(DataChannelTest, ExtractsOfficialNegativeAudioBitrateAsEnable)
{
	MediaControlUpdate update = dataChannel.parseMediaControl(R"({"audioBitrate":-1})");

	EXPECT_FALSE(update.hasVideoBitrate);
	EXPECT_TRUE(update.hasAudioBitrate);
	EXPECT_EQ(update.audioBitrateKbps, -1);
}

TEST_F(DataChannelTest, ExtractsMediaControlFromCombinedTopLevelMessage)
{
	std::string raw = R"({"obsState":{"visibility":true},"bitrate":0,"audioBitrate":-1})";
	DataMessage msg = dataChannel.parseMessage(raw);
	MediaControlUpdate update = dataChannel.parseMediaControl(raw);

	EXPECT_EQ(msg.type, DataMessageType::ObsState);
	EXPECT_TRUE(update.hasVideoBitrate);
	EXPECT_EQ(update.videoBitrateKbps, 0);
	EXPECT_TRUE(update.hasAudioBitrate);
	EXPECT_EQ(update.audioBitrateKbps, -1);
}

TEST_F(DataChannelTest, MediaControlIgnoresNestedStatsBitrate)
{
	MediaControlUpdate update = dataChannel.parseMediaControl(
	    R"({"remoteStats":{"peer-1":{"bitrate":2500,"audioBitrate":64}}})");

	EXPECT_FALSE(update.hasVideoBitrate);
	EXPECT_FALSE(update.hasAudioBitrate);
}

TEST_F(DataChannelTest, ParsesOfficialScreenShareStateMessage)
{
	std::string raw = R"({"screenShareState":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::ScreenShareState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialScreenStoppedMessage)
{
	std::string raw = R"({"screenStopped":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::ScreenShareState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialInitialInfoScreenShareStateMessage)
{
	std::string raw = R"({"info":{"screenShareState":true}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::ScreenShareState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ExtractsOfficialScreenShareState)
{
	ScreenShareStateUpdate update = dataChannel.parseScreenShareState(R"({"screenShareState":true,"screenStopped":false})");

	EXPECT_TRUE(update.hasScreenShareState);
	EXPECT_TRUE(update.screenShareState);
	EXPECT_TRUE(update.hasScreenStopped);
	EXPECT_FALSE(update.screenStopped);
}

TEST_F(DataChannelTest, ExtractsOfficialInitialInfoScreenShareState)
{
	ScreenShareStateUpdate update = dataChannel.parseScreenShareState(R"({"info":{"screenShareState":true}})");

	EXPECT_TRUE(update.hasScreenShareState);
	EXPECT_TRUE(update.screenShareState);
	EXPECT_FALSE(update.hasScreenStopped);
	EXPECT_FALSE(update.screenStopped);
}

TEST_F(DataChannelTest, ExtractsOfficialMixedInitialInfoScreenShareState)
{
	ScreenShareStateUpdate update =
	    dataChannel.parseScreenShareState(R"({"info":{"muted":true,"video_muted_init":false,"screenShareState":true}})");

	EXPECT_TRUE(update.hasScreenShareState);
	EXPECT_TRUE(update.screenShareState);
	EXPECT_FALSE(update.hasScreenStopped);
	EXPECT_FALSE(update.screenStopped);
}

TEST_F(DataChannelTest, ParsesOfficialInitialDirectorVideoMutedMessage)
{
	std::string raw = R"({"info":{"directorVideoMuted":true}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::DirectorVideoState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialDirectVideoMutedMessage)
{
	std::string raw = R"({"directVideoMuted":true,"target":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::DirectorVideoState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialVirtualHangupStateMessage)
{
	std::string raw = R"({"virtualHangup":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::DirectorVideoState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialRemoteVideoMutedRequestMessage)
{
	std::string raw = R"({"remoteVideoMuted":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "remoteVideoMuted");
}

TEST_F(DataChannelTest, UnsupportedDirectorControlTakesPrecedenceOverPassiveInfo)
{
	std::string raw = R"({"info":{"directorVideoMuted":true},"remoteVideoMuted":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "remoteVideoMuted");
}

TEST_F(DataChannelTest, UnsupportedDirectorControlTakesPrecedenceOverMuteState)
{
	std::string raw = R"({"videoMuted":true,"remoteVideoMuted":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "remoteVideoMuted");
}

TEST_F(DataChannelTest, ExtractsOfficialDirectorVideoState)
{
	DirectorVideoStateUpdate update = dataChannel.parseDirectorVideoState(
	    R"({"directVideoMuted":true,"virtualHangup":false,"target":"viewer-1"})");

	EXPECT_TRUE(update.hasDirectVideoMuted);
	EXPECT_TRUE(update.directVideoMuted);
	EXPECT_TRUE(update.hasVirtualHangup);
	EXPECT_FALSE(update.virtualHangup);
	EXPECT_TRUE(update.hasTarget);
	EXPECT_FALSE(update.targetSelf);
	EXPECT_EQ(update.target, "viewer-1");
}

TEST_F(DataChannelTest, ExtractsOfficialSelfTargetedDirectorVideoState)
{
	DirectorVideoStateUpdate update = dataChannel.parseDirectorVideoState(R"({"directVideoMuted":true,"target":true})");

	EXPECT_TRUE(update.hasDirectVideoMuted);
	EXPECT_TRUE(update.directVideoMuted);
	EXPECT_TRUE(update.hasTarget);
	EXPECT_TRUE(update.targetSelf);
	EXPECT_TRUE(update.target.empty());
}

TEST_F(DataChannelTest, ExtractsOfficialInitialDirectorVideoMutedState)
{
	DirectorVideoStateUpdate update = dataChannel.parseDirectorVideoState(R"({"info":{"directorVideoMuted":true}})");

	EXPECT_TRUE(update.hasDirectVideoMuted);
	EXPECT_TRUE(update.directVideoMuted);
	EXPECT_FALSE(update.hasVirtualHangup);
	EXPECT_FALSE(update.virtualHangup);
	EXPECT_FALSE(update.hasTarget);
}

TEST_F(DataChannelTest, ExtractsOfficialMixedInitialDirectorVideoMutedState)
{
	DirectorVideoStateUpdate update =
	    dataChannel.parseDirectorVideoState(R"({"info":{"video_muted_init":false,"directorVideoMuted":true}})");

	EXPECT_TRUE(update.hasDirectVideoMuted);
	EXPECT_TRUE(update.directVideoMuted);
	EXPECT_FALSE(update.hasVirtualHangup);
	EXPECT_FALSE(update.hasTarget);
}

TEST_F(DataChannelTest, ExtractsOfficialRemoteVideoMutedRequest)
{
	DirectorVideoStateUpdate update = dataChannel.parseDirectorVideoState(R"({"remoteVideoMuted":true})");

	EXPECT_TRUE(update.hasRemoteVideoMuted);
	EXPECT_TRUE(update.remoteVideoMuted);
	EXPECT_FALSE(update.hasDirectVideoMuted);
	EXPECT_FALSE(update.hasVirtualHangup);
	EXPECT_FALSE(update.hasTarget);
}

TEST_F(DataChannelTest, ExtractsReceiverVideoSuppressionState)
{
	ReceiverVideoSuppressionUpdate media =
	    dataChannel.parseReceiverVideoSuppression(R"({"videoMuted":true,"directVideoMuted":false})");
	EXPECT_TRUE(media.hasMediaVideoMuted);
	EXPECT_TRUE(media.mediaVideoMuted);
	EXPECT_TRUE(media.hasDirectorVideoMuted);
	EXPECT_FALSE(media.directorVideoMuted);
	EXPECT_FALSE(media.hasDirectorVideoTarget);

	ReceiverVideoSuppressionUpdate initial =
	    dataChannel.parseReceiverVideoSuppression(R"({"info":{"directorVideoMuted":true}})");
	EXPECT_FALSE(initial.hasMediaVideoMuted);
	EXPECT_TRUE(initial.hasDirectorVideoMuted);
	EXPECT_TRUE(initial.directorVideoMuted);
	EXPECT_FALSE(initial.hasDirectorVideoTarget);
	EXPECT_FALSE(initial.hasVirtualHangup);

	ReceiverVideoSuppressionUpdate virtualHangup =
	    dataChannel.parseReceiverVideoSuppression(R"({"virtualHangup":false})");
	EXPECT_FALSE(virtualHangup.hasMediaVideoMuted);
	EXPECT_FALSE(virtualHangup.hasDirectorVideoMuted);
	EXPECT_TRUE(virtualHangup.hasVirtualHangup);
	EXPECT_FALSE(virtualHangup.virtualHangup);
}

TEST_F(DataChannelTest, ReceiverDirectorVideoSuppressionUsesOfficialTargetScope)
{
	ReceiverVideoSuppressionUpdate targeted =
	    dataChannel.parseReceiverVideoSuppression(R"({"directVideoMuted":true,"target":"viewer-1"})");
	EXPECT_TRUE(targeted.hasDirectorVideoMuted);
	EXPECT_TRUE(targeted.directorVideoMuted);
	EXPECT_TRUE(targeted.hasDirectorVideoTarget);
	EXPECT_FALSE(targeted.directorVideoTargetSelf);
	EXPECT_EQ(targeted.directorVideoTarget, "viewer-1");
	EXPECT_TRUE(dataChannel.receiverDirectorVideoAppliesToPeer(targeted, "viewer-1"));
	EXPECT_FALSE(dataChannel.receiverDirectorVideoAppliesToPeer(targeted, "viewer-2"));
	EXPECT_FALSE(dataChannel.receiverDirectorVideoAppliesToPeer(targeted, ""));

	ReceiverVideoSuppressionUpdate selfTargeted =
	    dataChannel.parseReceiverVideoSuppression(R"({"directVideoMuted":true,"target":true})");
	EXPECT_TRUE(selfTargeted.hasDirectorVideoTarget);
	EXPECT_TRUE(selfTargeted.directorVideoTargetSelf);
	EXPECT_TRUE(selfTargeted.directorVideoTarget.empty());
	EXPECT_TRUE(dataChannel.receiverDirectorVideoAppliesToPeer(selfTargeted, ""));
}

TEST_F(DataChannelTest, ParsesOfficialInitialDirectorAudioDisplayMutedMessage)
{
	std::string raw = R"({"info":{"directorSpeakerMuted":true,"directorDisplayMuted":false}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::DirectorAudioState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialSpeakerMuteMessage)
{
	std::string raw = R"({"speakerMute":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "speakerMute");
}

TEST_F(DataChannelTest, ParsesOfficialDisplayMuteMessage)
{
	std::string raw = R"({"displayMute":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "displayMute");
}

TEST_F(DataChannelTest, ExtractsOfficialDirectorAudioState)
{
	DirectorAudioStateUpdate update = dataChannel.parseDirectorAudioState(R"({"speakerMute":true,"displayMute":false})");

	EXPECT_TRUE(update.hasSpeakerMuted);
	EXPECT_TRUE(update.speakerMuted);
	EXPECT_TRUE(update.hasDisplayMuted);
	EXPECT_FALSE(update.displayMuted);
}

TEST_F(DataChannelTest, ExtractsOfficialInitialDirectorAudioState)
{
	DirectorAudioStateUpdate update =
	    dataChannel.parseDirectorAudioState(R"({"info":{"directorSpeakerMuted":true,"directorDisplayMuted":false}})");

	EXPECT_TRUE(update.hasSpeakerMuted);
	EXPECT_TRUE(update.speakerMuted);
	EXPECT_TRUE(update.hasDisplayMuted);
	EXPECT_FALSE(update.displayMuted);
}

TEST_F(DataChannelTest, ExtractsOfficialMixedInitialDirectorAudioState)
{
	DirectorAudioStateUpdate update = dataChannel.parseDirectorAudioState(
	    R"({"info":{"muted":false,"directorSpeakerMuted":true,"directorDisplayMuted":false,"directorVideoMuted":true}})");

	EXPECT_TRUE(update.hasSpeakerMuted);
	EXPECT_TRUE(update.speakerMuted);
	EXPECT_TRUE(update.hasDisplayMuted);
	EXPECT_FALSE(update.displayMuted);
}

TEST_F(DataChannelTest, ParsesOfficialInitialDirectorTransformStateMessage)
{
	std::string raw = R"({"info":{"directorMirror":true,"directorFlip":false}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::DirectorTransformState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialMirrorGuestStateMessage)
{
	std::string raw = R"({"mirrorGuestState":true,"mirrorGuestTarget":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "mirrorGuestState");
}

TEST_F(DataChannelTest, ParsesOfficialRotateVideoStateMessage)
{
	std::string raw = R"({"rotate_video":90})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::DirectorTransformState);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialRemoteRotateCommandMessage)
{
	std::string raw = R"({"rotate":true,"remote":"secret"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "rotate");
}

TEST_F(DataChannelTest, ExtractsOfficialDirectorTransformState)
{
	DirectorTransformStateUpdate update =
	    dataChannel.parseDirectorTransformState(R"({"mirrorGuestState":true,"mirrorGuestTarget":"viewer-1"})");

	EXPECT_TRUE(update.hasMirror);
	EXPECT_TRUE(update.mirror);
	EXPECT_TRUE(update.hasTarget);
	EXPECT_FALSE(update.targetSelf);
	EXPECT_EQ(update.target, "viewer-1");
	EXPECT_FALSE(update.hasFlip);
}

TEST_F(DataChannelTest, ExtractsOfficialSelfTargetedDirectorTransformState)
{
	DirectorTransformStateUpdate update =
	    dataChannel.parseDirectorTransformState(R"({"mirrorGuestState":false,"mirrorGuestTarget":true})");

	EXPECT_TRUE(update.hasMirror);
	EXPECT_FALSE(update.mirror);
	EXPECT_TRUE(update.hasTarget);
	EXPECT_TRUE(update.targetSelf);
	EXPECT_TRUE(update.target.empty());
	EXPECT_FALSE(update.hasFlip);
}

TEST_F(DataChannelTest, ExtractsOfficialInitialDirectorTransformState)
{
	DirectorTransformStateUpdate update =
	    dataChannel.parseDirectorTransformState(R"({"info":{"directorMirror":true,"directorFlip":false,"rotate_video":270}})");

	EXPECT_TRUE(update.hasMirror);
	EXPECT_TRUE(update.mirror);
	EXPECT_TRUE(update.hasFlip);
	EXPECT_FALSE(update.flip);
	EXPECT_TRUE(update.hasRotation);
	EXPECT_EQ(update.rotationDegrees, 270);
	EXPECT_FALSE(update.hasTarget);
}

TEST_F(DataChannelTest, ExtractsOfficialRotateVideoState)
{
	DirectorTransformStateUpdate update = dataChannel.parseDirectorTransformState(R"({"rotate_video":90})");

	EXPECT_TRUE(update.hasRotation);
	EXPECT_EQ(update.rotationDegrees, 90);
	EXPECT_FALSE(update.hasRotateCommand);
	EXPECT_FALSE(update.hasMirror);
	EXPECT_FALSE(update.hasFlip);
	EXPECT_FALSE(update.hasTarget);
}

TEST_F(DataChannelTest, ExtractsOfficialRemoteRotateToggleCommand)
{
	DirectorTransformStateUpdate update = dataChannel.parseDirectorTransformState(R"({"rotate":true,"remote":"secret"})");

	EXPECT_TRUE(update.hasRotateCommand);
	EXPECT_TRUE(update.rotateToggle);
	EXPECT_FALSE(update.rotateReset);
	EXPECT_FALSE(update.hasRotateCommandDegrees);
	EXPECT_FALSE(update.hasRotation);
}

TEST_F(DataChannelTest, ExtractsOfficialRemoteRotateResetCommand)
{
	DirectorTransformStateUpdate update = dataChannel.parseDirectorTransformState(R"({"rotate":false,"remote":"secret"})");

	EXPECT_TRUE(update.hasRotateCommand);
	EXPECT_FALSE(update.rotateToggle);
	EXPECT_TRUE(update.rotateReset);
	EXPECT_FALSE(update.hasRotateCommandDegrees);
	EXPECT_FALSE(update.hasRotation);
}

TEST_F(DataChannelTest, ExtractsOfficialRemoteRotateDegreeCommand)
{
	DirectorTransformStateUpdate update = dataChannel.parseDirectorTransformState(R"({"rotate":180,"remote":"secret"})");

	EXPECT_TRUE(update.hasRotateCommand);
	EXPECT_FALSE(update.rotateToggle);
	EXPECT_FALSE(update.rotateReset);
	EXPECT_TRUE(update.hasRotateCommandDegrees);
	EXPECT_EQ(update.rotateCommandDegrees, 180);
	EXPECT_FALSE(update.hasRotation);
}

TEST_F(DataChannelTest, ExtractsOfficialMixedDirectorTransformState)
{
	DirectorTransformStateUpdate update = dataChannel.parseDirectorTransformState(
	    R"({"mirrorGuestState":false,"mirrorGuestTarget":true,"rotate_video":180,"info":{"directorMirror":true,"directorFlip":true,"rotate_video":90}})");

	EXPECT_TRUE(update.hasMirror);
	EXPECT_FALSE(update.mirror);
	EXPECT_TRUE(update.hasFlip);
	EXPECT_TRUE(update.flip);
	EXPECT_TRUE(update.hasRotation);
	EXPECT_EQ(update.rotationDegrees, 180);
	EXPECT_FALSE(update.hasRotateCommand);
	EXPECT_TRUE(update.hasTarget);
	EXPECT_TRUE(update.targetSelf);
}

TEST_F(DataChannelTest, ParsesStatsMessage)
{
	std::string raw = "{\"stats\":{\"bitrate\":1000}}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Stats);
	EXPECT_NE(msg.data.find("bitrate"), std::string::npos);
}

TEST_F(DataChannelTest, ParsesOfficialRemoteStatsMessage)
{
	std::string raw = R"({"remoteStats":{"peer1":{"video_bitrate_kbps":2500}}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Stats);
	EXPECT_NE(msg.data.find("video_bitrate_kbps"), std::string::npos);
}

TEST_F(DataChannelTest, RemoteStatsTakesPrecedenceOverObsState)
{
	std::string raw = R"({"obsState":{"visibility":true},"remoteStats":{"peer1":{"video_bitrate_kbps":2500}}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Stats);
	EXPECT_NE(msg.data.find("video_bitrate_kbps"), std::string::npos);
}

TEST_F(DataChannelTest, ParsesOfficialRequestStatsMessage)
{
	std::string raw = R"({"requestStats":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::StatsRequest);
	EXPECT_EQ(msg.data, raw);
	EXPECT_EQ(msg.statsRequestMode, StatsRequestMode::Immediate);
}

TEST_F(DataChannelTest, StatsRequestTakesPrecedenceOverObsState)
{
	std::string raw = R"({"obsState":{"streaming":true},"requestStats":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::StatsRequest);
	EXPECT_EQ(msg.data, raw);
	EXPECT_EQ(msg.statsRequestMode, StatsRequestMode::Immediate);
}

TEST_F(DataChannelTest, StatsRequestTakesPrecedenceOverUnsupportedControl)
{
	std::string raw = R"({"requestStats":true,"requestVideoHack":true,"keyname":"frameRate","value":30})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::StatsRequest);
	EXPECT_EQ(msg.data, raw);
	EXPECT_EQ(msg.statsRequestMode, StatsRequestMode::Immediate);
}

TEST_F(DataChannelTest, StatsRequestTakesPrecedenceOverRecoveryControl)
{
	std::string raw = R"({"requestStats":true,"refreshVideo":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::StatsRequest);
	EXPECT_EQ(msg.data, raw);
	EXPECT_EQ(msg.statsRequestMode, StatsRequestMode::Immediate);
}

TEST_F(DataChannelTest, ParsesTruthyOfficialContinuousStatsRequest)
{
	std::string raw = R"({"requestStatsContinuous":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::StatsRequest);
	EXPECT_EQ(msg.data, raw);
	EXPECT_EQ(msg.statsRequestMode, StatsRequestMode::ContinuousStart);
}

TEST_F(DataChannelTest, ParsesFalseOfficialContinuousStatsRequest)
{
	std::string raw = R"({"requestStatsContinuous":false})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::StatsRequest);
	EXPECT_EQ(msg.data, raw);
	EXPECT_EQ(msg.statsRequestMode, StatsRequestMode::ContinuousStop);
}

TEST_F(DataChannelTest, ParsesOfficialRefreshVideoControlMessage)
{
	std::string raw = R"({"refreshVideo":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RecoveryControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialRefreshMicrophoneControlMessage)
{
	std::string raw = R"({"refreshMicrophone":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RecoveryControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialRefreshConnectionControlMessage)
{
	std::string raw = R"({"refreshConnection":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RecoveryControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, RecoveryControlTakesPrecedenceOverObsState)
{
	std::string raw = R"({"obsState":{"visibility":true},"refreshConnection":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RecoveryControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialRefreshAllControlMessage)
{
	std::string raw = R"({"refreshAll":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RecoveryControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialRestartWhipControlMessage)
{
	std::string raw = R"({"restartWhip":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RecoveryControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ExtractsOfficialRecoveryControl)
{
	RecoveryControlUpdate update =
	    dataChannel.parseRecoveryControl(R"({"refreshVideo":true,"refreshMicrophone":false,"refreshConnection":false,"refreshAll":true,"restartWhip":true})");

	EXPECT_TRUE(update.hasRefreshVideo);
	EXPECT_TRUE(update.refreshVideo);
	EXPECT_TRUE(update.hasRefreshMicrophone);
	EXPECT_TRUE(update.refreshMicrophone);
	EXPECT_TRUE(update.hasRefreshConnection);
	EXPECT_TRUE(update.refreshConnection);
	EXPECT_TRUE(update.hasRefreshAll);
	EXPECT_TRUE(update.refreshAll);
	EXPECT_TRUE(update.hasRestartWhip);
	EXPECT_TRUE(update.restartWhip);
}

TEST_F(DataChannelTest, RecoveryControlsUseOfficialPresenceSemantics)
{
	RecoveryControlUpdate update = dataChannel.parseRecoveryControl(
	    R"({"refreshVideo":false,"refreshMicrophone":false,"refreshConnection":false,"refreshAll":false,"restartWhip":false})");

	EXPECT_TRUE(update.hasRefreshVideo);
	EXPECT_TRUE(update.refreshVideo);
	EXPECT_TRUE(update.hasRefreshMicrophone);
	EXPECT_TRUE(update.refreshMicrophone);
	EXPECT_TRUE(update.hasRefreshConnection);
	EXPECT_TRUE(update.refreshConnection);
	EXPECT_TRUE(update.hasRefreshAll);
	EXPECT_TRUE(update.refreshAll);
	EXPECT_TRUE(update.hasRestartWhip);
	EXPECT_TRUE(update.restartWhip);
}

TEST_F(DataChannelTest, SelectsOfficialRecoveryRejectionOrder)
{
	EXPECT_EQ(dataChannel.recoveryControlRejectionName(R"({"refreshAll":true,"refreshMicrophone":true})"),
	          "refreshMicrophone");
	EXPECT_EQ(dataChannel.recoveryControlRejectionName(R"({"refreshAll":true,"refreshVideo":true})"),
	          "refreshVideo");
	EXPECT_EQ(dataChannel.recoveryControlRejectionName(R"({"refreshAll":true,"refreshConnection":true})"),
	          "refreshConnection");
	EXPECT_EQ(dataChannel.recoveryControlRejectionName(R"({"restartWhip":false})"), "restartWhip");
}

TEST_F(DataChannelTest, ParsesOfficialSettingsReadbackAsUnsupportedControl)
{
	std::string raw = R"({"getAudioSettings":true,"cbid":"settings-1"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "getAudioSettings");
}

TEST_F(DataChannelTest, ParsesOfficialVideoHackAsUnsupportedControl)
{
	std::string raw = R"({"requestVideoHack":true,"keyname":"frameRate","value":30,"remote":"secret"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "requestVideoHack");
}

TEST_F(DataChannelTest, KeyframeRequestTakesPrecedenceOverUnsupportedControl)
{
	std::string raw = R"({"requestVideoHack":true,"keyframe":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RequestKeyframe);
}

TEST_F(DataChannelTest, KeyframeRequestTakesPrecedenceOverRecoveryControl)
{
	std::string raw = R"({"refreshMicrophone":true,"keyframe":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RequestKeyframe);
}

TEST_F(DataChannelTest, ExtractsUnsupportedControlNameFromCombinedActivePayload)
{
	EXPECT_EQ(
	    dataChannel.unsupportedControlName(R"({"requestStats":true,"requestVideoHack":true,"keyname":"frameRate"})"),
	    "requestVideoHack");
	EXPECT_EQ(dataChannel.unsupportedControlName(R"({"requestVideoHack":true,"keyframe":true})"), "requestVideoHack");
}

TEST_F(DataChannelTest, UnsupportedControlTakesPrecedenceOverObsState)
{
	std::string raw = R"({"obsState":{"streaming":true},"requestVideoHack":true,"keyname":"frameRate","value":30})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "requestVideoHack");
}

TEST_F(DataChannelTest, ParsesOfficialBrowserOnlyControlAsUnsupportedControl)
{
	std::string raw = R"({"requestAudioHack":true,"keyname":"gain","value":2})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "requestAudioHack");
}

TEST_F(DataChannelTest, ParsesOfficialPtzControlAsUnsupportedControl)
{
	std::string raw = R"({"zoom":20,"abs":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "zoom");
}

TEST_F(DataChannelTest, ParsesOfficialKeyframeRateAsUnsupportedControl)
{
	std::string raw = R"({"keyframeRate":5000})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "keyframeRate");
}

TEST_F(DataChannelTest, ParsesOfficialMicIsolateAsUnsupportedControl)
{
	std::string raw = R"({"micIsolate":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "micIsolate");
}

TEST_F(DataChannelTest, ParsesOfficialLowerVolumeAsUnsupportedControl)
{
	std::string raw = R"({"lowerVolume":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "lowerVolume");
}

TEST_F(DataChannelTest, ParsesOfficialReconnectPeerMeshControlMessage)
{
	std::string raw = R"({"reconnectPeer":"viewer-2"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "reconnectPeer");
}

TEST_F(DataChannelTest, ParsesOfficialGetConnectionMapMeshControlMessage)
{
	std::string raw = R"({"getConnectionMap":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "getConnectionMap");
}

TEST_F(DataChannelTest, ParsesOfficialConnectionMapResponseMessage)
{
	std::string raw = R"({"connectionMap":{"uuid":"guest-1","connections":[]}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::MeshControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ExtractsOfficialMeshControl)
{
	MeshControlUpdate update = dataChannel.parseMeshControl(
	    R"({"reconnectPeer":"viewer-2","getConnectionMap":true,"connectionMap":{"uuid":"guest-1","connections":[]}})");

	EXPECT_TRUE(update.hasReconnectPeer);
	EXPECT_EQ(update.reconnectPeer, "viewer-2");
	EXPECT_TRUE(update.hasGetConnectionMap);
	EXPECT_TRUE(update.getConnectionMap);
	EXPECT_TRUE(update.hasConnectionMap);
	EXPECT_NE(update.connectionMapJson.find(R"("guest-1")"), std::string::npos);
}

TEST_F(DataChannelTest, ParsesOfficialDataChannelDescriptionAsSignaling)
{
	std::string raw = R"({"description":{"type":"answer","sdp":"v=0\r\na=mid:0"},"session":"sess-1"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Signaling);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, GivesOfficialDataChannelSignalingPrecedenceOverAppFields)
{
	std::string raw = R"({"chat":"not-signaling","description":{"type":"answer","sdp":"v=0\r\na=mid:0"},"session":"sess-1"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Signaling);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialDataChannelCandidateBundleAsSignaling)
{
	std::string raw = R"({"candidates":[{"candidate":"candidate:1 1 UDP 1 192.0.2.1 9 typ host","sdpMid":"0"}],"session":"sess-1"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Signaling);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, PreparesOfficialDataChannelSignalingWithSenderUuid)
{
	std::string raw = R"({"description":{"type":"answer","sdp":"v=0\r\na=mid:0"},"session":"sess-1"})";
	std::string prepared = dataChannel.prepareSignalingMessage(raw, "viewer-1");
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(prepared, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Answer);
	EXPECT_EQ(parsed.uuid, "viewer-1");
	EXPECT_EQ(parsed.session, "sess-1");
}

TEST_F(DataChannelTest, PreparesOfficialDataChannelCandidateWithSenderUuid)
{
	std::string raw =
	    R"({"candidate":{"candidate":"candidate:1 1 UDP 1 192.0.2.1 9 typ host","sdpMid":"0"},"session":"sess-1"})";
	std::string prepared = dataChannel.prepareSignalingMessage(raw, "publisher-1");
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(prepared, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Candidate);
	EXPECT_EQ(parsed.uuid, "publisher-1");
	EXPECT_EQ(parsed.candidate, "candidate:1 1 UDP 1 192.0.2.1 9 typ host");
	EXPECT_EQ(parsed.mid, "0");
	EXPECT_EQ(parsed.session, "sess-1");
}

TEST_F(DataChannelTest, PreparesOfficialDataChannelCandidateBundleWithSenderUuid)
{
	std::string raw = R"({"candidates":[{"candidate":"candidate:1 1 UDP 1 192.0.2.1 9 typ host","sdpMid":"0"}],"session":"sess-1"})";
	std::string prepared = dataChannel.prepareSignalingMessage(raw, "publisher-1");
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(prepared, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::CandidatesBundle);
	EXPECT_EQ(parsed.uuid, "publisher-1");
	ASSERT_EQ(parsed.candidates.size(), 1);
	EXPECT_EQ(parsed.candidates[0].candidate, "candidate:1 1 UDP 1 192.0.2.1 9 typ host");
	EXPECT_EQ(parsed.candidates[0].mid, "0");
	EXPECT_EQ(parsed.session, "sess-1");
}

TEST_F(DataChannelTest, KeepsExistingUuidWhenPreparingOfficialDataChannelSignaling)
{
	std::string raw =
	    R"({"UUID":"viewer-original","description":{"type":"answer","sdp":"v=0\r\na=mid:0"},"session":"sess-1"})";
	std::string prepared = dataChannel.prepareSignalingMessage(raw, "viewer-sender");
	ParsedSignalMessage parsed;
	std::string error;

	EXPECT_TRUE(parseSignalingMessage(prepared, parsed, &error));
	EXPECT_EQ(parsed.kind, ParsedSignalKind::Answer);
	EXPECT_EQ(parsed.uuid, "viewer-original");
	EXPECT_EQ(parsed.session, "sess-1");
}

TEST_F(DataChannelTest, ParsesOfficialPingMessage)
{
	std::string raw = R"({"ping":174743})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Ping);
	EXPECT_EQ(msg.data, "174743");
}

TEST_F(DataChannelTest, ParsesOfficialPongMessage)
{
	std::string raw = R"({"pong":174743})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Pong);
	EXPECT_EQ(msg.data, "174743");
}

TEST_F(DataChannelTest, CreatesOfficialPongMessageWithoutStringifyingToken)
{
	std::string msg = dataChannel.createPongMessage("174743");

	EXPECT_NE(msg.find(R"("pong":174743)"), std::string::npos);
	EXPECT_EQ(msg.find(R"("pong":"174743")"), std::string::npos);
}

TEST_F(DataChannelTest, PreservesQuotedOfficialPingTokenForPong)
{
	std::string raw = R"({"ping":"token-1"})";
	DataMessage parsed = dataChannel.parseMessage(raw);
	std::string reply = dataChannel.createPongMessage(parsed.data);

	EXPECT_EQ(parsed.type, DataMessageType::Ping);
	EXPECT_EQ(parsed.data, R"("token-1")");
	EXPECT_NE(reply.find(R"("pong":"token-1")"), std::string::npos);
}

TEST_F(DataChannelTest, ParsesOfficialIceRestartRequest)
{
	std::string raw = R"({"iceRestartRequest":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::IceRestartRequest);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialIceRestartRequestByPresence)
{
	std::string raw = R"({"iceRestartRequest":false})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::IceRestartRequest);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, IceRestartRequestPreventsIndependentKeyframeFanoutByPresence)
{
	EXPECT_FALSE(dataChannel.hasKeyframeRequest(R"({"iceRestartRequest":false,"keyframe":true})"));
}

TEST_F(DataChannelTest, ParsesOfficialHangupRequest)
{
	std::string raw = R"({"hangup":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Hangup);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialHangupRequestByPresence)
{
	std::string raw = R"({"hangup":false})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Hangup);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, HangupPreventsIndependentKeyframeFanoutByPresence)
{
	EXPECT_FALSE(dataChannel.hasKeyframeRequest(R"({"hangup":false,"keyframe":true})"));
}

TEST_F(DataChannelTest, HangupTakesPrecedenceOverMuteState)
{
	std::string raw = R"({"videoMuted":true,"hangup":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Hangup);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialByeAsPeerCleanup)
{
	std::string raw = R"({"bye":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::PeerBye);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialCloseMessageAsPeerCleanupBeforeMute)
{
	std::string raw = R"({"videoMuted":true,"bye":true})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::PeerBye);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialByePresenceAsPeerCleanup)
{
	std::string raw = R"({"bye":false})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::PeerBye);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, ParsesOfficialCleanupRequestAsPeerCleanup)
{
	std::string raw = R"({"request":"cleanup","UUID":"peer-clean","session":"sess-1"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::PeerBye);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, IgnoresUnofficialByeRequestAsPeerCleanup)
{
	std::string raw = R"({"request":"bye","UUID":"peer-bye"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Unknown);
}

TEST_F(DataChannelTest, ParsesCustomMessage)
{
	std::string raw = "{\"type\":\"custom\",\"data\":\"payload\"}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Custom);
}

TEST_F(DataChannelTest, ParsesRemoteControlObsCommandMessage)
{
	std::string raw = R"({"obsCommand":{"action":"setCurrentScene","value":"Scene A"},"remote":"secret"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RemoteControl);
}

TEST_F(DataChannelTest, RemoteControlTakesPrecedenceOverObsState)
{
	std::string raw =
	    R"({"obsState":{"visibility":true},"obsCommand":{"action":"setCurrentScene","value":"Scene A"},"remote":"secret"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::RemoteControl);
	EXPECT_EQ(msg.data, raw);
}

TEST_F(DataChannelTest, RejectsObsCommandWithoutRemoteField)
{
	std::string raw = R"({"obsCommand":{"action":"setCurrentScene","value":"Scene A"}})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "obsCommand");
}

TEST_F(DataChannelTest, RejectsLegacyActionWithoutRemoteField)
{
	std::string raw = R"({"action":"startStreaming"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::UnsupportedControl);
	EXPECT_EQ(msg.data, "obsCommand");
}

TEST_F(DataChannelTest, DoesNotTreatRemoteAuthOnlyAsAction)
{
	std::string raw = R"({"remote":"secret"})";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Unknown);
}

TEST_F(DataChannelTest, ExtractsTopLevelPlaybackHint)
{
	std::string raw = "{\"whepUrl\":\"https://example.com/live/whep\"}";
	EXPECT_EQ(dataChannel.extractInboundPlaybackHint(raw), "https://example.com/live/whep");
}

TEST_F(DataChannelTest, ExtractsPlaybackHintFromSettingsObject)
{
	std::string raw = "{\"whepSettings\":{\"type\":\"whep\",\"url\":\"https://example.com/stream/whep\"}}";
	EXPECT_EQ(dataChannel.extractInboundPlaybackHint(raw), "https://example.com/stream/whep");
}

TEST_F(DataChannelTest, ExtractsPlaybackHintFromNestedInfoObject)
{
	std::string raw = "{\"info\":{\"whepUrl\":\"https://example.com/nested/whep\"}}";
	EXPECT_EQ(dataChannel.extractInboundPlaybackHint(raw), "https://example.com/nested/whep");
}

TEST_F(DataChannelTest, IgnoresNonUrlPlaybackHints)
{
	std::string raw = "{\"whep\":\"stream-id-not-url\"}";
	EXPECT_TRUE(dataChannel.extractInboundPlaybackHint(raw).empty());
}

TEST_F(DataChannelTest, SetsTimestampOnParse)
{
	std::string raw = "{\"chat\":\"test\"}";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_GT(msg.timestamp, 0);
}

TEST_F(DataChannelTest, HandlesInvalidJson)
{
	std::string raw = "not valid json";
	DataMessage msg = dataChannel.parseMessage(raw);

	EXPECT_EQ(msg.type, DataMessageType::Unknown);
}

// Message Creation Tests
TEST_F(DataChannelTest, CreatesChatMessage)
{
	std::string msg = dataChannel.createChatMessage("Hello!");

	EXPECT_NE(msg.find("\"chat\":\"Hello!\""), std::string::npos);
	EXPECT_NE(msg.find("\"timestamp\":"), std::string::npos);
}

TEST_F(DataChannelTest, CreatesTallyOnMessage)
{
	TallyState state;
	state.program = true;
	state.preview = false;

	std::string msg = dataChannel.createTallyMessage(state);

	EXPECT_NE(msg.find("\"tallyOn\":true"), std::string::npos);
}

TEST_F(DataChannelTest, CreatesTallyPreviewMessage)
{
	TallyState state;
	state.program = false;
	state.preview = true;

	std::string msg = dataChannel.createTallyMessage(state);

	EXPECT_NE(msg.find("\"tallyPreview\":true"), std::string::npos);
}

TEST_F(DataChannelTest, CreatesTallyOffMessage)
{
	TallyState state;
	state.program = false;
	state.preview = false;

	std::string msg = dataChannel.createTallyMessage(state);

	EXPECT_NE(msg.find("\"tallyOff\":true"), std::string::npos);
}

TEST_F(DataChannelTest, CreatesMuteMessage)
{
	std::string msg = dataChannel.createMuteMessage(true, false);

	EXPECT_NE(msg.find("\"audioMuted\":true"), std::string::npos);
	EXPECT_NE(msg.find("\"videoMuted\":false"), std::string::npos);
}

TEST_F(DataChannelTest, CreatesMuteMessageBothMuted)
{
	std::string msg = dataChannel.createMuteMessage(true, true);

	EXPECT_NE(msg.find("\"audioMuted\":true"), std::string::npos);
	EXPECT_NE(msg.find("\"videoMuted\":true"), std::string::npos);
}

TEST_F(DataChannelTest, CreatesKeyframeRequest)
{
	std::string msg = dataChannel.createKeyframeRequest();

	EXPECT_NE(msg.find("\"keyframe\":true"), std::string::npos);
}

TEST_F(DataChannelTest, KeyframeRequestFanoutIgnoresTerminalTransportMessages)
{
	EXPECT_TRUE(dataChannel.hasKeyframeRequest(R"({"requestStats":true,"keyframe":true})"));
	EXPECT_FALSE(dataChannel.hasKeyframeRequest(R"({"bye":true,"keyframe":true})"));
	EXPECT_FALSE(dataChannel.hasKeyframeRequest(R"({"ping":123,"keyframe":true})"));
	EXPECT_FALSE(dataChannel.hasKeyframeRequest(R"({"description":{"type":"offer","sdp":"v=0"},"keyframe":true})"));
}

TEST_F(DataChannelTest, CreatesCustomMessage)
{
	std::string msg = dataChannel.createCustomMessage("myType", "myData");

	EXPECT_NE(msg.find("\"type\":\"myType\""), std::string::npos);
	EXPECT_NE(msg.find("\"data\":\"myData\""), std::string::npos);
	EXPECT_NE(msg.find("\"timestamp\":"), std::string::npos);
}

// Callback Tests
class DataChannelCallbackTest : public ::testing::Test
{
protected:
	VDONinjaDataChannel dataChannel;
	bool chatCalled = false;
	std::string lastChatSenderId;
	std::string lastChatMessage;

	bool tallyCalled = false;
	std::string lastTallyStreamId;
	TallyState lastTallyState;

	bool muteCalled = false;
	bool lastAudioMuted = false;
	bool lastVideoMuted = false;

	bool keyframeCalled = false;

	bool customCalled = false;
	std::string lastCustomData;

	bool remoteCalled = false;
	std::string lastRemoteAction;
	std::string lastRemoteValue;

	void SetUp() override
	{
		dataChannel.setOnChatMessage([this](const std::string &senderId, const std::string &message) {
			chatCalled = true;
			lastChatSenderId = senderId;
			lastChatMessage = message;
		});

		dataChannel.setOnTallyChange([this](const std::string &streamId, const TallyState &state) {
			tallyCalled = true;
			lastTallyStreamId = streamId;
			lastTallyState = state;
		});

		dataChannel.setOnMuteChange([this](const std::string &, bool audioMuted, bool videoMuted) {
			muteCalled = true;
			lastAudioMuted = audioMuted;
			lastVideoMuted = videoMuted;
		});

		dataChannel.setOnKeyframeRequest([this](const std::string &) { keyframeCalled = true; });

		dataChannel.setOnCustomData([this](const std::string &, const std::string &data) {
			customCalled = true;
			lastCustomData = data;
		});

		dataChannel.setOnRemoteControl([this](const std::string &action, const std::string &value) {
			remoteCalled = true;
			lastRemoteAction = action;
			lastRemoteValue = value;
		});
	}
};

TEST_F(DataChannelCallbackTest, TriggersOnChatMessage)
{
	dataChannel.handleMessage("sender123", "{\"chat\":\"Hello\"}");

	EXPECT_TRUE(chatCalled);
	EXPECT_EQ(lastChatSenderId, "sender123");
	EXPECT_EQ(lastChatMessage, "Hello");
}

TEST_F(DataChannelCallbackTest, TriggersOnTallyChange)
{
	dataChannel.handleMessage("stream1", "{\"tallyOn\":true}");

	EXPECT_TRUE(tallyCalled);
	EXPECT_EQ(lastTallyStreamId, "stream1");
	EXPECT_TRUE(lastTallyState.program);
}

TEST_F(DataChannelCallbackTest, TriggersOnTallyPreview)
{
	dataChannel.handleMessage("stream1", "{\"tallyPreview\":true}");

	EXPECT_TRUE(tallyCalled);
	EXPECT_TRUE(lastTallyState.preview);
	EXPECT_FALSE(lastTallyState.program);
}

TEST_F(DataChannelCallbackTest, TriggersOnTallyOff)
{
	// First set tally on
	dataChannel.handleMessage("stream1", "{\"tallyOn\":true}");
	// Then set it off
	dataChannel.handleMessage("stream1", "{\"tallyOff\":true}");

	EXPECT_FALSE(lastTallyState.program);
	EXPECT_FALSE(lastTallyState.preview);
}

TEST_F(DataChannelCallbackTest, TriggersOnMuteChange)
{
	dataChannel.handleMessage("peer1", "{\"audioMuted\":true,\"videoMuted\":false}");

	EXPECT_TRUE(muteCalled);
	EXPECT_TRUE(lastAudioMuted);
	EXPECT_FALSE(lastVideoMuted);
}

TEST_F(DataChannelCallbackTest, TriggersOnOfficialMuteStateChange)
{
	dataChannel.handleMessage("peer1", R"({"muteState":true})");

	EXPECT_TRUE(muteCalled);
	EXPECT_TRUE(lastAudioMuted);
	EXPECT_FALSE(lastVideoMuted);
}

TEST_F(DataChannelCallbackTest, TriggersOnKeyframeRequest)
{
	dataChannel.handleMessage("peer1", "{\"requestKeyframe\":true}");

	EXPECT_TRUE(keyframeCalled);
}

TEST_F(DataChannelCallbackTest, TriggersOnKeyframeRequestWhenCombinedWithStatsRequest)
{
	dataChannel.handleMessage("peer1", R"({"requestStats":true,"keyframe":true})");

	EXPECT_TRUE(keyframeCalled);
}

TEST_F(DataChannelCallbackTest, DoesNotTriggerKeyframeRequestWhenCombinedWithPeerBye)
{
	dataChannel.handleMessage("peer1", R"({"bye":true,"keyframe":true})");

	EXPECT_FALSE(keyframeCalled);
}

TEST_F(DataChannelCallbackTest, DoesNotTriggerKeyframeRequestWhenCombinedWithPing)
{
	dataChannel.handleMessage("peer1", R"({"ping":123,"keyframe":true})");

	EXPECT_FALSE(keyframeCalled);
}

TEST_F(DataChannelCallbackTest, DoesNotTriggerKeyframeRequestWhenCombinedWithDirectSignaling)
{
	dataChannel.handleMessage("peer1", R"({"description":{"type":"offer","sdp":"v=0"},"keyframe":true})");

	EXPECT_FALSE(keyframeCalled);
}

TEST_F(DataChannelCallbackTest, TriggersOnCustomData)
{
	dataChannel.handleMessage("peer1", "{\"type\":\"custom\",\"data\":\"payload\"}");

	EXPECT_TRUE(customCalled);
	EXPECT_EQ(lastCustomData, "payload");
}

TEST_F(DataChannelCallbackTest, TriggersRemoteControlFromObsCommand)
{
	dataChannel.handleMessage("peer1",
	                          R"({"obsCommand":{"action":"setCurrentScene","value":"Program"},"remote":"secret"})");

	EXPECT_TRUE(remoteCalled);
	EXPECT_EQ(lastRemoteAction, "setScene");
	EXPECT_EQ(lastRemoteValue, "Program");
}

TEST_F(DataChannelCallbackTest, DoesNotTriggerRemoteControlFromObsCommandWithoutRemote)
{
	dataChannel.handleMessage("peer1", R"({"obsCommand":{"action":"setCurrentScene","value":"Program"}})");

	EXPECT_FALSE(remoteCalled);
}

TEST_F(DataChannelCallbackTest, TriggersRemoteControlFromLegacyActionField)
{
	dataChannel.handleMessage("peer1", R"({"action":"startStreaming","remote":"secret"})");

	EXPECT_TRUE(remoteCalled);
	EXPECT_EQ(lastRemoteAction, "startStreaming");
	EXPECT_TRUE(lastRemoteValue.empty());
}

TEST_F(DataChannelCallbackTest, TriggersRemoteControlFromLegacyRemoteKey)
{
	dataChannel.handleMessage("peer1", R"({"remote":"nextScene","scene":"unused"})");

	EXPECT_TRUE(remoteCalled);
	EXPECT_EQ(lastRemoteAction, "nextScene");
	EXPECT_EQ(lastRemoteValue, "unused");
}

// Tally State Management Tests
class TallyStateTest : public ::testing::Test
{
protected:
	VDONinjaDataChannel dataChannel;
};

TEST_F(TallyStateTest, SetsLocalTally)
{
	TallyState state;
	state.program = true;
	state.preview = false;

	dataChannel.setLocalTally(state);
	TallyState retrieved = dataChannel.getLocalTally();

	EXPECT_TRUE(retrieved.program);
	EXPECT_FALSE(retrieved.preview);
}

TEST_F(TallyStateTest, UpdatesLocalTally)
{
	TallyState state1{true, false};
	dataChannel.setLocalTally(state1);

	TallyState state2{false, true};
	dataChannel.setLocalTally(state2);

	TallyState retrieved = dataChannel.getLocalTally();
	EXPECT_FALSE(retrieved.program);
	EXPECT_TRUE(retrieved.preview);
}

TEST_F(TallyStateTest, TracksPeerTally)
{
	dataChannel.handleMessage("peer1", "{\"tallyOn\":true}");

	TallyState peerState = dataChannel.getPeerTally("peer1");
	EXPECT_TRUE(peerState.program);
}

TEST_F(TallyStateTest, TracksMultiplePeerTallies)
{
	dataChannel.handleMessage("peer1", "{\"tallyOn\":true}");
	dataChannel.handleMessage("peer2", "{\"tallyPreview\":true}");

	TallyState peer1State = dataChannel.getPeerTally("peer1");
	TallyState peer2State = dataChannel.getPeerTally("peer2");

	EXPECT_TRUE(peer1State.program);
	EXPECT_FALSE(peer1State.preview);

	EXPECT_FALSE(peer2State.program);
	EXPECT_TRUE(peer2State.preview);
}

TEST_F(TallyStateTest, ReturnsDefaultForUnknownPeer)
{
	TallyState state = dataChannel.getPeerTally("unknown");

	EXPECT_FALSE(state.program);
	EXPECT_FALSE(state.preview);
}

TEST_F(TallyStateTest, UpdatesPeerTallyState)
{
	dataChannel.handleMessage("peer1", "{\"tallyOn\":true}");
	EXPECT_TRUE(dataChannel.getPeerTally("peer1").program);

	dataChannel.handleMessage("peer1", "{\"tallyOff\":true}");
	EXPECT_FALSE(dataChannel.getPeerTally("peer1").program);
}

// Message Round-Trip Tests
class MessageRoundTripTest : public ::testing::Test
{
protected:
	VDONinjaDataChannel dataChannel;
};

TEST_F(MessageRoundTripTest, ChatMessageRoundTrip)
{
	std::string original = "Hello, this is a test!";
	std::string jsonMsg = dataChannel.createChatMessage(original);
	DataMessage parsed = dataChannel.parseMessage(jsonMsg);

	EXPECT_EQ(parsed.type, DataMessageType::Chat);
	EXPECT_EQ(parsed.data, original);
}

TEST_F(MessageRoundTripTest, TallyProgramRoundTrip)
{
	TallyState original{true, false};
	std::string jsonMsg = dataChannel.createTallyMessage(original);
	DataMessage parsed = dataChannel.parseMessage(jsonMsg);

	EXPECT_EQ(parsed.type, DataMessageType::Tally);
}

TEST_F(MessageRoundTripTest, TallyPreviewRoundTrip)
{
	TallyState original{false, true};
	std::string jsonMsg = dataChannel.createTallyMessage(original);
	DataMessage parsed = dataChannel.parseMessage(jsonMsg);

	EXPECT_EQ(parsed.type, DataMessageType::Tally);
}

TEST_F(MessageRoundTripTest, MuteMessageRoundTrip)
{
	std::string jsonMsg = dataChannel.createMuteMessage(true, true);
	DataMessage parsed = dataChannel.parseMessage(jsonMsg);

	EXPECT_EQ(parsed.type, DataMessageType::Mute);
}

TEST_F(MessageRoundTripTest, KeyframeRequestRoundTrip)
{
	std::string jsonMsg = dataChannel.createKeyframeRequest();
	DataMessage parsed = dataChannel.parseMessage(jsonMsg);

	EXPECT_EQ(parsed.type, DataMessageType::RequestKeyframe);
}

TEST_F(MessageRoundTripTest, CustomMessageRoundTrip)
{
	std::string jsonMsg = dataChannel.createCustomMessage("myEvent", "myPayload");
	DataMessage parsed = dataChannel.parseMessage(jsonMsg);

	EXPECT_EQ(parsed.type, DataMessageType::Custom);
}

// Edge Case Tests
class DataChannelEdgeCaseTest : public ::testing::Test
{
protected:
	VDONinjaDataChannel dataChannel;
};

TEST_F(DataChannelEdgeCaseTest, HandlesEmptyMessage)
{
	DataMessage msg = dataChannel.parseMessage("");
	EXPECT_EQ(msg.type, DataMessageType::Unknown);
}

TEST_F(DataChannelEdgeCaseTest, HandlesChatWithSpecialChars)
{
	std::string msg = dataChannel.createChatMessage("Hello <script>alert('xss')</script>");
	DataMessage parsed = dataChannel.parseMessage(msg);

	EXPECT_EQ(parsed.type, DataMessageType::Chat);
	EXPECT_NE(parsed.data.find("script"), std::string::npos);
}

TEST_F(DataChannelEdgeCaseTest, HandlesChatWithNewlines)
{
	std::string msg = dataChannel.createChatMessage("Line1\nLine2\nLine3");
	DataMessage parsed = dataChannel.parseMessage(msg);

	EXPECT_EQ(parsed.type, DataMessageType::Chat);
	EXPECT_NE(parsed.data.find('\n'), std::string::npos);
}

TEST_F(DataChannelEdgeCaseTest, HandlesChatWithEmoji)
{
	std::string msg = dataChannel.createChatMessage("Hello! 👋");
	// Should not crash, emoji handling depends on encoding
	EXPECT_FALSE(msg.empty());
}

TEST_F(DataChannelEdgeCaseTest, HandlesEmptyChatMessage)
{
	std::string msg = dataChannel.createChatMessage("");
	DataMessage parsed = dataChannel.parseMessage(msg);

	EXPECT_EQ(parsed.type, DataMessageType::Chat);
	EXPECT_EQ(parsed.data, "");
}

TEST_F(DataChannelEdgeCaseTest, FuzzesOfficialMessageParserAndExtractors)
{
	std::mt19937 rng(0xC0DEF00D);
	std::vector<std::string> messages = {
	    "",
	    "{",
	    R"({"info":)",
	    R"({"description":{"type":"offer","sdp":"v=0"},"requestKeyframe":true})",
	    R"({"ping":123,"pong":"abc","videoMuted":true,"directVideoMuted":false})",
	    R"({"remoteStats":{"peer":{"bitrate":2500}},"obsState":{"visibility":true}})",
	    R"({"directVideoMuted":true,"target":"viewer-1","virtualHangup":false})",
	    R"({"requestResolution":{"w":"bad","h":1080,"s":0,"c":true},"audioBitrate":0})",
	};
	for (int i = 0; i < 512; ++i) {
		messages.push_back(fuzzDataChannelMessage(rng));
	}

	for (const std::string &message : messages) {
		SCOPED_TRACE(message);
		DataMessage parsed = dataChannel.parseMessage(message);
		(void)parsed;
		(void)dataChannel.hasKeyframeRequest(message);
		(void)dataChannel.parseMuteState(message);
		(void)dataChannel.parseMediaControl(message);
		(void)dataChannel.parseScreenShareState(message);
		DirectorVideoStateUpdate directorVideo = dataChannel.parseDirectorVideoState(message);
		ReceiverVideoSuppressionUpdate suppression = dataChannel.parseReceiverVideoSuppression(message);
		(void)dataChannel.parseDirectorAudioState(message);
		(void)dataChannel.parseDirectorTransformState(message);
		(void)dataChannel.parseRecoveryControl(message);
		(void)dataChannel.parseMeshControl(message);
		(void)dataChannel.recoveryControlRejectionName(message);
		(void)dataChannel.unsupportedControlName(message);
		(void)dataChannel.extractInboundPlaybackHint(message);

		if (suppression.hasDirectorVideoTarget) {
			EXPECT_TRUE(suppression.hasDirectorVideoMuted);
		}
		if (suppression.hasDirectorVideoTarget && !suppression.directorVideoTargetSelf) {
			EXPECT_TRUE(dataChannel.receiverDirectorVideoAppliesToPeer(suppression, suppression.directorVideoTarget));
			EXPECT_FALSE(dataChannel.receiverDirectorVideoAppliesToPeer(suppression, "definitely-not-target"));
		}
		if (!suppression.hasDirectorVideoMuted) {
			EXPECT_FALSE(dataChannel.receiverDirectorVideoAppliesToPeer(suppression, "any-peer"));
		}
	}
}
