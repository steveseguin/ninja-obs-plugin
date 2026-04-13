/*
 * Unit tests for vdoninja-peer-manager helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

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
