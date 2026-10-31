#include <gtest/gtest.h>

#include "vdoninja-guest-return.h"

using namespace vdoninja;

class GuestReturnTest : public testing::Test
{
protected:
	GuestReturnSettings settings{{"returnTest", "false", "", "", ""}, "obs_x264", 2, 2500};
	void SetUp() override
	{
		guest_stub::failService = guest_stub::failOutput = guest_stub::failVideo = guest_stub::failAudio =
		    guest_stub::failStart = false;
		guest_stub::forceStops = 0;
	}
	void TearDown() override
	{
		EXPECT_EQ(guest_stub::outputs, 0);
		EXPECT_EQ(guest_stub::services, 0);
		EXPECT_EQ(guest_stub::encoders, 0);
	}
};

TEST_F(GuestReturnTest, UsesSelectedMixerInSlotZeroAndDedicatedEncoders)
{
	GuestReturnOutput publisher;
	for (int track = 1; track <= 6; ++track) {
		settings.audioTrack = track;
		ASSERT_TRUE(publisher.start(settings));
		auto *output = publisher.output();
		ASSERT_NE(output, nullptr);
		EXPECT_EQ(output->audio->mixer, static_cast<size_t>(track - 1));
		EXPECT_EQ(output->audioSlot, 0u);
		EXPECT_EQ(output->audio->id, "ffmpeg_opus");
		EXPECT_EQ(output->settings.numbers["audio_encoder_index"], 0);
		EXPECT_EQ(output->video->settings.numbers["bf"], 0);
		EXPECT_EQ(output->video->settings.strings["rate_control"], "CBR");
		EXPECT_FALSE(output->settings.numbers["auto_inbound_enabled"]);
		EXPECT_FALSE(output->settings.numbers["enable_remote"]);
		EXPECT_EQ(guest_stub::encoders, 2);
		publisher.stop();
	}
}

TEST_F(GuestReturnTest, DownscalesOnlyDedicatedReturnAndDividesFrameRate)
{
	GuestReturnOutput publisher;
	settings.maxHeight = 540;
	settings.frameRateDivisor = 2;
	ASSERT_TRUE(publisher.start(settings));
	EXPECT_EQ(publisher.output()->video->width, 960u);
	EXPECT_EQ(publisher.output()->video->height, 540u);
	EXPECT_EQ(publisher.output()->video->divisor, 2u);
	publisher.stop();
	settings.maxHeight = 1080;
	settings.frameRateDivisor = 1;
	ASSERT_TRUE(publisher.start(settings));
	EXPECT_EQ(publisher.output()->video->width, 0u); // No scaler for equal or smaller OBS output.
	EXPECT_EQ(publisher.output()->video->divisor, 1u);
	publisher.stop();
	settings.maxHeight = 719;
	EXPECT_FALSE(publisher.start(settings));
	settings.maxHeight = 0;
	for (int divisor : {0, -1, 5}) {
		settings.frameRateDivisor = divisor;
		EXPECT_FALSE(publisher.start(settings));
	}
}

TEST_F(GuestReturnTest, StopsEvenBeforeDataCaptureAndRepeatedStopIsHarmless)
{
	GuestReturnOutput publisher;
	ASSERT_TRUE(publisher.start(settings));
	EXPECT_TRUE(publisher.busy());
	publisher.stop();
	publisher.stop();
	EXPECT_EQ(guest_stub::forceStops, 1);
	EXPECT_FALSE(publisher.busy());
}

TEST_F(GuestReturnTest, DoesNotReplaceRunningOutputOrIdentity)
{
	GuestReturnOutput publisher;
	ASSERT_TRUE(publisher.start(settings));
	const auto *first = publisher.output();
	settings.identity.streamId = "changed";
	EXPECT_FALSE(publisher.start(settings));
	EXPECT_EQ(publisher.output(), first);
	EXPECT_EQ(publisher.settings().identity.streamId, "returnTest");
}

TEST_F(GuestReturnTest, FreesResourcesAtEverySynchronousFailure)
{
	for (bool *failure : {&guest_stub::failService, &guest_stub::failOutput, &guest_stub::failVideo,
	                      &guest_stub::failAudio, &guest_stub::failStart}) {
		*failure = true;
		GuestReturnOutput publisher;
		EXPECT_FALSE(publisher.start(settings));
		EXPECT_FALSE(publisher.busy());
		EXPECT_FALSE(publisher.error().empty());
		EXPECT_EQ(publisher.output(), nullptr);
		EXPECT_EQ(guest_stub::outputs + guest_stub::services + guest_stub::encoders, 0);
		*failure = false;
	}
}

TEST_F(GuestReturnTest, ReportsAsyncFailureAndCanRestart)
{
	GuestReturnOutput publisher;
	ASSERT_TRUE(publisher.start(settings));
	calldata_t failure{-1};
	auto &signals = publisher.output()->signals;
	signals.callback(signals.data, &failure);
	EXPECT_FALSE(publisher.busy());
	EXPECT_FALSE(publisher.error().empty());
	ASSERT_TRUE(publisher.start(settings));
	EXPECT_TRUE(publisher.error().empty());
	EXPECT_TRUE(publisher.busy());
	EXPECT_EQ(guest_stub::outputs, 1);
}

TEST_F(GuestReturnTest, RejectsInvalidConfigurationBeforeAllocating)
{
	GuestReturnOutput publisher;
	settings.audioTrack = 0;
	EXPECT_FALSE(publisher.start(settings));
	settings.audioTrack = 7;
	EXPECT_FALSE(publisher.start(settings));
	settings.audioTrack = 2;
	settings.videoEncoder = "hevc";
	EXPECT_FALSE(publisher.start(settings));
	EXPECT_EQ(guest_stub::outputs, 0);
}

TEST_F(GuestReturnTest, NewProfileCanClearPreviousError)
{
	GuestReturnOutput publisher;
	settings.audioTrack = 0;
	EXPECT_FALSE(publisher.start(settings));
	EXPECT_FALSE(publisher.error().empty());
	publisher.stop();
	publisher.clearError();
	EXPECT_TRUE(publisher.error().empty());
	EXPECT_FALSE(publisher.busy());
}

TEST_F(GuestReturnTest, AppliesIndependentRoomCapacityAndNetworkSettings)
{
	settings.identity = {"return", "secret", "group", "custom-salt", "wss://signal.example.test"};
	settings.maxViewers = 16;
	settings.customIceServers = "turn:relay.example.test:3478|user|pass";
	settings.forceTurn = true;
	GuestReturnOutput publisher;
	ASSERT_TRUE(publisher.start(settings));
	for (const auto *data : {&publisher.output()->settings, &publisher.output()->service->settings}) {
		EXPECT_EQ(data->strings.at("room_id"), "group");
		EXPECT_EQ(data->strings.at("wss_host"), settings.identity.wssHost);
		EXPECT_EQ(data->strings.at("salt"), "custom-salt");
		EXPECT_EQ(data->strings.at("custom_ice_servers"), settings.customIceServers);
		EXPECT_EQ(data->numbers.at("force_turn"), 1);
		EXPECT_EQ(data->numbers.at("max_viewers"), 16);
	}
	settings.maxViewers = 3;
	settings.identity.roomId = "other-room";
	EXPECT_FALSE(publisher.start(settings));
	EXPECT_EQ(publisher.settings().maxViewers, 16);
	EXPECT_EQ(publisher.settings().identity.roomId, "group");
}

TEST_F(GuestReturnTest, RejectsInvalidCapacityAndRelayOnlyWithoutTurnBeforeAllocation)
{
	GuestReturnOutput publisher;
	for (int limit : {0, -1, 51}) {
		settings.maxViewers = limit;
		EXPECT_FALSE(publisher.start(settings));
		EXPECT_EQ(publisher.error(), "GuestReturn.InvalidSettings");
	}
	settings.maxViewers = 10;
	settings.forceTurn = true;
	for (const auto *servers : {"", "stun:stun.example.test:3478", "not-a-server"}) {
		settings.customIceServers = servers;
		EXPECT_FALSE(publisher.start(settings));
		EXPECT_EQ(publisher.error(), "GuestReturn.TurnRequired");
		EXPECT_EQ(guest_stub::outputs + guest_stub::services + guest_stub::encoders, 0);
	}
	settings.customIceServers = "turns:relay.example.test:5349|user|pass";
	EXPECT_TRUE(publisher.start(settings));
}
