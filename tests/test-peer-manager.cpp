/*
 * Unit tests for vdoninja-peer-manager helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-peer-manager.h"
#include "vdoninja-track-utils.h"

using namespace vdoninja;

TEST(PeerManagerTrackClassificationTest, ClassifiesAudioTracksFromMediaType)
{
	EXPECT_EQ(classifyIncomingTrackKind("audio", "audio", "video", "video-alpha", false), TrackType::Audio);
}

TEST(PeerManagerTrackClassificationTest, ClassifiesExplicitVideoAlphaMidAsAlpha)
{
	EXPECT_EQ(classifyIncomingTrackKind("video", "video-alpha", "video", "", false), TrackType::AlphaVideo);
}

TEST(PeerManagerTrackClassificationTest, ClassifiesConfiguredAlphaMidAsAlpha)
{
	EXPECT_EQ(classifyIncomingTrackKind("video", "mid-2", "video", "mid-2", false), TrackType::AlphaVideo);
}

TEST(PeerManagerTrackClassificationTest, PreservesPrimaryVideoMidWhenAlphaTrackExists)
{
	EXPECT_EQ(classifyIncomingTrackKind("video", "video", "video", "video-alpha", false), TrackType::Video);
}

TEST(PeerManagerTrackClassificationTest, DoesNotTreatPlaceholderAlphaMidAsDistinctAlphaTrack)
{
	EXPECT_EQ(classifyIncomingTrackKind("video", "video", "video", "video", true), TrackType::Video);
}

TEST(PeerManagerTrackClassificationTest, FallsBackToAlphaWhenTrackHandleMatchesAlphaSlot)
{
	EXPECT_EQ(classifyIncomingTrackKind("video", "", "video", "video-alpha", true), TrackType::AlphaVideo);
}

TEST(PeerManagerTrackClassificationTest, RenegotiationKeepsFirstRepeatedVideoSectionAsPrimary)
{
	EXPECT_TRUE(isExistingPrimaryVideoSection(0, "video", "video", true));
}

TEST(PeerManagerTrackClassificationTest, GameCaptureVideoAlphaSectionIsNotTheRepeatedPrimary)
{
	EXPECT_FALSE(isExistingPrimaryVideoSection(1, "video-alpha", "video", true));
}

TEST(PeerManagerTrackClassificationTest, ExplicitVideoAlphaMidWorksWhenItIsTheOnlyOfferedVideoSection)
{
	EXPECT_FALSE(isExistingPrimaryVideoSection(0, "video-alpha", "video", true));
}

TEST(PeerManagerSnapshotTest, ExposesPerPeerMediaSendState)
{
	PeerSnapshot snapshot;

	EXPECT_TRUE(snapshot.audioSendEnabled);
	EXPECT_TRUE(snapshot.videoSendEnabled);
}
