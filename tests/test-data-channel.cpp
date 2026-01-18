/*
 * Unit tests for VDONinjaDataChannel
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "vdoninja-data-channel.h"

using namespace vdoninja;
using ::testing::_;

// DataChannel Tests
class DataChannelTest : public ::testing::Test {
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

TEST_F(DataChannelTest, ParsesStatsMessage)
{
    std::string raw = "{\"stats\":{\"bitrate\":1000}}";
    DataMessage msg = dataChannel.parseMessage(raw);

    EXPECT_EQ(msg.type, DataMessageType::Stats);
}

TEST_F(DataChannelTest, ParsesCustomMessage)
{
    std::string raw = "{\"type\":\"custom\",\"data\":\"payload\"}";
    DataMessage msg = dataChannel.parseMessage(raw);

    EXPECT_EQ(msg.type, DataMessageType::Custom);
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

    EXPECT_NE(msg.find("\"requestKeyframe\":true"), std::string::npos);
}

TEST_F(DataChannelTest, CreatesCustomMessage)
{
    std::string msg = dataChannel.createCustomMessage("myType", "myData");

    EXPECT_NE(msg.find("\"type\":\"myType\""), std::string::npos);
    EXPECT_NE(msg.find("\"data\":\"myData\""), std::string::npos);
    EXPECT_NE(msg.find("\"timestamp\":"), std::string::npos);
}

// Callback Tests
class DataChannelCallbackTest : public ::testing::Test {
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

        dataChannel.setOnKeyframeRequest([this](const std::string &) {
            keyframeCalled = true;
        });

        dataChannel.setOnCustomData([this](const std::string &, const std::string &data) {
            customCalled = true;
            lastCustomData = data;
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

TEST_F(DataChannelCallbackTest, TriggersOnKeyframeRequest)
{
    dataChannel.handleMessage("peer1", "{\"requestKeyframe\":true}");

    EXPECT_TRUE(keyframeCalled);
}

TEST_F(DataChannelCallbackTest, TriggersOnCustomData)
{
    dataChannel.handleMessage("peer1", "{\"type\":\"custom\",\"data\":\"payload\"}");

    EXPECT_TRUE(customCalled);
    EXPECT_EQ(lastCustomData, "payload");
}

// Tally State Management Tests
class TallyStateTest : public ::testing::Test {
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
class MessageRoundTripTest : public ::testing::Test {
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
class DataChannelEdgeCaseTest : public ::testing::Test {
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
