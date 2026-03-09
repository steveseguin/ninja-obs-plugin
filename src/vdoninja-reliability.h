/*
 * OBS VDO.Ninja Plugin
 * Reliability and retry policy helpers
 */

#pragma once

#include <string>

namespace vdoninja
{

enum class SignalingAlertCategory { None, Backoff, TerminalConflict };

struct SignalingAlertPolicy {
	SignalingAlertCategory category = SignalingAlertCategory::None;
	bool suppressAutoReconnect = false;
	bool suppressViewerRetry = false;
	int signalingReconnectDelayMs = 0;
	int viewerRetryDelayMs = 0;
};

SignalingAlertPolicy classifySignalingAlert(const std::string &message);
int computeSignalingReconnectDelayMs(int attemptNumber);
int computeViewerRetryDelayMs(int retryCount);
int computeViewerPeerRecoveryDelayMs(int retryCount);
bool isSupportedNativeVideoCodecName(const std::string &codecName);
bool isSupportedNativeAudioCodecName(const std::string &codecName);

} // namespace vdoninja
