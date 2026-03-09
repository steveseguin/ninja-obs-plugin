/*
 * OBS VDO.Ninja Plugin
 * Reliability and retry policy helpers
 */

#include "vdoninja-reliability.h"

#include <algorithm>
#include <cctype>

namespace vdoninja
{

namespace
{

constexpr int kSignalingReconnectBaseDelayMs = 1000;
constexpr int kSignalingReconnectMaxDelayMs = 30000;
constexpr int kViewerRetryInitialDelayMs = 15000;
constexpr int kViewerRetrySecondDelayMs = 45000;
constexpr int kViewerRetryLongDelayMs = 180000;
constexpr int kViewerPeerRecoveryDelayMs = 5000;

std::string asciiLowerCopy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

bool containsInsensitive(const std::string &value, const char *needle)
{
	return asciiLowerCopy(value).find(asciiLowerCopy(needle)) != std::string::npos;
}

} // namespace

SignalingAlertPolicy classifySignalingAlert(const std::string &message)
{
	SignalingAlertPolicy policy;
	if (message.empty()) {
		return policy;
	}

	if (containsInsensitive(message, "already in use") || containsInsensitive(message, "already claimed") ||
	    containsInsensitive(message, "duplicate") || containsInsensitive(message, "conflict") ||
	    containsInsensitive(message, "not approved") || containsInsensitive(message, "rejected")) {
		policy.category = SignalingAlertCategory::TerminalConflict;
		policy.suppressAutoReconnect = true;
		policy.suppressViewerRetry = true;
		return policy;
	}

	if (containsInsensitive(message, "busy") || containsInsensitive(message, "unavailable") ||
	    containsInsensitive(message, "try again shortly") || containsInsensitive(message, "viewer capacity") ||
	    containsInsensitive(message, "at viewer capacity") || containsInsensitive(message, "room is full") ||
	    containsInsensitive(message, "capacity")) {
		policy.category = SignalingAlertCategory::Backoff;
		policy.signalingReconnectDelayMs = computeViewerRetryDelayMs(0);
		policy.viewerRetryDelayMs = computeViewerRetryDelayMs(0);
		return policy;
	}

	return policy;
}

int computeSignalingReconnectDelayMs(int attemptNumber)
{
	if (attemptNumber <= 1) {
		return kSignalingReconnectBaseDelayMs;
	}

	int delay = kSignalingReconnectBaseDelayMs;
	for (int i = 1; i < attemptNumber && delay < kSignalingReconnectMaxDelayMs; ++i) {
		delay = std::min(delay * 2, kSignalingReconnectMaxDelayMs);
	}
	return delay;
}

int computeViewerRetryDelayMs(int retryCount)
{
	if (retryCount <= 0) {
		return kViewerRetryInitialDelayMs;
	}
	if (retryCount == 1) {
		return kViewerRetrySecondDelayMs;
	}
	return kViewerRetryLongDelayMs;
}

int computeViewerPeerRecoveryDelayMs(int retryCount)
{
	(void)retryCount;
	return kViewerPeerRecoveryDelayMs;
}

bool isSupportedNativeVideoCodecName(const std::string &codecName)
{
	return asciiLowerCopy(codecName) == "h264";
}

bool isSupportedNativeAudioCodecName(const std::string &codecName)
{
	return asciiLowerCopy(codecName) == "opus";
}

} // namespace vdoninja
