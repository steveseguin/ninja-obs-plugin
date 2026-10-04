#include <limits>

#include <gtest/gtest.h>

#include "vdoninja-publish-settings.h"
#include "vdoninja-rtp-pacer.h"

using namespace vdoninja;

TEST(PublishSettingsTest, ExplicitIdWinsAndStreamKeySuppliesMissingFields)
{
	PublishIdentity identity{"sidebar-id"};
	parsePublishStreamKey("key-id|secret|room|salt|wss://example.test", identity);
	EXPECT_EQ(identity.streamId, "sidebar-id");
	EXPECT_EQ(identity.password, "secret");
	EXPECT_EQ(identity.roomId, "room");
}

TEST(PublishSettingsTest, BareStreamKeyIsUsedBeforeGeneratingAnId)
{
	PublishIdentity identity;
	parsePublishStreamKey(" chosen-id ", identity);
	EXPECT_EQ(identity.streamId, "chosen-id");
}

TEST(PublishSettingsTest, DoesNotImportUnrelatedServiceBareKeys)
{
	PublishIdentity identity;
	parsePublishStreamKey("another-service-secret", identity, false);
	EXPECT_TRUE(identity.streamId.empty());
}

TEST(PublishSettingsTest, ViewerAndCompatibilityKeysRoundTripSpecialCharacters)
{
	const PublishIdentity original{"my-screen", "a &+% secret", "my room", "custom salt", "wss://example.test:443"};
	PublishIdentity loaded;
	parsePublishStreamKey(buildPublishUrl(original, true), loaded);
	EXPECT_EQ(loaded.streamId, original.streamId);
	EXPECT_EQ(loaded.password, original.password);
	EXPECT_EQ(loaded.roomId, original.roomId);
	EXPECT_EQ(loaded.salt, original.salt);
	EXPECT_EQ(loaded.wssHost, original.wssHost);
	const auto view = buildPublishUrl(loaded);
	EXPECT_NE(view.find("?view=my-screen"), std::string::npos);
	EXPECT_NE(view.find("&solo"), std::string::npos);
	EXPECT_NE(view.find("&wss2="), std::string::npos);
}

TEST(PublishSettingsTest, SignalingOverrideRetainsNativeBrowserProtocol)
{
	PublishIdentity identity;
	parsePublishStreamKey("https://vdo.ninja/?push=chosen&password=secret&wss=wss://wss.vdo.ninja:443", identity);
	const auto view = buildPublishUrl(identity);
	EXPECT_EQ(view.find("&wss="), std::string::npos);
	EXPECT_NE(view.find("&wss2="), std::string::npos);
	PublishIdentity loaded;
	parsePublishStreamKey(view, loaded);
	EXPECT_EQ(loaded.wssHost, "wss://wss.vdo.ninja:443");
	EXPECT_EQ(loaded.password, "secret");
}

TEST(PublishSettingsTest, ViewUrlNeverStripsAnIdThatLooksLikeAHash)
{
	EXPECT_EQ(buildPublishUrl({"my-screen123abc", "false"}), "https://vdo.ninja/?view=my-screen123abc&password=false");
	EXPECT_TRUE(buildPublishUrl({}).empty());
}

TEST(PublishSettingsTest, BitratesAboveSixAndTwelveMbpsArePreserved)
{
	EXPECT_EQ(publishPacingBitrate("CBR", 12000, 6000, 4000000), 12000000);
	EXPECT_EQ(publishPacingBitrate("CBR", 25000, 6000, 4000000), 25000000);
	EXPECT_EQ(publishPacingBitrate("VBR", 12000, 24000, 4000000), 24000000);
	EXPECT_EQ(publishPacingBitrate("CBR", std::numeric_limits<int64_t>::max(), 0, 4000000), 4000000);
}

TEST(PublishSettingsTest, QualityModesIgnoreInactiveBitrateFields)
{
	for (const auto *mode : {"ICQ", "LA_ICQ", "CQP", "CRF", "CQ", "VBR_CQ", "icq"}) {
		EXPECT_TRUE(isQualityRateControl(mode));
		const int rate = publishPacingBitrate(mode, 6000, 6000, 4000000);
		EXPECT_EQ(videoPacerBitrateForEncoderRate(rate), 100000000u);
	}
	EXPECT_FALSE(isQualityRateControl("CBR"));
}

TEST(PublishSettingsTest, CompatibilityKeepsShortGopsAndBoundsAutoOrLongGops)
{
	EXPECT_EQ(publishKeyframeInterval(0), 2);
	EXPECT_EQ(publishKeyframeInterval(1), 1);
	EXPECT_EQ(publishKeyframeInterval(2), 2);
	EXPECT_EQ(publishKeyframeInterval(10), 2);
}
