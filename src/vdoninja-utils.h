/*
 * OBS VDO.Ninja Plugin
 * Utility functions for hashing, JSON handling, and string manipulation
 */

#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "vdoninja-common.h"

namespace vdoninja
{

struct AspectFitLayout {
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint32_t contentWidth = 0;
	uint32_t contentHeight = 0;
	uint32_t offsetX = 0;
	uint32_t offsetY = 0;
};

struct SdpOfferedCodec {
	int payloadType = -1;
	std::string codec;
	int clockRate = 0;
	int channels = 0;
	std::string formatParameters;
	int associatedPayloadType = -1;
};

struct SdpOfferedMediaSection {
	std::string type;
	std::string mid;
	std::vector<int> payloadTypes;
	std::vector<SdpOfferedCodec> codecs;
};

// UUID generation
std::string generateUUID();

// Session ID generation (8 characters)
std::string generateSessionId();

// SHA-256 based hashing for stream/room IDs (matching VDO.Ninja SDK)
std::string sha256(const std::string &input);
bool isPasswordDisabledToken(const std::string &password);
std::string hashStreamId(const std::string &streamId, const std::string &password, const std::string &salt);
std::string hashRoomId(const std::string &roomId, const std::string &password, const std::string &salt);
std::string deriveViewStreamId(const std::string &streamId, const std::string &password, const std::string &salt);
std::string buildViewerPageUrl(const std::string &baseUrl, const std::string &streamId, const std::string &password,
                               const std::string &roomId, const std::string &salt, const std::string &wssHost = "");
std::string buildInboundViewUrl(const std::string &baseUrl, const std::string &streamId, const std::string &password,
                                const std::string &roomId, const std::string &salt);
int chooseViewerTargetBitrateKbps(uint32_t width, uint32_t height);
std::string buildViewerRequestMessage(uint32_t width, uint32_t height, bool guest);

// Sanitize stream ID (replace non-alphanumeric with underscores)
std::string sanitizeStreamId(const std::string &streamId);

// Simple JSON utilities (avoiding external JSON library dependency)
class JsonBuilder
{
public:
	JsonBuilder &add(const std::string &key, const std::string &value);
	JsonBuilder &add(const std::string &key, const char *value);
	JsonBuilder &add(const std::string &key, int value);
	JsonBuilder &add(const std::string &key, int64_t value);
	JsonBuilder &add(const std::string &key, bool value);
	JsonBuilder &addRaw(const std::string &key, const std::string &rawJson);
	std::string build() const;

private:
	std::vector<std::pair<std::string, std::string>> entries_;
};

class JsonParser
{
public:
	explicit JsonParser(const std::string &json);

	bool hasKey(const std::string &key) const;
	std::string getString(const std::string &key, const std::string &defaultValue = "") const;
	int getInt(const std::string &key, int defaultValue = 0) const;
	bool getBool(const std::string &key, bool defaultValue = false) const;
	std::string getRaw(const std::string &key) const;

	// Get nested object as raw JSON
	std::string getObject(const std::string &key) const;

	// Get array elements
	std::vector<std::string> getArray(const std::string &key) const;

private:
	std::string json_;
	std::map<std::string, std::string> values_;
	void parse();
	std::string extractValue(size_t &pos) const;
};

// String utilities
std::string base64Encode(const std::vector<uint8_t> &data);
std::vector<uint8_t> base64Decode(const std::string &encoded);
std::string urlEncode(const std::string &value);
std::string jsEncodeURIComponent(const std::string &value);
std::string trim(const std::string &str);
std::vector<std::string> split(const std::string &str, char delimiter);
std::vector<IceServer> parseIceServers(const std::string &config);
bool countsTowardViewerLimit(ConnectionState state);
AspectFitLayout computeAspectFitLayout(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t outputWidth,
                                       uint32_t outputHeight);

// Time utilities
int64_t currentTimeMs();
std::string formatTimestamp(int64_t ms);

// SDP manipulation utilities
std::string modifySdpForCodec(const std::string &sdp, VideoCodec codec);
std::string modifySdpBitrate(const std::string &sdp, int bitrate);
std::string extractMid(const std::string &sdp, const std::string &mediaType);
std::string stripUnsupportedTransportCcFeedback(const std::string &sdp);
std::vector<SdpOfferedMediaSection> parseOfferedMediaSections(const std::string &sdp);

// Logging helpers
void logInfo(const char *format, ...);
void logWarning(const char *format, ...);
void logError(const char *format, ...);
void logDebug(const char *format, ...);

} // namespace vdoninja
