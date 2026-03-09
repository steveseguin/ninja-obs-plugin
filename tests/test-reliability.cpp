/*
 * Unit tests for reconnect and alert policy helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-reliability.h"

using namespace vdoninja;

TEST(ReliabilityTest, SignalingReconnectDelayUsesExponentialBackoff)
{
	EXPECT_EQ(computeSignalingReconnectDelayMs(1), 1000);
	EXPECT_EQ(computeSignalingReconnectDelayMs(2), 2000);
	EXPECT_EQ(computeSignalingReconnectDelayMs(3), 4000);
	EXPECT_EQ(computeSignalingReconnectDelayMs(4), 8000);
	EXPECT_EQ(computeSignalingReconnectDelayMs(5), 16000);
	EXPECT_EQ(computeSignalingReconnectDelayMs(6), 30000);
	EXPECT_EQ(computeSignalingReconnectDelayMs(9), 30000);
}

TEST(ReliabilityTest, ViewerRetryDelayUsesGentleCadence)
{
	EXPECT_EQ(computeViewerRetryDelayMs(0), 15000);
	EXPECT_EQ(computeViewerRetryDelayMs(1), 45000);
	EXPECT_EQ(computeViewerRetryDelayMs(2), 180000);
	EXPECT_EQ(computeViewerRetryDelayMs(8), 180000);
}

TEST(ReliabilityTest, ViewerPeerRecoveryRetryMatchesVdoNinjaWatchRetryCadence)
{
	EXPECT_EQ(computeViewerPeerRecoveryDelayMs(0), 5000);
	EXPECT_EQ(computeViewerPeerRecoveryDelayMs(1), 5000);
	EXPECT_EQ(computeViewerPeerRecoveryDelayMs(4), 5000);
}

TEST(ReliabilityTest, BusyAlertsBackOffInsteadOfSuppressingReconnect)
{
	const SignalingAlertPolicy policy =
	    classifySignalingAlert("Stream is busy or unavailable, please try again shortly");

	EXPECT_EQ(policy.category, SignalingAlertCategory::Backoff);
	EXPECT_FALSE(policy.suppressAutoReconnect);
	EXPECT_FALSE(policy.suppressViewerRetry);
	EXPECT_EQ(policy.signalingReconnectDelayMs, 15000);
	EXPECT_EQ(policy.viewerRetryDelayMs, 15000);
}

TEST(ReliabilityTest, CapacityAlertsBackOffInsteadOfSuppressingReconnect)
{
	const SignalingAlertPolicy policy = classifySignalingAlert("Stream is at viewer capacity");

	EXPECT_EQ(policy.category, SignalingAlertCategory::Backoff);
	EXPECT_FALSE(policy.suppressAutoReconnect);
	EXPECT_EQ(policy.viewerRetryDelayMs, 15000);
}

TEST(ReliabilityTest, RoomFullAlertsBackOffInsteadOfBeingIgnored)
{
	const SignalingAlertPolicy policy = classifySignalingAlert("Room is full");

	EXPECT_EQ(policy.category, SignalingAlertCategory::Backoff);
	EXPECT_FALSE(policy.suppressAutoReconnect);
	EXPECT_FALSE(policy.suppressViewerRetry);
	EXPECT_EQ(policy.viewerRetryDelayMs, 15000);
}

TEST(ReliabilityTest, ConflictAlertsSuppressFurtherAutomaticRetry)
{
	const SignalingAlertPolicy policy = classifySignalingAlert("Stream ID is already in use in this room.");

	EXPECT_EQ(policy.category, SignalingAlertCategory::TerminalConflict);
	EXPECT_TRUE(policy.suppressAutoReconnect);
	EXPECT_TRUE(policy.suppressViewerRetry);
	EXPECT_EQ(policy.signalingReconnectDelayMs, 0);
	EXPECT_EQ(policy.viewerRetryDelayMs, 0);
}

TEST(ReliabilityTest, ClaimedRoomAlertsSuppressFurtherAutomaticRetry)
{
	const SignalingAlertPolicy policy = classifySignalingAlert("Room already claimed");

	EXPECT_EQ(policy.category, SignalingAlertCategory::TerminalConflict);
	EXPECT_TRUE(policy.suppressAutoReconnect);
	EXPECT_TRUE(policy.suppressViewerRetry);
}

TEST(ReliabilityTest, NativeCodecSupportMatchesCurrentExperimentalDecoderScope)
{
	EXPECT_TRUE(isSupportedNativeVideoCodecName("h264"));
	EXPECT_TRUE(isSupportedNativeVideoCodecName("H264"));
	EXPECT_FALSE(isSupportedNativeVideoCodecName("VP8"));
	EXPECT_FALSE(isSupportedNativeVideoCodecName("VP9"));
	EXPECT_FALSE(isSupportedNativeVideoCodecName("AV1"));
	EXPECT_TRUE(isSupportedNativeAudioCodecName("opus"));
	EXPECT_TRUE(isSupportedNativeAudioCodecName("OPUS"));
	EXPECT_FALSE(isSupportedNativeAudioCodecName("pcmu"));
	EXPECT_FALSE(isSupportedNativeAudioCodecName("aac"));
}
