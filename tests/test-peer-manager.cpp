/*
 * Unit tests for vdoninja-peer-manager helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-peer-manager.h"
#include "vdoninja-track-utils.h"
#include "vdoninja-video-keyframe-gate.h"

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

TEST(VideoKeyframeGateTest, NewViewerAcceptsOneCachedPrime)
{
	VideoKeyframeGate gate;

	EXPECT_TRUE(gate.isAwaitingKeyframe());
	EXPECT_TRUE(gate.isCachedPrimeAllowed());
	EXPECT_FALSE(gate.canQueueFrame(false, false));
	EXPECT_TRUE(gate.canQueueFrame(true, true));

	const auto ticket = gate.onKeyframeQueued(true);
	EXPECT_TRUE(gate.isAwaitingKeyframe());
	EXPECT_TRUE(gate.hasPendingKeyframe());
	EXPECT_FALSE(gate.hasPendingLiveKeyframe());
	EXPECT_FALSE(gate.canQueueFrame(true, true));
	EXPECT_FALSE(gate.canQueueFrame(false, false));

	EXPECT_FALSE(gate.onKeyframeSendCompleted(ticket, true, true));
	EXPECT_TRUE(gate.isAwaitingKeyframe());
	EXPECT_FALSE(gate.hasPendingKeyframe());
	EXPECT_FALSE(gate.canQueueFrame(true, true));
	EXPECT_FALSE(gate.canQueueFrame(false, false));

	const auto liveTicket = gate.onKeyframeQueued();
	EXPECT_TRUE(gate.hasPendingLiveKeyframe());
	EXPECT_TRUE(gate.canQueueFrame(false, false));
	EXPECT_TRUE(gate.onKeyframeSendCompleted(liveTicket, true));
	EXPECT_FALSE(gate.isAwaitingKeyframe());
	EXPECT_FALSE(gate.hasPendingKeyframe());
	EXPECT_FALSE(gate.canQueueFrame(true, true));
	EXPECT_TRUE(gate.canQueueFrame(false, false));
}

TEST(VideoKeyframeGateTest, CachedPrimeCannotAuthorizeDependentLiveDeltas)
{
	VideoKeyframeGate gate;

	const auto cachedTicket = gate.onKeyframeQueued(true);
	EXPECT_FALSE(gate.canQueueFrame(false, false));
	EXPECT_FALSE(gate.onKeyframeSendCompleted(cachedTicket, true, true));
	EXPECT_FALSE(gate.canQueueFrame(false, false));

	const auto liveTicket = gate.onKeyframeQueued(false);
	EXPECT_TRUE(gate.canQueueFrame(false, false));
	EXPECT_TRUE(gate.onKeyframeSendCompleted(liveTicket, true, false));
	EXPECT_TRUE(gate.canQueueFrame(false, false));
}

TEST(VideoKeyframeGateTest, InitialDecoderRequestStillAllowsCachedPrime)
{
	VideoKeyframeGate gate;

	EXPECT_EQ(gate.onDecoderKeyframeRequest(), VideoKeyframeGate::DecoderRequestDisposition::CachedPrimeAllowed);

	EXPECT_TRUE(gate.isAwaitingKeyframe());
	EXPECT_TRUE(gate.isCachedPrimeAllowed());
	EXPECT_TRUE(gate.canQueueFrame(true, true));
}

TEST(VideoKeyframeGateTest, SynchronizedDecoderRequestKeepsLiveDeltasFlowingAndRejectsStaleCache)
{
	VideoKeyframeGate gate;
	const auto initialTicket = gate.onKeyframeQueued();
	gate.onKeyframeSendCompleted(initialTicket, true);

	EXPECT_EQ(gate.onDecoderKeyframeRequest(), VideoKeyframeGate::DecoderRequestDisposition::ContinueLiveStream);

	EXPECT_FALSE(gate.isAwaitingKeyframe());
	EXPECT_FALSE(gate.isCachedPrimeAllowed());
	EXPECT_FALSE(gate.canQueueFrame(true, true));
	EXPECT_TRUE(gate.canQueueFrame(false, false));
	EXPECT_TRUE(gate.canQueueFrame(true, false));

	const auto recoveryTicket = gate.onKeyframeQueued();
	EXPECT_FALSE(gate.isAwaitingKeyframe());
	EXPECT_TRUE(gate.canQueueFrame(false, false));
	EXPECT_FALSE(gate.onKeyframeSendCompleted(recoveryTicket, true));
	EXPECT_FALSE(gate.isAwaitingKeyframe());
	EXPECT_TRUE(gate.canQueueFrame(false, false));
}

TEST(VideoKeyframeGateTest, KnownLocalLossRemainsGatedAcrossDecoderRequests)
{
	VideoKeyframeGate gate;
	const auto initialTicket = gate.onKeyframeQueued();
	gate.onKeyframeSendCompleted(initialTicket, true);
	gate.requireLiveKeyframe();

	EXPECT_EQ(gate.onDecoderKeyframeRequest(), VideoKeyframeGate::DecoderRequestDisposition::RequireNewLiveKeyframe);
	EXPECT_TRUE(gate.isAwaitingKeyframe());
	EXPECT_FALSE(gate.canQueueFrame(false, false));

	const auto recoveryTicket = gate.onKeyframeQueued();
	EXPECT_TRUE(gate.canQueueFrame(false, false));
	EXPECT_TRUE(gate.onKeyframeSendCompleted(recoveryTicket, true));
	EXPECT_FALSE(gate.isAwaitingKeyframe());
}

TEST(VideoKeyframeGateTest, DecoderRequestKeepsAnAlreadyPendingLiveKeyframeValid)
{
	VideoKeyframeGate gate;
	const auto liveTicket = gate.onKeyframeQueued();

	EXPECT_EQ(gate.onDecoderKeyframeRequest(), VideoKeyframeGate::DecoderRequestDisposition::PendingLiveKeyframe);
	EXPECT_TRUE(gate.hasPendingLiveKeyframe());
	EXPECT_TRUE(gate.canQueueFrame(false, false));
	EXPECT_TRUE(gate.onKeyframeSendCompleted(liveTicket, true));
	EXPECT_FALSE(gate.isAwaitingKeyframe());
	EXPECT_TRUE(gate.canQueueFrame(false, false));
}

TEST(VideoKeyframeGateTest, ReenabledVideoCanUseCachedPrimeAgain)
{
	VideoKeyframeGate gate;
	const auto ticket = gate.onKeyframeQueued();
	gate.onKeyframeSendCompleted(ticket, true);
	gate.onDecoderKeyframeRequest();

	gate.resetForCachedPrime();

	EXPECT_TRUE(gate.isAwaitingKeyframe());
	EXPECT_TRUE(gate.isCachedPrimeAllowed());
	EXPECT_TRUE(gate.canQueueFrame(true, true));
}

TEST(VideoKeyframeGateTest, FailedPendingKeyframeClosesGateAgain)
{
	VideoKeyframeGate gate;
	const auto ticket = gate.onKeyframeQueued();

	EXPECT_TRUE(gate.canQueueFrame(false, false));
	EXPECT_FALSE(gate.onKeyframeSendCompleted(ticket, false));

	EXPECT_TRUE(gate.isAwaitingKeyframe());
	EXPECT_FALSE(gate.hasPendingKeyframe());
	EXPECT_FALSE(gate.canQueueFrame(false, false));
	EXPECT_TRUE(gate.canQueueFrame(true, false));
}

TEST(VideoKeyframeGateTest, StaleCompletionCannotReopenNewRecoveryGeneration)
{
	VideoKeyframeGate gate;
	const auto staleTicket = gate.onKeyframeQueued();
	gate.requireLiveKeyframe();

	EXPECT_FALSE(gate.onKeyframeSendCompleted(staleTicket, true));
	EXPECT_TRUE(gate.isAwaitingKeyframe());
	EXPECT_FALSE(gate.canQueueFrame(false, false));
}
