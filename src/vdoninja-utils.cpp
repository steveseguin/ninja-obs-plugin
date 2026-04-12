/*
 * OBS VDO.Ninja Plugin
 * Utility function implementations
 */

#include "vdoninja-utils.h"

#include <obs-module.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>

namespace vdoninja
{

namespace
{

inline uint32_t rotr32(uint32_t value, uint32_t bits)
{
	return (value >> bits) | (value << (32 - bits));
}

const std::array<uint32_t, 64> SHA256_K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::string sanitizeIdentifier(const std::string &value, size_t maxLength)
{
	std::string result;
	result.reserve(value.size());

	// Match SDK behavior: trim whitespace, keep case, replace each run of non-word chars with '_'.
	std::string trimmed = trim(value);
	bool inInvalidRun = false;
	for (char c : trimmed) {
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
			result += c;
			inInvalidRun = false;
		} else {
			if (!inInvalidRun) {
				result += '_';
				inInvalidRun = true;
			}
		}
	}

	if (result.size() > maxLength) {
		result.resize(maxLength);
	}

	return result;
}

bool startsWithInsensitive(const std::string &value, const char *prefix)
{
	size_t idx = 0;
	for (; prefix[idx] != '\0'; ++idx) {
		if (idx >= value.size()) {
			return false;
		}
		const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(value[idx])));
		const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[idx])));
		if (a != b) {
			return false;
		}
	}
	return true;
}

bool isIceUrl(const std::string &url)
{
	return startsWithInsensitive(url, "stun:") || startsWithInsensitive(url, "stuns:") ||
	       startsWithInsensitive(url, "turn:") || startsWithInsensitive(url, "turns:");
}

bool isDirectPlaybackUrl(const std::string &value)
{
	return startsWithInsensitive(value, "http://") || startsWithInsensitive(value, "https://");
}

std::string asciiLowerCopy(std::string value)
{
	for (char &c : value) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return value;
}

std::vector<std::string> splitWhitespace(const std::string &value)
{
	std::vector<std::string> tokens;
	std::istringstream input(value);
	std::string token;
	while (input >> token) {
		tokens.push_back(token);
	}
	return tokens;
}

int parseIntOrDefault(const std::string &value, int defaultValue = -1)
{
	try {
		return std::stoi(value);
	} catch (...) {
		return defaultValue;
	}
}

bool containsPayloadType(const std::vector<int> &payloadTypes, int payloadType)
{
	return std::find(payloadTypes.begin(), payloadTypes.end(), payloadType) != payloadTypes.end();
}

SdpOfferedCodec *findCodecByPayloadType(std::vector<SdpOfferedCodec> &codecs, int payloadType)
{
	for (auto &codec : codecs) {
		if (codec.payloadType == payloadType) {
			return &codec;
		}
	}
	return nullptr;
}

bool containsInsensitive(const std::string &value, const char *needle)
{
	return asciiLowerCopy(value).find(asciiLowerCopy(needle)) != std::string::npos;
}

std::string normalizeInboundPlaybackTarget(const std::string &value)
{
	if (startsWithInsensitive(value, "whep://")) {
		return "https://" + value.substr(7);
	}

	if (startsWithInsensitive(value, "wheps://")) {
		return "https://" + value.substr(8);
	}

	if (!startsWithInsensitive(value, "whep:")) {
		return value;
	}

	return trim(value.substr(5));
}

bool hasVdoBrowserPlaybackParam(const std::string &candidate)
{
	return containsInsensitive(candidate, "?view=") || containsInsensitive(candidate, "&view=") ||
	       containsInsensitive(candidate, "?whep=") || containsInsensitive(candidate, "&whep=") ||
	       containsInsensitive(candidate, "?whepplay=") || containsInsensitive(candidate, "&whepplay=");
}

bool isBrowserSourceViewerUrl(const std::string &candidate, const std::string &baseUrl)
{
	if (!isDirectPlaybackUrl(candidate)) {
		return false;
	}

	const std::string normalizedBaseUrl = trim(baseUrl);
	if (!normalizedBaseUrl.empty() && startsWithInsensitive(candidate, normalizedBaseUrl.c_str()) &&
	    hasVdoBrowserPlaybackParam(candidate)) {
		return true;
	}

	return (containsInsensitive(candidate, "://vdo.ninja") || containsInsensitive(candidate, "://obs.ninja")) &&
	       hasVdoBrowserPlaybackParam(candidate);
}

std::string normalizeBaseBrowserUrl(const std::string &baseUrl)
{
	std::string normalized = trim(baseUrl);
	if (normalized.empty()) {
		normalized = "https://vdo.ninja";
	}

	while (!normalized.empty() && normalized.back() == '/') {
		normalized.pop_back();
	}

	return normalized.empty() ? "https://vdo.ninja" : normalized;
}

} // namespace

// UUID Generation
std::string generateUUID()
{
	thread_local std::random_device rd;
	thread_local std::mt19937 gen(rd());
	thread_local std::uniform_int_distribution<> dis(0, 15);
	thread_local std::uniform_int_distribution<> dis2(8, 11);

	std::stringstream ss;
	ss << std::hex;

	for (int i = 0; i < 8; i++)
		ss << dis(gen);
	ss << "-";
	for (int i = 0; i < 4; i++)
		ss << dis(gen);
	ss << "-4"; // Version 4
	for (int i = 0; i < 3; i++)
		ss << dis(gen);
	ss << "-";
	ss << dis2(gen); // Variant
	for (int i = 0; i < 3; i++)
		ss << dis(gen);
	ss << "-";
	for (int i = 0; i < 12; i++)
		ss << dis(gen);

	return ss.str();
}

std::string generateSessionId()
{
	static const char alphanum[] = "0123456789"
	                               "abcdefghijklmnopqrstuvwxyz";
	thread_local std::random_device rd;
	thread_local std::mt19937 gen(rd());
	thread_local std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

	std::string result;
	result.reserve(8);
	for (int i = 0; i < 8; i++) {
		result += alphanum[dis(gen)];
	}
	return result;
}

// SHA-256 hashing
std::string sha256(const std::string &input)
{
	std::vector<uint8_t> data(input.begin(), input.end());
	const uint64_t originalBitLen = static_cast<uint64_t>(data.size()) * 8ULL;

	data.push_back(0x80);
	while ((data.size() % 64) != 56) {
		data.push_back(0x00);
	}

	for (int shift = 56; shift >= 0; shift -= 8) {
		data.push_back(static_cast<uint8_t>((originalBitLen >> shift) & 0xff));
	}

	uint32_t h0 = 0x6a09e667;
	uint32_t h1 = 0xbb67ae85;
	uint32_t h2 = 0x3c6ef372;
	uint32_t h3 = 0xa54ff53a;
	uint32_t h4 = 0x510e527f;
	uint32_t h5 = 0x9b05688c;
	uint32_t h6 = 0x1f83d9ab;
	uint32_t h7 = 0x5be0cd19;

	for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
		uint32_t w[64] = {};
		for (int i = 0; i < 16; ++i) {
			const size_t idx = chunk + static_cast<size_t>(i) * 4;
			w[i] = (static_cast<uint32_t>(data[idx]) << 24) | (static_cast<uint32_t>(data[idx + 1]) << 16) |
			       (static_cast<uint32_t>(data[idx + 2]) << 8) | (static_cast<uint32_t>(data[idx + 3]));
		}

		for (int i = 16; i < 64; ++i) {
			const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}

		uint32_t a = h0;
		uint32_t b = h1;
		uint32_t c = h2;
		uint32_t d = h3;
		uint32_t e = h4;
		uint32_t f = h5;
		uint32_t g = h6;
		uint32_t h = h7;

		for (int i = 0; i < 64; ++i) {
			const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
			const uint32_t ch = (e & f) ^ ((~e) & g);
			const uint32_t temp1 = h + s1 + ch + SHA256_K[i] + w[i];
			const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
			const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t temp2 = s0 + maj;

			h = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
		}

		h0 += a;
		h1 += b;
		h2 += c;
		h3 += d;
		h4 += e;
		h5 += f;
		h6 += g;
		h7 += h;
	}

	std::stringstream ss;
	ss << std::hex << std::setfill('0') << std::nouppercase;
	ss << std::setw(8) << h0 << std::setw(8) << h1 << std::setw(8) << h2 << std::setw(8) << h3 << std::setw(8) << h4
	   << std::setw(8) << h5 << std::setw(8) << h6 << std::setw(8) << h7;
	return ss.str();
}

bool isPasswordDisabledToken(const std::string &password)
{
	if (password.empty()) {
		return false;
	}

	std::string normalized = trim(password);
	if (normalized.empty()) {
		return false;
	}

	std::transform(normalized.begin(), normalized.end(), normalized.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return normalized == "false" || normalized == "0" || normalized == "off" || normalized == "no";
}

// Hash stream ID matching VDO.Ninja SDK algorithm
std::string hashStreamId(const std::string &streamId, const std::string &password, const std::string &salt)
{
	std::string sanitized = sanitizeIdentifier(streamId, 64);
	const std::string normalizedPassword = trim(password);

	// If no password, just return sanitized stream ID
	if (normalizedPassword.empty() || isPasswordDisabledToken(normalizedPassword)) {
		return sanitized;
	}

	// VDO.Ninja's JS runs encodeURIComponent() on the password before hashing
	// (sanitizePassword in lib.js). We must do the same to produce matching hashes.
	std::string encodedPassword = jsEncodeURIComponent(normalizedPassword);
	std::string passwordHash = sha256(encodedPassword + salt).substr(0, 6);
	return sanitized + passwordHash;
}

std::string hashRoomId(const std::string &roomId, const std::string &password, const std::string &salt)
{
	std::string sanitized = sanitizeIdentifier(roomId, 30);
	const std::string normalizedPassword = trim(password);

	if (normalizedPassword.empty() || isPasswordDisabledToken(normalizedPassword)) {
		return sanitized;
	}

	// VDO.Ninja's JS runs encodeURIComponent() on the password before hashing
	std::string encodedPassword = jsEncodeURIComponent(normalizedPassword);
	std::string combined = sanitized + encodedPassword + salt;
	std::string fullHash = sha256(combined);
	return fullHash.substr(0, 16);
}

std::string deriveViewStreamId(const std::string &streamId, const std::string &password, const std::string &salt)
{
	std::string viewId = streamId;
	if (viewId.size() <= 6) {
		return viewId;
	}

	const std::string normalizedPassword = trim(password);
	const bool passwordDisabled = isPasswordDisabledToken(normalizedPassword);

	std::vector<std::string> suffixes;
	if (!normalizedPassword.empty() && !passwordDisabled) {
		suffixes.push_back(sha256(jsEncodeURIComponent(normalizedPassword) + salt).substr(0, 6));
	}
	suffixes.push_back(sha256(jsEncodeURIComponent(std::string(DEFAULT_PASSWORD)) + salt).substr(0, 6));

	for (const auto &suffix : suffixes) {
		if (!suffix.empty() && viewId.size() > suffix.size() &&
		    viewId.compare(viewId.size() - suffix.size(), suffix.size(), suffix) == 0) {
			viewId.resize(viewId.size() - suffix.size());
			break;
		}
	}

	return viewId;
}

std::string buildViewerPageUrl(const std::string &baseUrl, const std::string &streamId, const std::string &password,
                               const std::string &roomId, const std::string &salt, const std::string &wssHost)
{
	const std::string normalizedStreamId = trim(streamId);
	if (normalizedStreamId.empty()) {
		return "";
	}

	const std::string normalizedBaseUrl = normalizeBaseBrowserUrl(baseUrl);
	const std::string normalizedPassword = trim(password);
	const bool passwordDisabled = isPasswordDisabledToken(normalizedPassword);
	const std::string viewId = deriveViewStreamId(normalizedStreamId, normalizedPassword, salt);

	std::string url = normalizedBaseUrl + "/?view=" + urlEncode(viewId);
	if (!roomId.empty()) {
		url += "&room=" + urlEncode(roomId);
		url += "&solo";
	}

	if (!normalizedPassword.empty()) {
		url += passwordDisabled ? "&password=false" : "&password=" + urlEncode(normalizedPassword);
	}

	if (!salt.empty() && salt != DEFAULT_SALT) {
		url += "&salt=" + urlEncode(salt);
	}

	if (!wssHost.empty() && wssHost != DEFAULT_WSS_HOST) {
		url += "&wss=" + urlEncode(wssHost);
	}

	return url;
}

std::string buildInboundViewUrl(const std::string &baseUrl, const std::string &streamId, const std::string &password,
                                const std::string &roomId, const std::string &salt)
{
	const std::string normalizedStreamId = normalizeInboundPlaybackTarget(trim(streamId));
	const std::string normalizedBaseUrl = normalizeBaseBrowserUrl(baseUrl);

	if (normalizedStreamId.empty()) {
		return "";
	}

	// Browser-source auto-add should only point at VDO.Ninja-style viewer pages.
	if (isBrowserSourceViewerUrl(normalizedStreamId, normalizedBaseUrl)) {
		return normalizedStreamId;
	}

	if (isDirectPlaybackUrl(normalizedStreamId)) {
		return normalizedBaseUrl + "/?whepplay=" + urlEncode(normalizedStreamId);
	}

	return buildViewerPageUrl(normalizedBaseUrl, normalizedStreamId, password, roomId, salt);
}

int chooseViewerTargetBitrateKbps(uint32_t width, uint32_t height)
{
	if (width >= 1920 || height >= 1080) {
		return 4000;
	}
	if (width >= 1280 || height >= 720) {
		return 2500;
	}
	if (width >= 854 || height >= 480) {
		return 1200;
	}
	return 800;
}

std::string buildViewerRequestMessage(uint32_t width, uint32_t height, bool guest, const std::string &viewerInfoJson)
{
	JsonBuilder request;
	request.add("downloads", false)
	    .add("allowmidi", false)
	    .add("allowdrawing", false)
	    .add("iframe", false)
	    .add("widget", false)
	    .add("audio", true)
	    .add("video", true)
	    .add("broadcast", false)
	    .add("allowwebp", false)
	    .add("allowscreenaudio", false)
	    .add("allowscreenvideo", false)
	    .add("allowchunked", false)
	    .add("allowresources", false)
	    .add("degrade", "maintain-resolution")
	    .add("bitrate", chooseViewerTargetBitrateKbps(width, height))
	    .add("targetBitrate", chooseViewerTargetBitrateKbps(width, height));

	if (guest) {
		request.add("guest", true);
	}

	if (width > 0 || height > 0) {
		JsonBuilder resolution;
		if (width > 0) {
			resolution.add("w", static_cast<int>(width));
		}
		if (height > 0) {
			resolution.add("h", static_cast<int>(height));
		}
		request.addRaw("requestResolution", resolution.build());
	}

	if (!viewerInfoJson.empty()) {
		request.addRaw("info", viewerInfoJson);
	}

	return request.build();
}

std::vector<std::string> buildIncomingSignalingPasswordCandidates(const std::string &messageStreamId,
                                                                  const std::string &defaultPassword,
                                                                  const StreamInfo &publishedStream,
                                                                  const std::vector<StreamInfo> &viewingStreams,
                                                                  const RoomInfo &currentRoom)
{
	std::vector<std::string> candidates;
	auto addCandidate = [&candidates](const std::string &password) {
		if (password.empty() ||
		    std::find(candidates.begin(), candidates.end(), password) != candidates.end()) {
			return;
		}
		candidates.push_back(password);
	};

	auto matchesStream = [&messageStreamId](const StreamInfo &stream) {
		return !messageStreamId.empty() &&
		       (messageStreamId == stream.streamId || messageStreamId == stream.hashedStreamId);
	};

	if (matchesStream(publishedStream)) {
		addCandidate(publishedStream.password);
	}

	for (const auto &stream : viewingStreams) {
		if (matchesStream(stream)) {
			addCandidate(stream.password);
		}
	}

	addCandidate(publishedStream.password);
	for (const auto &stream : viewingStreams) {
		addCandidate(stream.password);
	}
	addCandidate(currentRoom.password);
	addCandidate(defaultPassword);

	return candidates;
}

std::string sanitizeStreamId(const std::string &streamId)
{
	return sanitizeIdentifier(streamId, 64);
}

// JSON Builder implementation
JsonBuilder &JsonBuilder::add(const std::string &key, const std::string &value)
{
	std::string escaped;
	escaped.reserve(value.size() + 2);
	escaped += '"';
	for (char c : value) {
		switch (c) {
		case '"':
			escaped += "\\\"";
			break;
		case '\\':
			escaped += "\\\\";
			break;
		case '\b':
			escaped += "\\b";
			break;
		case '\f':
			escaped += "\\f";
			break;
		case '\n':
			escaped += "\\n";
			break;
		case '\r':
			escaped += "\\r";
			break;
		case '\t':
			escaped += "\\t";
			break;
		default:
			escaped += c;
			break;
		}
	}
	escaped += '"';
	entries_.emplace_back(key, escaped);
	return *this;
}

JsonBuilder &JsonBuilder::add(const std::string &key, const char *value)
{
	return add(key, std::string(value));
}

JsonBuilder &JsonBuilder::add(const std::string &key, int value)
{
	entries_.emplace_back(key, std::to_string(value));
	return *this;
}

JsonBuilder &JsonBuilder::add(const std::string &key, int64_t value)
{
	entries_.emplace_back(key, std::to_string(value));
	return *this;
}

JsonBuilder &JsonBuilder::add(const std::string &key, bool value)
{
	entries_.emplace_back(key, value ? "true" : "false");
	return *this;
}

JsonBuilder &JsonBuilder::addRaw(const std::string &key, const std::string &rawJson)
{
	entries_.emplace_back(key, rawJson);
	return *this;
}

std::string JsonBuilder::build() const
{
	std::stringstream ss;
	ss << "{";
	for (size_t i = 0; i < entries_.size(); i++) {
		if (i > 0)
			ss << ",";
		ss << "\"" << entries_[i].first << "\":" << entries_[i].second;
	}
	ss << "}";
	return ss.str();
}

// JSON Parser implementation
JsonParser::JsonParser(const std::string &json) : json_(json)
{
	parse();
}

void JsonParser::parse()
{
	// Simple JSON parser - handles basic key-value pairs
	size_t pos = 0;
	const auto isWhitespace = [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; };

	// Skip whitespace and opening brace
	while (pos < json_.size() && (isWhitespace(json_[pos]) || json_[pos] == '{'))
		pos++;

	while (pos < json_.size() && json_[pos] != '}') {
		// Skip whitespace
		while (pos < json_.size() && isWhitespace(json_[pos]))
			pos++;
		if (pos >= json_.size() || json_[pos] == '}') {
			break;
		}

		if (json_[pos] != '"')
			break;
		pos++; // Skip opening quote

		// Extract key
		std::string key;
		while (pos < json_.size() && json_[pos] != '"') {
			key += json_[pos++];
		}
		pos++; // Skip closing quote

		// Skip to colon
		while (pos < json_.size() && json_[pos] != ':')
			pos++;
		if (pos >= json_.size()) {
			break;
		}
		pos++; // Skip colon

		// Skip whitespace
		while (pos < json_.size() && isWhitespace(json_[pos]))
			pos++;
		if (pos >= json_.size()) {
			break;
		}

		// Extract value
		std::string value = extractValue(pos);
		values_[key] = value;

		// Skip comma and whitespace
		while (pos < json_.size() && (isWhitespace(json_[pos]) || json_[pos] == ','))
			pos++;
	}
}

std::string JsonParser::extractValue(size_t &pos) const
{
	std::string value;
	if (pos >= json_.size()) {
		return value;
	}

	if (json_[pos] == '"') {
		// String value
		pos++; // Skip opening quote
		while (pos < json_.size() && json_[pos] != '"') {
			if (json_[pos] == '\\' && pos + 1 < json_.size()) {
				pos++;
				switch (json_[pos]) {
				case 'n':
					value += '\n';
					break;
				case 'r':
					value += '\r';
					break;
				case 't':
					value += '\t';
					break;
				case '"':
					value += '"';
					break;
				case '\\':
					value += '\\';
					break;
				default:
					value += json_[pos];
					break;
				}
			} else {
				value += json_[pos];
			}
			pos++;
		}
		pos++; // Skip closing quote
	} else if (json_[pos] == '{') {
		// Object - capture the whole thing, skipping over string literals
		int depth = 1;
		value += json_[pos++];
		while (pos < json_.size() && depth > 0) {
			if (json_[pos] == '"') {
				value += json_[pos++];
				while (pos < json_.size() && json_[pos] != '"') {
					if (json_[pos] == '\\' && pos + 1 < json_.size()) {
						value += json_[pos++];
					}
					value += json_[pos++];
				}
				if (pos < json_.size())
					value += json_[pos++];
			} else {
				if (json_[pos] == '{')
					depth++;
				else if (json_[pos] == '}')
					depth--;
				value += json_[pos++];
			}
		}
	} else if (json_[pos] == '[') {
		// Array - capture the whole thing, skipping over string literals
		int depth = 1;
		value += json_[pos++];
		while (pos < json_.size() && depth > 0) {
			if (json_[pos] == '"') {
				value += json_[pos++];
				while (pos < json_.size() && json_[pos] != '"') {
					if (json_[pos] == '\\' && pos + 1 < json_.size()) {
						value += json_[pos++];
					}
					value += json_[pos++];
				}
				if (pos < json_.size())
					value += json_[pos++];
			} else {
				if (json_[pos] == '[')
					depth++;
				else if (json_[pos] == ']')
					depth--;
				value += json_[pos++];
			}
		}
	} else {
		// Number, boolean, or null
		while (pos < json_.size() && json_[pos] != ',' && json_[pos] != '}' &&
		       !std::isspace(static_cast<unsigned char>(json_[pos]))) {
			value += json_[pos++];
		}
	}

	return value;
}

bool JsonParser::hasKey(const std::string &key) const
{
	return values_.find(key) != values_.end();
}

std::string JsonParser::getString(const std::string &key, const std::string &defaultValue) const
{
	auto it = values_.find(key);
	if (it != values_.end()) {
		return it->second;
	}
	return defaultValue;
}

int JsonParser::getInt(const std::string &key, int defaultValue) const
{
	auto it = values_.find(key);
	if (it != values_.end()) {
		try {
			return std::stoi(it->second);
		} catch (...) {
			return defaultValue;
		}
	}
	return defaultValue;
}

bool JsonParser::getBool(const std::string &key, bool defaultValue) const
{
	auto it = values_.find(key);
	if (it != values_.end()) {
		return it->second == "true";
	}
	return defaultValue;
}

std::string JsonParser::getRaw(const std::string &key) const
{
	auto it = values_.find(key);
	if (it != values_.end()) {
		return it->second;
	}
	return "";
}

std::string JsonParser::getObject(const std::string &key) const
{
	return getRaw(key);
}

std::vector<std::string> JsonParser::getArray(const std::string &key) const
{
	std::vector<std::string> result;
	std::string arr = getRaw(key);
	const auto isWhitespace = [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; };

	if (arr.empty() || arr[0] != '[')
		return result;

	size_t pos = 1;
	while (pos < arr.size() && arr[pos] != ']') {
		while (pos < arr.size() && isWhitespace(arr[pos]))
			pos++;
		if (pos >= arr.size()) {
			break;
		}

		if (arr[pos] == ']')
			break;

		// This is a simplified extraction - real impl would need full parser
		std::string value;
		if (arr[pos] == '"') {
			pos++;
			while (pos < arr.size() && arr[pos] != '"') {
				value += arr[pos++];
			}
			pos++;
		} else if (arr[pos] == '{') {
			int depth = 1;
			value += arr[pos++];
			while (pos < arr.size() && depth > 0) {
				if (arr[pos] == '"') {
					value += arr[pos++];
					while (pos < arr.size() && arr[pos] != '"') {
						if (arr[pos] == '\\' && pos + 1 < arr.size()) {
							value += arr[pos++];
						}
						value += arr[pos++];
					}
					if (pos < arr.size())
						value += arr[pos++];
				} else {
					if (arr[pos] == '{')
						depth++;
					else if (arr[pos] == '}')
						depth--;
					value += arr[pos++];
				}
			}
		}

		if (!value.empty()) {
			result.push_back(value);
		}

		while (pos < arr.size() && (isWhitespace(arr[pos]) || arr[pos] == ','))
			pos++;
	}

	return result;
}

// String utilities
std::string base64Encode(const std::vector<uint8_t> &data)
{
	static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	std::string result;
	result.reserve(((data.size() + 2) / 3) * 4);

	for (size_t i = 0; i < data.size(); i += 3) {
		uint32_t n = data[i] << 16;
		if (i + 1 < data.size())
			n |= data[i + 1] << 8;
		if (i + 2 < data.size())
			n |= data[i + 2];

		result += charset[(n >> 18) & 0x3F];
		result += charset[(n >> 12) & 0x3F];
		result += (i + 1 < data.size()) ? charset[(n >> 6) & 0x3F] : '=';
		result += (i + 2 < data.size()) ? charset[n & 0x3F] : '=';
	}

	return result;
}

std::vector<uint8_t> base64Decode(const std::string &encoded)
{
	static const uint8_t lookup[256] = {
	    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63, 52, 53, 54, 55,
	    56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64, 64, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
	    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64, 64, 26, 27, 28, 29, 30, 31, 32,
	    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
	};

	std::vector<uint8_t> result;
	result.reserve(encoded.size() * 3 / 4);

	uint32_t buffer = 0;
	int bits = 0;

	for (char c : encoded) {
		if (c == '=')
			break;
		uint8_t val = lookup[static_cast<unsigned char>(c)];
		if (val == 64)
			continue;

		buffer = (buffer << 6) | val;
		bits += 6;

		if (bits >= 8) {
			bits -= 8;
			result.push_back((buffer >> bits) & 0xFF);
		}
	}

	return result;
}

std::string urlEncode(const std::string &value)
{
	std::ostringstream escaped;
	escaped.fill('0');
	escaped << std::hex;

	for (char c : value) {
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
			escaped << c;
		} else {
			escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
		}
	}

	return escaped.str();
}

std::string jsEncodeURIComponent(const std::string &value)
{
	std::ostringstream escaped;
	escaped.fill('0');
	escaped << std::hex << std::uppercase;

	for (char c : value) {
		// JS encodeURIComponent preserves: A-Z a-z 0-9 - _ . ! ~ * ' ( )
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '!' || c == '~' ||
		    c == '*' || c == '\'' || c == '(' || c == ')') {
			escaped << c;
		} else {
			escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
		}
	}

	return escaped.str();
}

std::string trim(const std::string &str)
{
	size_t start = str.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return "";
	size_t end = str.find_last_not_of(" \t\r\n");
	return str.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string &str, char delimiter)
{
	std::vector<std::string> result;
	if (str.empty()) {
		result.push_back("");
		return result;
	}
	std::stringstream ss(str);
	std::string item;
	while (std::getline(ss, item, delimiter)) {
		result.push_back(item);
	}
	return result;
}

std::vector<IceServer> parseIceServers(const std::string &config)
{
	std::vector<IceServer> servers;
	std::stringstream lines(config);
	std::string rawLine;

	auto parseEntry = [&](const std::string &entryValue) {
		std::string line = trim(entryValue);
		if (line.empty() || startsWithInsensitive(line, "#") || startsWithInsensitive(line, "//")) {
			return;
		}

		IceServer server;

		if (line.find('|') != std::string::npos) {
			const std::vector<std::string> parts = split(line, '|');
			if (!parts.empty()) {
				server.urls = trim(parts[0]);
			}
			if (parts.size() > 1) {
				server.username = trim(parts[1]);
			}
			if (parts.size() > 2) {
				server.credential = trim(parts[2]);
			}
		} else if (line.find(',') != std::string::npos) {
			const std::vector<std::string> parts = split(line, ',');
			if (!parts.empty()) {
				server.urls = trim(parts[0]);
			}
			if (parts.size() > 1) {
				server.username = trim(parts[1]);
			}
			if (parts.size() > 2) {
				server.credential = trim(parts[2]);
			}
		} else {
			std::stringstream tokenStream(line);
			std::string token;
			std::vector<std::string> tokens;
			while (tokenStream >> token) {
				tokens.push_back(token);
			}

			if (!tokens.empty()) {
				server.urls = trim(tokens[0]);
			}

			for (size_t i = 1; i < tokens.size(); ++i) {
				std::string value = trim(tokens[i]);
				const size_t equalsPos = value.find('=');
				if (equalsPos != std::string::npos) {
					std::string key = value.substr(0, equalsPos);
					std::string mapped = value.substr(equalsPos + 1);
					std::transform(key.begin(), key.end(), key.begin(),
					               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (key == "username" || key == "user") {
						server.username = mapped;
						continue;
					}
					if (key == "credential" || key == "password" || key == "pass") {
						server.credential = mapped;
						continue;
					}
				}

				if (server.username.empty()) {
					server.username = value;
				} else if (server.credential.empty()) {
					server.credential = value;
				}
			}
		}

		server.urls = trim(server.urls);
		server.username = trim(server.username);
		server.credential = trim(server.credential);
		if (!server.urls.empty() && isIceUrl(server.urls)) {
			servers.push_back(std::move(server));
		}
	};

	while (std::getline(lines, rawLine)) {
		const std::vector<std::string> entries = split(rawLine, ';');
		for (const auto &entry : entries) {
			parseEntry(entry);
		}
	}

	return servers;
}

bool countsTowardViewerLimit(ConnectionState state)
{
	switch (state) {
	case ConnectionState::New:
	case ConnectionState::Connecting:
	case ConnectionState::Connected:
		return true;
	case ConnectionState::Disconnected:
	case ConnectionState::Failed:
	case ConnectionState::Closed:
	default:
		return false;
	}
}

AspectFitLayout computeAspectFitLayout(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t outputWidth,
                                       uint32_t outputHeight)
{
	AspectFitLayout layout;
	layout.outputWidth = outputWidth;
	layout.outputHeight = outputHeight;

	if (layout.outputWidth == 0) {
		layout.outputWidth = sourceWidth;
	}
	if (layout.outputHeight == 0) {
		layout.outputHeight = sourceHeight;
	}
	if (layout.outputWidth == 0) {
		layout.outputWidth = 1;
	}
	if (layout.outputHeight == 0) {
		layout.outputHeight = 1;
	}

	if (sourceWidth == 0 || sourceHeight == 0) {
		layout.contentWidth = layout.outputWidth;
		layout.contentHeight = layout.outputHeight;
		return layout;
	}

	const double widthScale = static_cast<double>(layout.outputWidth) / static_cast<double>(sourceWidth);
	const double heightScale = static_cast<double>(layout.outputHeight) / static_cast<double>(sourceHeight);
	const double scale = std::min(widthScale, heightScale);

	layout.contentWidth =
	    std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(sourceWidth) * scale)));
	layout.contentHeight =
	    std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(sourceHeight) * scale)));
	layout.contentWidth = std::min(layout.contentWidth, layout.outputWidth);
	layout.contentHeight = std::min(layout.contentHeight, layout.outputHeight);
	layout.offsetX = (layout.outputWidth - layout.contentWidth) / 2;
	layout.offsetY = (layout.outputHeight - layout.contentHeight) / 2;
	return layout;
}

SignalingConnectErrorCategory classifySignalingConnectError(const std::string &error)
{
	const std::string normalized = trim(error);
	if (normalized.empty()) {
		return SignalingConnectErrorCategory::Unknown;
	}

	if (containsInsensitive(normalized, "tls connection failed") || containsInsensitive(normalized, "certificate") ||
	    containsInsensitive(normalized, "x509") || containsInsensitive(normalized, "unknown ca") ||
	    containsInsensitive(normalized, "hostname mismatch") || containsInsensitive(normalized, "handshake failure")) {
		return SignalingConnectErrorCategory::Tls;
	}

	if (containsInsensitive(normalized, "tcp connection failed") || containsInsensitive(normalized, "timed out") ||
	    containsInsensitive(normalized, "timeout") || containsInsensitive(normalized, "connection refused") ||
	    containsInsensitive(normalized, "network is unreachable") ||
	    containsInsensitive(normalized, "no route to host") ||
	    containsInsensitive(normalized, "name or service not known") ||
	    containsInsensitive(normalized, "temporary failure in name resolution") ||
	    containsInsensitive(normalized, "nodename nor servname")) {
		return SignalingConnectErrorCategory::Tcp;
	}

	if (containsInsensitive(normalized, "websocket connection failed") || containsInsensitive(normalized, "upgrade") ||
	    containsInsensitive(normalized, "unexpected http response") ||
	    containsInsensitive(normalized, "bad http response") || containsInsensitive(normalized, "http status")) {
		return SignalingConnectErrorCategory::WebSocketHandshake;
	}

	return SignalingConnectErrorCategory::Unknown;
}

const char *signalingConnectErrorCategoryName(SignalingConnectErrorCategory category)
{
	switch (category) {
	case SignalingConnectErrorCategory::Tcp:
		return "tcp";
	case SignalingConnectErrorCategory::Tls:
		return "tls";
	case SignalingConnectErrorCategory::WebSocketHandshake:
		return "websocket-handshake";
	case SignalingConnectErrorCategory::Unknown:
	default:
		return "unknown";
	}
}

const char *signalingConnectErrorLikelyCauses(SignalingConnectErrorCategory category)
{
	switch (category) {
	case SignalingConnectErrorCategory::Tcp:
		return "DNS failure, bad route, firewall/VPN/proxy blocking port 443, or the server/edge being unreachable";
	case SignalingConnectErrorCategory::Tls:
		return "wrong system clock, outdated CA bundle/OpenSSL, HTTPS interception, wrong IP/edge on port 443, or a "
		       "server/network device closing during TLS handshake";
	case SignalingConnectErrorCategory::WebSocketHandshake:
		return "HTTP upgrade rejected by a reverse proxy/WAF, a non-WebSocket endpoint, or a middlebox altering the "
		       "WebSocket request";
	case SignalingConnectErrorCategory::Unknown:
	default:
		return "check the surrounding [VDO.Ninja/RTC] lines; likely candidates are DNS/routing issues, TLS "
		       "interception, or the remote closing before WebSocket open";
	}
}

// Time utilities
int64_t currentTimeMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string formatTimestamp(int64_t ms)
{
	time_t seconds = ms / 1000;
	struct tm timeinfo;
#ifdef _WIN32
	localtime_s(&timeinfo, &seconds);
#else
	localtime_r(&seconds, &timeinfo);
#endif
	char buffer[32];
	strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
	return std::string(buffer);
}

// SDP manipulation
std::string modifySdpForCodec(const std::string &sdp, VideoCodec codec)
{
	// For now, return SDP as-is. Full implementation would reorder codec preferences
	// based on the desired codec
	(void)codec;
	return sdp;
}

std::string modifySdpBitrate(const std::string &sdp, int bitrate)
{
	// Add b=AS line for bandwidth limiting
	std::string result = sdp;
	std::string bLine = "b=AS:" + std::to_string(bitrate / 1000) + "\r\n";

	// Find m=video line and add bandwidth after it
	size_t videoPos = result.find("m=video");
	if (videoPos != std::string::npos) {
		size_t lineEnd = result.find("\r\n", videoPos);
		if (lineEnd != std::string::npos) {
			result.insert(lineEnd + 2, bLine);
		}
	}

	return result;
}

std::string extractMid(const std::string &sdp, const std::string &mediaType)
{
	std::string searchStr = "m=" + mediaType;
	size_t pos = sdp.find(searchStr);
	if (pos == std::string::npos)
		return "";

	// Find a=mid: line after this
	pos = sdp.find("a=mid:", pos);
	if (pos == std::string::npos)
		return "";

	pos += 6; // Skip "a=mid:"
	size_t end = sdp.find_first_of("\r\n", pos);
	if (end == std::string::npos)
		return "";

	return sdp.substr(pos, end - pos);
}

std::string stripUnsupportedTransportCcFeedback(const std::string &sdp)
{
	std::stringstream input(sdp);
	std::string line;
	std::string filtered;
	bool stripped = false;

	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.find("transport-wide-cc-extensions-01") != std::string::npos) {
			stripped = true;
			continue;
		}
		if (line.rfind("a=rtcp-fb:", 0) == 0 && line.find("transport-cc") != std::string::npos) {
			stripped = true;
			continue;
		}
		filtered += line + "\r\n";
	}

	return stripped ? filtered : sdp;
}

std::vector<SdpOfferedMediaSection> parseOfferedMediaSections(const std::string &sdp)
{
	std::vector<SdpOfferedMediaSection> sections;
	SdpOfferedMediaSection *current = nullptr;

	std::stringstream input(sdp);
	std::string line;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}

		if (line.rfind("m=", 0) == 0) {
			const auto tokens = splitWhitespace(line.substr(2));
			if (tokens.empty()) {
				current = nullptr;
				continue;
			}

			const std::string mediaType = asciiLowerCopy(tokens[0]);
			if (mediaType != "audio" && mediaType != "video") {
				current = nullptr;
				continue;
			}

			sections.push_back({});
			current = &sections.back();
			current->type = mediaType;
			for (size_t i = 3; i < tokens.size(); ++i) {
				const int payloadType = parseIntOrDefault(tokens[i]);
				if (payloadType >= 0) {
					current->payloadTypes.push_back(payloadType);
				}
			}
			continue;
		}

		if (!current) {
			continue;
		}

		if (line.rfind("a=mid:", 0) == 0) {
			current->mid = trim(line.substr(6));
			continue;
		}

		if (line.rfind("a=rtpmap:", 0) == 0) {
			const size_t separator = line.find(' ');
			if (separator == std::string::npos) {
				continue;
			}

			const int payloadType = parseIntOrDefault(line.substr(9, separator - 9));
			if (payloadType < 0 || !containsPayloadType(current->payloadTypes, payloadType)) {
				continue;
			}

			SdpOfferedCodec *codec = findCodecByPayloadType(current->codecs, payloadType);
			if (!codec) {
				current->codecs.push_back({});
				codec = &current->codecs.back();
				codec->payloadType = payloadType;
			}

			const auto descriptorParts = split(line.substr(separator + 1), '/');
			if (!descriptorParts.empty()) {
				codec->codec = descriptorParts[0];
			}
			if (descriptorParts.size() > 1) {
				codec->clockRate = parseIntOrDefault(descriptorParts[1], 0);
			}
			if (descriptorParts.size() > 2) {
				codec->channels = parseIntOrDefault(descriptorParts[2], 0);
			}
			continue;
		}

		if (line.rfind("a=fmtp:", 0) == 0) {
			const size_t separator = line.find(' ');
			if (separator == std::string::npos) {
				continue;
			}

			const int payloadType = parseIntOrDefault(line.substr(7, separator - 7));
			if (payloadType < 0 || !containsPayloadType(current->payloadTypes, payloadType)) {
				continue;
			}

			SdpOfferedCodec *codec = findCodecByPayloadType(current->codecs, payloadType);
			if (!codec) {
				current->codecs.push_back({});
				codec = &current->codecs.back();
				codec->payloadType = payloadType;
			}

			codec->formatParameters = trim(line.substr(separator + 1));
			const std::string fmtpLower = asciiLowerCopy(codec->formatParameters);
			const size_t aptPos = fmtpLower.find("apt=");
			if (aptPos != std::string::npos) {
				size_t valueStart = aptPos + 4;
				size_t valueEnd = valueStart;
				while (valueEnd < codec->formatParameters.size() &&
				       std::isdigit(static_cast<unsigned char>(codec->formatParameters[valueEnd]))) {
					++valueEnd;
				}
				codec->associatedPayloadType =
				    parseIntOrDefault(codec->formatParameters.substr(valueStart, valueEnd - valueStart));
			}
		}
	}

	return sections;
}

// Logging
void logInfo(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	blog(LOG_INFO, "[VDO.Ninja] %s", buffer);
}

void logWarning(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	blog(LOG_WARNING, "[VDO.Ninja] %s", buffer);
}

void logError(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	blog(LOG_ERROR, "[VDO.Ninja] %s", buffer);
}

void logDebug(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	blog(LOG_DEBUG, "[VDO.Ninja] %s", buffer);
}

} // namespace vdoninja
