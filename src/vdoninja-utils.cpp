/*
 * OBS VDO.Ninja Plugin
 * Utility function implementations
 */

#include "vdoninja-utils.h"
#include <obs-module.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <cstdarg>
#include <algorithm>
#include <cctype>
#include <regex>

namespace vdoninja {

// UUID Generation
std::string generateUUID()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;

    for (int i = 0; i < 8; i++)
        ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++)
        ss << dis(gen);
    ss << "-4";  // Version 4
    for (int i = 0; i < 3; i++)
        ss << dis(gen);
    ss << "-";
    ss << dis2(gen);  // Variant
    for (int i = 0; i < 3; i++)
        ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++)
        ss << dis(gen);

    return ss.str();
}

std::string generateSessionId()
{
    static const char alphanum[] =
        "0123456789"
        "abcdefghijklmnopqrstuvwxyz";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

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
    unsigned char hash[SHA256_DIGEST_LENGTH];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();

    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, input.c_str(), input.size());
    EVP_DigestFinal_ex(ctx, hash, nullptr);
    EVP_MD_CTX_free(ctx);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

// Hash stream ID matching VDO.Ninja SDK algorithm
std::string hashStreamId(const std::string &streamId, const std::string &password, const std::string &salt)
{
    std::string sanitized = sanitizeStreamId(streamId);

    // If no password, just return sanitized stream ID
    if (password.empty()) {
        return sanitized;
    }

    // Hash: sha256(streamId + password + salt) truncated
    std::string combined = sanitized + password + salt;
    std::string fullHash = sha256(combined);

    // VDO.Ninja uses first 16 characters of the hash
    return fullHash.substr(0, 16);
}

std::string hashRoomId(const std::string &roomId, const std::string &password, const std::string &salt)
{
    std::string sanitized = sanitizeStreamId(roomId);

    if (password.empty()) {
        return sanitized;
    }

    std::string combined = sanitized + password + salt;
    std::string fullHash = sha256(combined);
    return fullHash.substr(0, 16);
}

std::string sanitizeStreamId(const std::string &streamId)
{
    std::string result;
    result.reserve(streamId.size());

    for (char c : streamId) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result += std::tolower(static_cast<unsigned char>(c));
        } else {
            result += '_';
        }
    }
    return result;
}

// JSON Builder implementation
JsonBuilder &JsonBuilder::add(const std::string &key, const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped += '"';
    for (char c : value) {
        switch (c) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += c; break;
        }
    }
    escaped += '"';
    entries_.emplace_back(key, escaped);
    return *this;
}

JsonBuilder &JsonBuilder::add(const std::string &key, int value)
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
        if (i > 0) ss << ",";
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

        if (json_[pos] != '"') break;
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
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                default: value += json_[pos]; break;
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
            if (json_[pos] == '{') depth++;
            else if (json_[pos] == '}') depth--;
            value += json_[pos++];
        }
    } else if (json_[pos] == '[') {
        // Array - capture the whole thing
        int depth = 1;
        value += json_[pos++];
        while (pos < json_.size() && depth > 0) {
            if (json_[pos] == '[') depth++;
            else if (json_[pos] == ']') depth--;
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

    if (arr.empty() || arr[0] != '[') return result;

    size_t pos = 1;
    while (pos < arr.size() && arr[pos] != ']') {
        while (pos < arr.size() && std::isspace(arr[pos]))
            pos++;

        if (arr[pos] == ']') break;

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
                if (arr[pos] == '{') depth++;
                else if (arr[pos] == '}') depth--;
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
        if (i + 1 < data.size()) n |= data[i + 1] << 8;
        if (i + 2 < data.size()) n |= data[i + 2];

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
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
        64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
        64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
    };

    std::vector<uint8_t> result;
    result.reserve(encoded.size() * 3 / 4);

    uint32_t buffer = 0;
    int bits = 0;

    for (char c : encoded) {
        if (c == '=') break;
        uint8_t val = lookup[static_cast<unsigned char>(c)];
        if (val == 64) continue;

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
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string &str, char delimiter)
{
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delimiter)) {
        result.push_back(item);
    }
    return result;
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
    if (pos == std::string::npos) return "";

    // Find a=mid: line after this
    pos = sdp.find("a=mid:", pos);
    if (pos == std::string::npos) return "";

    pos += 6; // Skip "a=mid:"
    size_t end = sdp.find_first_of("\r\n", pos);
    if (end == std::string::npos) return "";

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
