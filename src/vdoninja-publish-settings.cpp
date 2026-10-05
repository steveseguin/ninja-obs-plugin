#include "vdoninja-publish-settings.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <limits>

#include "vdoninja-utils.h"

namespace vdoninja
{
namespace
{
bool startsWithInsensitive(const std::string &value, const char *prefix)
{
	if (!prefix) {
		return false;
	}

	const size_t prefixLength = std::strlen(prefix);
	if (value.size() < prefixLength) {
		return false;
	}

	for (size_t i = 0; i < prefixLength; ++i) {
		const auto lhs = static_cast<unsigned char>(value[i]);
		const auto rhs = static_cast<unsigned char>(prefix[i]);
		if (std::tolower(lhs) != std::tolower(rhs)) {
			return false;
		}
	}
	return true;
}

int hexValue(unsigned char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + (c - 'a');
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + (c - 'A');
	}
	return -1;
}

std::string urlDecode(const std::string &value)
{
	std::string decoded;
	decoded.reserve(value.size());

	for (size_t i = 0; i < value.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(value[i]);
		if (c == '%' && i + 2 < value.size()) {
			const int hi = hexValue(static_cast<unsigned char>(value[i + 1]));
			const int lo = hexValue(static_cast<unsigned char>(value[i + 2]));
			if (hi >= 0 && lo >= 0) {
				decoded.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
		}

		if (c == '+') {
			decoded.push_back(' ');
			continue;
		}

		decoded.push_back(static_cast<char>(c));
	}

	return decoded;
}

std::string queryValue(const std::string &url, const char *param)
{
	if (!param || !*param) {
		return "";
	}

	const size_t queryPos = url.find('?');
	if (queryPos == std::string::npos || queryPos + 1 >= url.size()) {
		return "";
	}

	const std::string keyPrefix = std::string(param) + "=";
	const std::vector<std::string> pairs = split(url.substr(queryPos + 1), '&');
	for (const std::string &pair : pairs) {
		if (pair.rfind(keyPrefix, 0) == 0) {
			return urlDecode(pair.substr(keyPrefix.size()));
		}
	}

	return "";
}

std::string queryFirstValue(const std::string &url, const std::initializer_list<const char *> &params)
{
	for (const char *param : params) {
		const std::string value = queryValue(url, param);
		if (!value.empty()) {
			return value;
		}
	}
	return "";
}

} // namespace

void parsePublishStreamKey(const std::string &keyValue, PublishIdentity &identity, bool allowBareStreamId)
{
	auto &[streamId, password, roomId, salt, wssHost] = identity;
	if (keyValue.empty()) {
		return;
	}

	const bool hasQuery = keyValue.find('?') != std::string::npos;
	const bool keyLooksLikeUrl =
	    startsWithInsensitive(keyValue, "https://") || startsWithInsensitive(keyValue, "http://") ||
	    (hasQuery && (keyValue.find("push=") != std::string::npos || keyValue.find("view=") != std::string::npos));
	if (!keyLooksLikeUrl) {
		const std::vector<std::string> parts = split(keyValue, '|');
		if (parts.size() > 1) {
			if (streamId.empty()) {
				streamId = trim(parts[0]);
			}
			if (password.empty() && parts.size() > 1) {
				password = trim(parts[1]);
			}
			if (roomId.empty() && parts.size() > 2) {
				roomId = trim(parts[2]);
			}
			if (salt.empty() && parts.size() > 3) {
				salt = trim(parts[3]);
			}
			if (wssHost.empty() && parts.size() > 4) {
				wssHost = trim(parts[4]);
			}
			return;
		}

		if (allowBareStreamId && streamId.empty()) {
			streamId = trim(keyValue);
		}
		return;
	}

	if (streamId.empty()) {
		const std::string push = queryValue(keyValue, "push");
		const std::string view = queryValue(keyValue, "view");
		if (!push.empty()) {
			streamId = push;
		} else if (!view.empty()) {
			streamId = view;
		}
	}

	if (password.empty()) {
		password = queryFirstValue(keyValue, {"password", "pasword", "pass", "pw", "p"});
	}

	if (roomId.empty()) {
		roomId = queryValue(keyValue, "room");
	}
	if (salt.empty()) {
		salt = queryValue(keyValue, "salt");
	}
	if (wssHost.empty()) {
		wssHost = queryValue(keyValue, "wss");
		if (wssHost.empty()) {
			wssHost = queryValue(keyValue, "wss2");
		}
		if (wssHost.empty()) {
			wssHost = queryValue(keyValue, "wss_host");
		}
		if (wssHost.empty()) {
			wssHost = queryValue(keyValue, "server");
		}
		if (wssHost.empty()) {
			wssHost = queryValue(keyValue, "signaling");
		}
	}
}

std::string buildPublishUrl(const PublishIdentity &identity, bool push)
{
	if (trim(identity.streamId).empty())
		return "";
	std::string url =
	    std::string("https://vdo.ninja/?") + (push ? "push=" : "view=") + urlEncode(trim(identity.streamId));
	const std::string password = trim(identity.password);
	if (!password.empty())
		url += isPasswordDisabledToken(password) ? "&password=false" : "&password=" + urlEncode(password);
	if (!identity.roomId.empty()) {
		url += "&room=" + urlEncode(identity.roomId);
		if (!push)
			url += "&solo";
	}
	if (!identity.salt.empty() && identity.salt != DEFAULT_SALT)
		url += "&salt=" + urlEncode(identity.salt);
	url += buildSignalingUrlParameter(identity.wssHost);
	return url;
}

bool isQualityRateControl(const std::string &rateControl)
{
	std::string mode = trim(rateControl);
	std::transform(mode.begin(), mode.end(), mode.begin(),
	               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	return mode == "ICQ" || mode == "LA_ICQ" || mode == "CQP" || mode == "CRF" || mode == "CQ" || mode == "VBR_CQ";
}

int publishPacingBitrate(const std::string &rateControl, int64_t bitrateKbps, int64_t maxBitrateKbps,
                         int fallbackBitsPerSecond)
{
	// Quality modes have no nominal bitrate. An inactive bitrate field must not
	// throttle their media. Use the pacer's transport ceiling (ten times this
	// input); this does not change the encoder settings.
	if (isQualityRateControl(rateControl))
		return 50000000;
	std::string mode = rateControl;
	std::transform(mode.begin(), mode.end(), mode.begin(),
	               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	if (mode == "VBR" || mode == "AVBR")
		bitrateKbps = std::max(bitrateKbps, maxBitrateKbps);
	if (bitrateKbps <= 0 || bitrateKbps > std::numeric_limits<int>::max() / 1000)
		return fallbackBitsPerSecond;
	return static_cast<int>(bitrateKbps * 1000);
}

int64_t publishKeyframeInterval(int64_t seconds)
{
	return seconds <= 0 || seconds > 2 ? 2 : seconds;
}

std::string publishNvencOptions(const std::string &options)
{
	// OBS splits custom options on spaces and applies them after the normal
	// B-frame setting. Preserve unrelated options and their formatting.
	std::string result = options;
	constexpr const char *prefix = "frameIntervalP=";
	constexpr size_t prefixLength = 15;
	size_t start = 0;
	while ((start = result.find_first_not_of(' ', start)) != std::string::npos) {
		const size_t end = result.find(' ', start);
		const size_t length = (end == std::string::npos ? result.size() : end) - start;
		if (length > prefixLength && result.compare(start, prefixLength, prefix) == 0) {
			const auto value = result.substr(start + prefixLength, length - prefixLength);
			if (value != "0" && value != "1") {
				result.replace(start + prefixLength, length - prefixLength, "1");
				start += prefixLength + 1;
				continue;
			}
		}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return result;
}

std::string publishEncoderTuning(const std::string &tuning)
{
	std::string normalized = tuning;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	// NVENC UHQ requires B-frames, including when the B-frame field is zero.
	return normalized == "uhq" ? "hq" : tuning;
}

} // namespace vdoninja
