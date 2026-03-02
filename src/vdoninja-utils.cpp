/*
 * OBS VDO.Ninja Plugin
 * Utility function implementations
 */

#include "vdoninja-utils.h"

#include <obs-module.h>

#include <algorithm>
#include <array>
#include <cctype>
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

	// SDK convention: streamID + first 6 hex chars of sha256(password + salt).
	std::string passwordHash = sha256(normalizedPassword + salt).substr(0, 6);
	return sanitized + passwordHash;
}

std::string hashRoomId(const std::string &roomId, const std::string &password, const std::string &salt)
{
	std::string sanitized = sanitizeIdentifier(roomId, 30);
	const std::string normalizedPassword = trim(password);

	if (normalizedPassword.empty() || isPasswordDisabledToken(normalizedPassword)) {
		return sanitized;
	}

	std::string combined = sanitized + normalizedPassword + salt;
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
		suffixes.push_back(sha256(normalizedPassword + salt).substr(0, 6));
	}
	suffixes.push_back(sha256(std::string(DEFAULT_PASSWORD) + salt).substr(0, 6));

	for (const auto &suffix : suffixes) {
		if (!suffix.empty() && viewId.size() > suffix.size() &&
		    viewId.compare(viewId.size() - suffix.size(), suffix.size(), suffix) == 0) {
			viewId.resize(viewId.size() - suffix.size());
			break;
		}
	}

	return viewId;
}

std::string buildInboundViewUrl(const std::string &baseUrl, const std::string &streamId, const std::string &password,
                                const std::string &roomId, const std::string &salt)
{
	// Accept direct WHEP URLs when signaling metadata provides one.
	if (streamId.rfind("http://", 0) == 0 || streamId.rfind("https://", 0) == 0) {
		return streamId;
	}

	if (streamId.rfind("whep:", 0) == 0) {
		return streamId.substr(5);
	}

	const std::string normalizedBaseUrl = baseUrl.empty() ? "https://vdo.ninja" : baseUrl;
	const std::string normalizedPassword = trim(password);
	const bool passwordDisabled = isPasswordDisabledToken(normalizedPassword);
	const std::string viewId = deriveViewStreamId(streamId, normalizedPassword, salt);

	std::string url = normalizedBaseUrl + "/?view=" + urlEncode(viewId);
	if (!roomId.empty()) {
		url += "&solo&room=" + urlEncode(roomId);
	}

	if (!normalizedPassword.empty()) {
		url += passwordDisabled ? "&password=false" : "&password=" + urlEncode(normalizedPassword);
	}

	return url;
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

	// Skip whitespace and opening brace
	while (pos < json_.size() && (std::isspace(json_[pos]) || json_[pos] == '{'))
		pos++;

	while (pos < json_.size() && json_[pos] != '}') {
		// Skip whitespace
		while (pos < json_.size() && std::isspace(json_[pos]))
			pos++;

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
		pos++; // Skip colon

		// Skip whitespace
		while (pos < json_.size() && std::isspace(json_[pos]))
			pos++;

		// Extract value
		std::string value = extractValue(pos);
		values_[key] = value;

		// Skip comma and whitespace
		while (pos < json_.size() && (std::isspace(json_[pos]) || json_[pos] == ','))
			pos++;
	}
}

std::string JsonParser::extractValue(size_t &pos) const
{
	std::string value;

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
		// Object - capture the whole thing
		int depth = 1;
		value += json_[pos++];
		while (pos < json_.size() && depth > 0) {
			if (json_[pos] == '{')
				depth++;
			else if (json_[pos] == '}')
				depth--;
			value += json_[pos++];
		}
	} else if (json_[pos] == '[') {
		// Array - capture the whole thing
		int depth = 1;
		value += json_[pos++];
		while (pos < json_.size() && depth > 0) {
			if (json_[pos] == '[')
				depth++;
			else if (json_[pos] == ']')
				depth--;
			value += json_[pos++];
		}
	} else {
		// Number, boolean, or null
		while (pos < json_.size() && json_[pos] != ',' && json_[pos] != '}' && !std::isspace(json_[pos])) {
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

	if (arr.empty() || arr[0] != '[')
		return result;

	size_t pos = 1;
	while (pos < arr.size() && arr[pos] != ']') {
		while (pos < arr.size() && std::isspace(arr[pos]))
			pos++;

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
				if (arr[pos] == '{')
					depth++;
				else if (arr[pos] == '}')
					depth--;
				value += arr[pos++];
			}
		}

		if (!value.empty()) {
			result.push_back(value);
		}

		while (pos < arr.size() && (std::isspace(arr[pos]) || arr[pos] == ','))
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
