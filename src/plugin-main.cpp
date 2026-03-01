/*
 * OBS VDO.Ninja Plugin
 * Main plugin entry point
 *
 * This plugin provides VDO.Ninja integration for OBS Studio:
 * - Output: Stream to VDO.Ninja (multiple P2P viewers)
 * - Source: View VDO.Ninja streams
 * - Virtual Camera: Go live to VDO.Ninja
 * - Data Channels: Tally, chat, remote control
 */

#include "plugin-main.h"
#include "vdoninja-dock.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-vdoninja", "en")

#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "vdoninja-output.h"
#include "vdoninja-source.h"
#include "vdoninja-utils.h"

using namespace vdoninja;

static void vdoninja_service_defaults(obs_data_t *settings);

namespace
{
void syncCompatibilityServiceFields(obs_data_t *settings);

constexpr const char *kVdoNinjaServiceType = "vdoninja_service";
constexpr const char *kVdoNinjaServiceName = "default_service";
constexpr const char *kRtmpServicesModuleName = "rtmp-services";
constexpr const char *kVdoCatalogServiceName = "VDO.Ninja";
constexpr const char *kVdoNinjaControlCenterSourceId = "vdoninja_control_center";
constexpr const char *kVdoNinjaControlCenterSourceName = "VDO.Ninja Control Center";
constexpr const char *kVdoNinjaDocsHomeLink = "https://steveseguin.github.io/ninja-obs-plugin/";
constexpr const char *kVdoNinjaQuickStartLink = "https://steveseguin.github.io/ninja-obs-plugin/#quick-start";
constexpr const char *kVdoNinjaServerDisplayName =
    "wss://wss.vdo.ninja:443 (open Tools -> VDO.Ninja Studio for stream ID/password/room)";

struct ControlCenterContext {
	obs_source_t *source = nullptr;
	uint64_t previousTotalBytes = 0;
	int64_t previousSampleTimeMs = 0;
};

struct ServiceSnapshot {
	std::string serviceType;
	obs_data_t *settings = nullptr;
	obs_data_t *hotkeys = nullptr;
};

obs_source_t *gControlCenterSource = nullptr;
ServiceSnapshot gLastNonVdoServiceSnapshot = {};
ServiceSnapshot gTemporaryRestoreSnapshot = {};
VDONinjaDock *g_vdo_dock = nullptr;

constexpr const char *kVdoNinjaRtmpServiceEntry = R"VDOJSON(
        {
            "name": "VDO.Ninja",
            "common": true,
            "protocol": "VDO.Ninja",
            "stream_key_link": "https://steveseguin.github.io/ninja-obs-plugin/#quick-start",
            "more_info_link": "https://steveseguin.github.io/ninja-obs-plugin/",
            "servers": [
                {
                    "name": "wss://wss.vdo.ninja:443 (open Tools -> VDO.Ninja Studio for stream ID/password/room)",
                    "url": "wss://wss.vdo.ninja:443"
                }
            ],
            "supported video codecs": [
                "h264"
            ],
            "supported audio codecs": [
                "opus"
            ],
            "recommended": {
                "keyint": 2,
                "bframes": 0,
                "max audio bitrate": 320,
                "max video bitrate": 12000
            }
        })VDOJSON";

const char *tr(const char *key, const char *fallback)
{
	const char *localized = obs_module_text(key);
	if (!localized || !*localized || std::strcmp(localized, key) == 0) {
		return fallback;
	}
	return localized;
}

std::string findAudioEncoderIdForCodec(const char *codec)
{
	if (!codec || !*codec) {
		return "";
	}

	const char *encoderId = nullptr;
	size_t idx = 0;
	while (obs_enum_encoder_types(idx++, &encoderId)) {
		if (!encoderId || obs_get_encoder_type(encoderId) != OBS_ENCODER_AUDIO) {
			continue;
		}

		const char *encoderCodec = obs_get_encoder_codec(encoderId);
		if (encoderCodec && std::strcmp(encoderCodec, codec) == 0) {
			return encoderId;
		}
	}

	return "";
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

bool hasCatalogServiceEntry(const std::string &json, const char *serviceName)
{
	if (!serviceName || !*serviceName) {
		return false;
	}

	const std::string quotedService = std::string("\"name\": \"") + serviceName + "\"";
	const std::string compactService = std::string("\"name\":\"") + serviceName + "\"";
	return json.find(quotedService) != std::string::npos || json.find(compactService) != std::string::npos;
}

bool replaceFirst(std::string &text, const std::string &from, const std::string &to)
{
	if (from.empty()) {
		return false;
	}

	const size_t pos = text.find(from);
	if (pos == std::string::npos) {
		return false;
	}

	text.replace(pos, from.size(), to);
	return true;
}

size_t findMatchingClosingBracket(const std::string &text, size_t openingIndex)
{
	if (openingIndex >= text.size() || text[openingIndex] != '[') {
		return std::string::npos;
	}

	bool inString = false;
	bool escaped = false;
	int depth = 0;
	for (size_t i = openingIndex; i < text.size(); ++i) {
		const char c = text[i];
		if (inString) {
			if (escaped) {
				escaped = false;
				continue;
			}
			if (c == '\\') {
				escaped = true;
				continue;
			}
			if (c == '"') {
				inString = false;
			}
			continue;
		}

		if (c == '"') {
			inString = true;
			continue;
		}
		if (c == '[') {
			++depth;
			continue;
		}
		if (c == ']') {
			--depth;
			if (depth == 0) {
				return i;
			}
		}
	}

	return std::string::npos;
}

size_t findMatchingClosingBrace(const std::string &text, size_t openingIndex)
{
	if (openingIndex >= text.size() || text[openingIndex] != '{') {
		return std::string::npos;
	}

	bool inString = false;
	bool escaped = false;
	int depth = 0;
	for (size_t i = openingIndex; i < text.size(); ++i) {
		const char c = text[i];
		if (inString) {
			if (escaped) {
				escaped = false;
				continue;
			}
			if (c == '\\') {
				escaped = true;
				continue;
			}
			if (c == '"') {
				inString = false;
			}
			continue;
		}

		if (c == '"') {
			inString = true;
			continue;
		}
		if (c == '{') {
			++depth;
			continue;
		}
		if (c == '}') {
			--depth;
			if (depth == 0) {
				return i;
			}
		}
	}

	return std::string::npos;
}

bool serviceEntryContainsToken(const std::string &json, const char *serviceName, const char *token)
{
	if (!serviceName || !*serviceName || !token || !*token) {
		return false;
	}

	size_t servicePos = json.find(std::string("\"name\": \"") + serviceName + "\"");
	if (servicePos == std::string::npos) {
		servicePos = json.find(std::string("\"name\":\"") + serviceName + "\"");
		if (servicePos == std::string::npos) {
			return false;
		}
	}

	const size_t entryStart = json.rfind('{', servicePos);
	if (entryStart == std::string::npos) {
		return false;
	}

	const size_t entryEnd = findMatchingClosingBrace(json, entryStart);
	if (entryEnd == std::string::npos || entryEnd <= entryStart) {
		return false;
	}

	const size_t tokenPos = json.find(token, entryStart);
	return tokenPos != std::string::npos && tokenPos < entryEnd;
}

bool injectServiceIntoCatalog(std::string &catalogJson, const char *serviceEntry)
{
	if (!serviceEntry || !*serviceEntry) {
		return false;
	}

	const size_t servicesKey = catalogJson.find("\"services\"");
	if (servicesKey == std::string::npos) {
		return false;
	}

	const size_t servicesArrayStart = catalogJson.find('[', servicesKey);
	if (servicesArrayStart == std::string::npos) {
		return false;
	}

	const size_t servicesArrayEnd = findMatchingClosingBracket(catalogJson, servicesArrayStart);
	if (servicesArrayEnd == std::string::npos) {
		return false;
	}

	bool hasExistingEntries = false;
	for (size_t i = servicesArrayStart + 1; i < servicesArrayEnd; ++i) {
		const char c = catalogJson[i];
		if (!std::isspace(static_cast<unsigned char>(c))) {
			hasExistingEntries = true;
			break;
		}
	}

	std::string insertion = hasExistingEntries ? ",\n" : "\n";
	insertion += serviceEntry;
	insertion += "\n";

	catalogJson.insert(servicesArrayEnd, insertion);
	return true;
}

void ensureRtmpCatalogHasVdoNinjaEntry(void)
{
	obs_module_t *rtmpServicesModule = obs_get_module(kRtmpServicesModuleName);
	if (!rtmpServicesModule) {
		logWarning("rtmp-services module not found; cannot inject VDO.Ninja stream destination");
		return;
	}

	char *configPath = obs_module_get_config_path(rtmpServicesModule, "services.json");
	if (!configPath) {
		logWarning("Failed to get rtmp-services config path");
		return;
	}

	std::string catalogJson;
	if (char *configCatalog = os_quick_read_utf8_file(configPath)) {
		catalogJson = configCatalog;
		bfree(configCatalog);
	}

	if (catalogJson.empty()) {
		char *defaultCatalogPath = obs_find_module_file(rtmpServicesModule, "services.json");
		if (defaultCatalogPath) {
			if (char *defaultCatalog = os_quick_read_utf8_file(defaultCatalogPath)) {
				catalogJson = defaultCatalog;
				bfree(defaultCatalog);
			}
			bfree(defaultCatalogPath);
		}
	}

	if (catalogJson.empty()) {
		logWarning("Unable to load rtmp-services catalog for VDO.Ninja service injection");
		bfree(configPath);
		return;
	}

	bool updatedExistingEntry = false;
	if (hasCatalogServiceEntry(catalogJson, kVdoCatalogServiceName)) {
		const std::string quickStartField = std::string("\"stream_key_link\": \"") + kVdoNinjaQuickStartLink + "\"";
		const std::string quickStartFieldCompact = std::string("\"stream_key_link\":\"") + kVdoNinjaQuickStartLink + "\"";
		const std::string moreInfoField = std::string("\"more_info_link\": \"") + kVdoNinjaDocsHomeLink + "\"";
		const std::string moreInfoFieldCompact = std::string("\"more_info_link\":\"") + kVdoNinjaDocsHomeLink + "\"";
		updatedExistingEntry |= replaceFirst(catalogJson, "\"stream_key_link\": \"https://vdo.ninja/\"", quickStartField);
		updatedExistingEntry |= replaceFirst(catalogJson, "\"stream_key_link\":\"https://vdo.ninja/\"", quickStartFieldCompact);
		updatedExistingEntry |= replaceFirst(catalogJson,
		                                     "\"stream_key_link\": "
		                                     "\"https://github.com/steveseguin/ninja-obs-plugin/blob/main/QUICKSTART.md#2-publish-your-first-stream\"",
		                                     quickStartField);
		updatedExistingEntry |= replaceFirst(catalogJson,
		                                     "\"stream_key_link\":"
		                                     "\"https://github.com/steveseguin/ninja-obs-plugin/blob/main/QUICKSTART.md#2-publish-your-first-stream\"",
		                                     quickStartFieldCompact);
		updatedExistingEntry |= replaceFirst(catalogJson,
		                                     "\"more_info_link\": "
		                                     "\"https://github.com/steveseguin/ninja-obs-plugin/blob/main/README.md#2-publish-to-vdoninja\"",
		                                     moreInfoField);
		updatedExistingEntry |= replaceFirst(catalogJson,
		                                     "\"more_info_link\":\"https://github.com/steveseguin/ninja-obs-plugin/blob/main/README.md#2-publish-to-vdoninja\"",
		                                     moreInfoFieldCompact);
		updatedExistingEntry |= replaceFirst(catalogJson, "\"more_info_link\": \"https://vdo.ninja/\"", moreInfoField);
		updatedExistingEntry |= replaceFirst(catalogJson, "\"more_info_link\":\"https://vdo.ninja/\"", moreInfoFieldCompact);

		if (!serviceEntryContainsToken(catalogJson, kVdoCatalogServiceName, "\"stream_key_link\"")) {
			updatedExistingEntry |= replaceFirst(
			    catalogJson, "\"protocol\": \"VDO.Ninja\",",
			    std::string("\"protocol\": \"VDO.Ninja\",\n            ") + quickStartField + ",");
			updatedExistingEntry |= replaceFirst(catalogJson, "\"protocol\":\"VDO.Ninja\",",
			                                     std::string("\"protocol\":\"VDO.Ninja\",") + quickStartFieldCompact + ",");
		}

		updatedExistingEntry |= replaceFirst(catalogJson, "\"name\": \"Default Signaling\"",
		                                     std::string("\"name\": \"") + kVdoNinjaServerDisplayName + "\"");
		updatedExistingEntry |= replaceFirst(catalogJson, "\"name\":\"Default Signaling\"",
		                                     std::string("\"name\":\"") + kVdoNinjaServerDisplayName + "\"");
		updatedExistingEntry |= replaceFirst(
		    catalogJson, "\"name\": \"wss://wss.vdo.ninja:443 (default; override via Stream Key URL)\"",
		    std::string("\"name\": \"") + kVdoNinjaServerDisplayName + "\"");
		updatedExistingEntry |= replaceFirst(
		    catalogJson, "\"name\":\"wss://wss.vdo.ninja:443 (default; override via Stream Key URL)\"",
		    std::string("\"name\":\"") + kVdoNinjaServerDisplayName + "\"");
		updatedExistingEntry |= replaceFirst(
		    catalogJson,
		    "\"name\": \"wss://wss.vdo.ninja:443 (default; use Tools -> Configure VDO.Ninja for password/room/salt)\"",
		    std::string("\"name\": \"") + kVdoNinjaServerDisplayName + "\"");
		updatedExistingEntry |= replaceFirst(
		    catalogJson,
		    "\"name\":\"wss://wss.vdo.ninja:443 (default; use Tools -> Configure VDO.Ninja for password/room/salt)\"",
		    std::string("\"name\":\"") + kVdoNinjaServerDisplayName + "\"");
		updatedExistingEntry |= replaceFirst(
		    catalogJson, "\"name\": \"wss://wss.vdo.ninja:443 (default; use Tools -> Configure VDO.Ninja)\"",
		    std::string("\"name\": \"") + kVdoNinjaServerDisplayName + "\"");
		updatedExistingEntry |= replaceFirst(
		    catalogJson, "\"name\":\"wss://wss.vdo.ninja:443 (default; use Tools -> Configure VDO.Ninja)\"",
		    std::string("\"name\":\"") + kVdoNinjaServerDisplayName + "\"");
	}

	bool injectedEntry = false;
	if (!hasCatalogServiceEntry(catalogJson, kVdoCatalogServiceName)) {
		if (!injectServiceIntoCatalog(catalogJson, kVdoNinjaRtmpServiceEntry)) {
			logWarning("Failed to inject VDO.Ninja into rtmp-services catalog");
			bfree(configPath);
			return;
		}
		injectedEntry = true;
	}

	if (!injectedEntry && !updatedExistingEntry) {
		bfree(configPath);
		return;
	}

	try {
		std::filesystem::create_directories(std::filesystem::path(configPath).parent_path());
	} catch (const std::exception &e) {
		logWarning("Failed creating rtmp-services config directory: %s", e.what());
		bfree(configPath);
		return;
	}

	const bool wroteCatalog = os_quick_write_utf8_file_safe(configPath, catalogJson.c_str(), catalogJson.size(), false,
	                                                         ".tmp", ".bak") ||
	                          os_quick_write_utf8_file(configPath, catalogJson.c_str(), catalogJson.size(), false);
	if (!wroteCatalog) {
		logWarning("Failed writing VDO.Ninja entry to rtmp-services catalog");
	} else {
		if (injectedEntry) {
			logInfo("Injected VDO.Ninja into rtmp-services catalog at: %s", configPath);
		} else if (updatedExistingEntry) {
			logInfo("Updated VDO.Ninja catalog metadata in rtmp-services config: %s", configPath);
		}
	}

	bfree(configPath);
}

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

void parseVdoStreamKey(const std::string &keyValue, std::string &streamId, std::string &password, std::string &roomId,
                       std::string &salt, std::string &wssHost, bool allowBareStreamId = true)
{
	if (keyValue.empty()) {
		return;
	}

	const bool hasQuery = keyValue.find('?') != std::string::npos;
	const bool keyLooksLikeUrl =
	    startsWithInsensitive(keyValue, "https://") || startsWithInsensitive(keyValue, "http://") ||
	    (hasQuery && (keyValue.find("push=") != std::string::npos || keyValue.find("view=") != std::string::npos));

	if (keyLooksLikeUrl) {
		const std::string push = queryValue(keyValue, "push");
		const std::string view = queryValue(keyValue, "view");
		if (streamId.empty()) {
			if (!push.empty()) {
				streamId = push;
			} else if (!view.empty()) {
				streamId = view;
			}
		}

		if (password.empty()) {
			password = queryValue(keyValue, "password");
			if (password.empty()) {
				password = queryValue(keyValue, "pasword");
			}
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
				wssHost = queryValue(keyValue, "wss_host");
			}
			if (wssHost.empty()) {
				wssHost = queryValue(keyValue, "server");
			}
			if (wssHost.empty()) {
				wssHost = queryValue(keyValue, "signaling");
			}
		}
		return;
	}

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
		streamId = keyValue;
	}
}

void seedVdoNinjaSettingsFromCurrentService(obs_service_t *currentService, obs_data_t *settings)
{
	if (!currentService || !settings) {
		return;
	}

	const char *currentType = obs_service_get_type(currentService);
	obs_data_t *currentSettings = obs_service_get_settings(currentService);
	if (!currentSettings) {
		return;
	}

	if (currentType && std::strcmp(currentType, kVdoNinjaServiceType) == 0) {
		obs_data_apply(settings, currentSettings);
		// Normalize compatibility fields so key-only configs populate
		// stream_id/password/room/salt/wss in Tools -> VDO.Ninja Studio.
		syncCompatibilityServiceFields(settings);
		obs_data_release(currentSettings);
		return;
	}

	const std::string keyValue = obs_data_get_string(currentSettings, "key");
	std::string streamId;
	std::string password;
	std::string roomId;
	std::string salt;
	std::string wssHost;
	parseVdoStreamKey(keyValue, streamId, password, roomId, salt, wssHost, false);
	if (!streamId.empty()) {
		obs_data_set_string(settings, "stream_id", streamId.c_str());
	}
	if (!password.empty()) {
		obs_data_set_string(settings, "password", password.c_str());
	}
	if (!roomId.empty()) {
		obs_data_set_string(settings, "room_id", roomId.c_str());
	}
	if (!salt.empty()) {
		obs_data_set_string(settings, "salt", salt.c_str());
	}
	if (!wssHost.empty()) {
		obs_data_set_string(settings, "wss_host", wssHost.c_str());
	}

	const std::string wsServer = obs_data_get_string(currentSettings, "server");
	if (wssHost.empty() && !wsServer.empty() &&
	    (startsWithInsensitive(wsServer, "wss://") || startsWithInsensitive(wsServer, "ws://"))) {
		obs_data_set_string(settings, "wss_host", wsServer.c_str());
	}

	obs_data_release(currentSettings);
}

void syncCompatibilityServiceFields(obs_data_t *settings)
{
	if (!settings) {
		return;
	}

	obs_data_set_string(settings, "service", kVdoCatalogServiceName);
	obs_data_set_string(settings, "protocol", "VDO.Ninja");

	std::string streamId = trim(obs_data_get_string(settings, "stream_id"));
	std::string password = obs_data_get_string(settings, "password");
	std::string roomId = obs_data_get_string(settings, "room_id");
	std::string salt = obs_data_get_string(settings, "salt");
	std::string wssHost = obs_data_get_string(settings, "wss_host");

	if (streamId.empty()) {
		std::string parsedSalt;
		const std::string keyValue = obs_data_get_string(settings, "key");
		parseVdoStreamKey(keyValue, streamId, password, roomId, parsedSalt, wssHost);
		if (salt.empty() && !parsedSalt.empty()) {
			salt = parsedSalt;
		}
	}

	if (wssHost.empty()) {
		const char *serverValue = obs_data_get_string(settings, "server");
		if (serverValue && *serverValue &&
		    (startsWithInsensitive(serverValue, "wss://") || startsWithInsensitive(serverValue, "ws://"))) {
			wssHost = serverValue;
		}
	}

	if (!streamId.empty()) {
		obs_data_set_string(settings, "stream_id", streamId.c_str());
	}
	if (!password.empty()) {
		obs_data_set_string(settings, "password", password.c_str());
	}
	if (!roomId.empty()) {
		obs_data_set_string(settings, "room_id", roomId.c_str());
	}
	if (!salt.empty()) {
		obs_data_set_string(settings, "salt", salt.c_str());
	}
	if (!wssHost.empty()) {
		obs_data_set_string(settings, "wss_host", wssHost.c_str());
	}
}

void configureProfileForVdoNinjaStreaming(void)
{
	// Removed profile-wide modifications to avoid conflicts with RTMP/WHIP.
	// We now prefer surgical configuration only when VDO.Ninja output is explicitly active.
}

bool isVdoNinjaService(obs_service_t *service)
{
	if (!service) {
		return false;
	}
	const char *serviceType = obs_service_get_type(service);
	return serviceType && std::strcmp(serviceType, kVdoNinjaServiceType) == 0;
}

void releaseServiceSnapshot(ServiceSnapshot &snapshot)
{
	if (snapshot.settings) {
		obs_data_release(snapshot.settings);
		snapshot.settings = nullptr;
	}
	if (snapshot.hotkeys) {
		obs_data_release(snapshot.hotkeys);
		snapshot.hotkeys = nullptr;
	}
	snapshot.serviceType.clear();
}

bool hasServiceSnapshot(const ServiceSnapshot &snapshot)
{
	return !snapshot.serviceType.empty() && snapshot.settings != nullptr;
}

void cloneServiceSnapshot(const ServiceSnapshot &src, ServiceSnapshot &dst)
{
	releaseServiceSnapshot(dst);
	if (!hasServiceSnapshot(src)) {
		return;
	}

	dst.serviceType = src.serviceType;
	dst.settings = obs_data_create();
	obs_data_apply(dst.settings, src.settings);
	if (src.hotkeys) {
		dst.hotkeys = obs_data_create();
		obs_data_apply(dst.hotkeys, src.hotkeys);
	}
}

bool captureServiceSnapshot(obs_service_t *service, ServiceSnapshot &snapshot)
{
	releaseServiceSnapshot(snapshot);
	if (!service) {
		return false;
	}

	const char *serviceType = obs_service_get_type(service);
	if (!serviceType || !*serviceType) {
		return false;
	}

	obs_data_t *settings = obs_service_get_settings(service);
	if (!settings) {
		return false;
	}

	snapshot.serviceType = serviceType;
	snapshot.settings = settings;
	snapshot.hotkeys = obs_hotkeys_save_service(service);
	return true;
}

void captureLastNonVdoServiceSnapshot(obs_service_t *service)
{
	if (!service || isVdoNinjaService(service)) {
		return;
	}

	captureServiceSnapshot(service, gLastNonVdoServiceSnapshot);
}

void clearTemporaryServiceRestoreBackup()
{
	releaseServiceSnapshot(gTemporaryRestoreSnapshot);
}

void backupServiceForTemporaryRestore(obs_service_t *service)
{
	if (service && !isVdoNinjaService(service)) {
		captureServiceSnapshot(service, gTemporaryRestoreSnapshot);
		captureLastNonVdoServiceSnapshot(service);
		return;
	}

	if (hasServiceSnapshot(gLastNonVdoServiceSnapshot)) {
		cloneServiceSnapshot(gLastNonVdoServiceSnapshot, gTemporaryRestoreSnapshot);
		return;
	}

	clearTemporaryServiceRestoreBackup();
}

bool restoreServiceFromTemporaryBackupIfNeeded()
{
	if (!hasServiceSnapshot(gTemporaryRestoreSnapshot)) {
		return false;
	}

	obs_service_t *restoredService = obs_service_create(gTemporaryRestoreSnapshot.serviceType.c_str(),
	                                                    kVdoNinjaServiceName, gTemporaryRestoreSnapshot.settings,
	                                                    gTemporaryRestoreSnapshot.hotkeys);
	if (!restoredService) {
		clearTemporaryServiceRestoreBackup();
		return false;
	}

	obs_frontend_set_streaming_service(restoredService);
	obs_frontend_save_streaming_service();
	captureLastNonVdoServiceSnapshot(restoredService);
	obs_service_release(restoredService);
	clearTemporaryServiceRestoreBackup();
	return true;
}

void ensureActiveVdoNinjaServiceConfigured(void)
{
	obs_service_t *currentService = obs_frontend_get_streaming_service();
	if (!currentService) {
		return;
	}

	if (isVdoNinjaService(currentService)) {
		configureProfileForVdoNinjaStreaming();
	}
}

void ensureStreamingServiceExists(void)
{
	obs_service_t *currentService = obs_frontend_get_streaming_service();
	if (currentService) {
		return;
	}

	logWarning("No active streaming service found; creating fallback custom RTMP service.");
	obs_data_t *fallbackSettings = obs_data_create();
	obs_data_set_string(fallbackSettings, "service", "Custom");
	obs_data_set_string(fallbackSettings, "server", "rtmp://localhost/live");
	obs_data_set_string(fallbackSettings, "key", "");

	obs_service_t *fallbackService = obs_service_create("rtmp_custom", "default_service", fallbackSettings, nullptr);
	obs_data_release(fallbackSettings);

	if (!fallbackService) {
		logWarning("Failed to create fallback streaming service; OBS Settings may be unstable.");
		return;
	}

	obs_frontend_set_streaming_service(fallbackService);
	obs_frontend_save_streaming_service();
	obs_service_release(fallbackService);
	logInfo("Created fallback custom RTMP service to keep OBS stream settings valid.");
}

} // namespace

// Plugin information
const char *obs_module_name(void)
{
	return "VDO.Ninja";
}

const char *obs_module_description(void)
{
	return "VDO.Ninja WebRTC streaming integration for OBS Studio";
}

// Virtual camera output (simplified - registers as a service)
static const char *vdoninja_service_getname(void *)
{
	return tr("VDONinjaService", "VDO.Ninja");
}

static void *vdoninja_service_create(obs_data_t *settings, obs_service_t *service)
{
	UNUSED_PARAMETER(service);

	// Service just holds settings, actual work is in output
	obs_data_t *data = obs_data_create();
	if (settings) {
		obs_data_apply(data, settings);
	}
	syncCompatibilityServiceFields(data);
	return data;
}

static void vdoninja_service_destroy(void *data)
{
	obs_data_t *settings = static_cast<obs_data_t *>(data);
	obs_data_release(settings);
}

static void vdoninja_service_update(void *data, obs_data_t *settings)
{
	obs_data_t *svc_settings = static_cast<obs_data_t *>(data);
	if (!svc_settings) {
		return;
	}
	if (settings) {
		obs_data_apply(svc_settings, settings);
	}
	syncCompatibilityServiceFields(svc_settings);
}

static obs_properties_t *vdoninja_service_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *serviceHint = obs_properties_add_text(
	    props, "service_hint",
	    tr("ServiceSetupHint",
	       "Tip: Use Tools -> VDO.Ninja Studio for full setup (stream ID, password, room, salt, signaling). "
	       "VDO.Ninja publishing uses OBS Start Streaming and cannot run in parallel with another stream destination. "
	       "Signaling Server and Salt are optional; leave blank for defaults."),
	    OBS_TEXT_INFO);
	obs_property_text_set_info_type(serviceHint, OBS_TEXT_INFO_NORMAL);
	obs_property_text_set_info_word_wrap(serviceHint, true);

	obs_properties_add_text(props, "stream_id", tr("StreamID", "Stream ID"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "room_id", tr("RoomID", "Room ID"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "password", tr("Password", "Password"), OBS_TEXT_PASSWORD);

	obs_property_t *codec = obs_properties_add_list(props, "video_codec", tr("VideoCodec", "Video Codec"),
	                                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(codec, "H.264", 0);

	obs_properties_add_int(props, "max_viewers", tr("MaxViewers", "Max Viewers"), 1, 50, 1);

	obs_properties_t *advanced = obs_properties_create();
	obs_property_t *wssHost =
	    obs_properties_add_text(advanced, "wss_host", tr("SignalingServer", "Signaling Server"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(
	    wssHost,
	    tr("SignalingServer.OptionalHelp",
	       "Optional. Leave blank to use default signaling server: wss://wss.vdo.ninja:443"));
	obs_property_t *salt = obs_properties_add_text(advanced, "salt", tr("Salt", "Salt"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(
	    salt, tr("Salt.OptionalHelp", "Optional. Leave blank to use default salt: vdo.ninja"));
	obs_property_t *iceServers = obs_properties_add_text(
	    advanced, "custom_ice_servers", tr("CustomICEServers", "Custom STUN/TURN Servers"), OBS_TEXT_DEFAULT);
	obs_property_text_set_monospace(iceServers, true);
	obs_property_set_long_description(
	    iceServers, tr("CustomICEServers.Help",
	                   "Format: one server entry per item. Use ';' to separate multiple entries. "
	                   "Examples: stun:stun.l.google.com:19302; turn:turn.example.com:3478|user|pass. "
	                   "Leave empty to use built-in STUN defaults (Google + Cloudflare); no TURN is added automatically."));
	obs_property_t *iceHelp = obs_properties_add_text(
	    advanced, "custom_ice_servers_help",
	    tr("CustomICEServers.Help",
	       "Format: one server entry per item. Use ';' to separate multiple entries. "
	       "Examples: stun:stun.l.google.com:19302; turn:turn.example.com:3478|user|pass. "
	       "Leave empty to use built-in STUN defaults (Google + Cloudflare); no TURN is added automatically."),
	    OBS_TEXT_INFO);
	obs_property_text_set_info_type(iceHelp, OBS_TEXT_INFO_NORMAL);
	obs_property_text_set_info_word_wrap(iceHelp, true);
	obs_properties_add_bool(advanced, "force_turn", tr("ForceTURN", "Force TURN Relay"));
	obs_properties_add_group(props, "advanced", tr("AdvancedSettings", "Advanced Settings"), OBS_GROUP_NORMAL,
	                         advanced);

	return props;
}

static void vdoninja_service_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "stream_id", "");
	obs_data_set_default_string(settings, "room_id", "");
	obs_data_set_default_string(settings, "password", "");
	obs_data_set_default_string(settings, "wss_host", "");
	obs_data_set_default_string(settings, "service", kVdoCatalogServiceName);
	obs_data_set_default_string(settings, "server", "");
	obs_data_set_default_string(settings, "protocol", "VDO.Ninja");
	obs_data_set_default_string(settings, "key", "");
	obs_data_set_default_string(settings, "salt", "");
	obs_data_set_default_string(settings, "custom_ice_servers", "");
	obs_data_set_default_string(
	    settings, "custom_ice_servers_help",
	    "Format: one server entry per item. Use ';' to separate multiple entries. "
	    "Examples: stun:stun.l.google.com:19302; turn:turn.example.com:3478|user|pass. "
	    "Leave empty to use built-in STUN defaults (Google + Cloudflare); no TURN is added automatically.");
	obs_data_set_default_int(settings, "video_codec", 0);
	obs_data_set_default_int(settings, "max_viewers", 10);
	obs_data_set_default_bool(settings, "force_turn", false);
}

static const char *vdoninja_service_url(void *data)
{
	UNUSED_PARAMETER(data);
	return "https://vdo.ninja";
}

static const char *vdoninja_service_key(void *data)
{
	obs_data_t *settings = static_cast<obs_data_t *>(data);
	if (!settings) {
		return "";
	}
	return obs_data_get_string(settings, "stream_id");
}

static const char *vdoninja_service_protocol(void *data)
{
	UNUSED_PARAMETER(data);
	return "VDO.Ninja";
}

static const char *vdoninja_service_connect_info(void *data, uint32_t type)
{
	obs_data_t *settings = static_cast<obs_data_t *>(data);
	if (!settings) {
		return nullptr;
	}
	switch ((enum obs_service_connect_info)type) {
	case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
		return obs_data_get_string(settings, "wss_host");
	case OBS_SERVICE_CONNECT_INFO_STREAM_ID:
		return obs_data_get_string(settings, "stream_id");
	case OBS_SERVICE_CONNECT_INFO_PASSWORD:
		return obs_data_get_string(settings, "password");
	default:
		return nullptr;
	}
}

static void vdoninja_service_apply_encoder_settings(void *, obs_data_t *video_settings, obs_data_t *audio_settings)
{
	UNUSED_PARAMETER(audio_settings);

	if (video_settings) {
		obs_data_set_int(video_settings, "bf", 0);
		obs_data_set_bool(video_settings, "repeat_headers", true);
	}
}

static const char *vdoninja_video_codecs[] = {"h264", nullptr};
static const char *vdoninja_audio_codecs[] = {"opus", nullptr};

static bool vdoninja_service_can_try_connect(void *data)
{
	obs_data_t *settings = static_cast<obs_data_t *>(data);
	if (!settings) {
		return false;
	}
	const char *stream_id = obs_data_get_string(settings, "stream_id");
	return stream_id && *stream_id;
}

void registerVdoNinjaService(void)
{
	obs_service_info info = {};
	info.id = kVdoNinjaServiceType;
	info.get_name = vdoninja_service_getname;
	info.create = vdoninja_service_create;
	info.destroy = vdoninja_service_destroy;
	info.update = vdoninja_service_update;
	info.get_defaults = vdoninja_service_defaults;
	info.get_properties = vdoninja_service_properties;
	info.get_url = vdoninja_service_url;
	info.get_key = vdoninja_service_key;
	info.apply_encoder_settings = vdoninja_service_apply_encoder_settings;
	info.get_output_type = [](void *) -> const char * {
		return "vdoninja_output";
	};
	info.get_supported_video_codecs = [](void *) -> const char ** {
		return vdoninja_video_codecs;
	};
	info.get_protocol = vdoninja_service_protocol;
	info.get_supported_audio_codecs = [](void *) -> const char ** {
		return vdoninja_audio_codecs;
	};
	info.get_connect_info = vdoninja_service_connect_info;
	info.can_try_to_connect = vdoninja_service_can_try_connect;

	obs_register_service(&info);
}

static std::string buildPushUrlFromSettings(obs_data_t *settings)
{
	if (!settings) {
		return "";
	}

	const std::string streamId = obs_data_get_string(settings, "stream_id");
	if (streamId.empty()) {
		return "";
	}

	const std::string password = obs_data_get_string(settings, "password");
	const std::string roomId = obs_data_get_string(settings, "room_id");
	const std::string salt = obs_data_get_string(settings, "salt");
	const std::string wssHost = obs_data_get_string(settings, "wss_host");

	std::string pushUrl = "https://vdo.ninja/?push=" + urlEncode(streamId);
	if (!password.empty()) {
		pushUrl += "&password=" + urlEncode(password);
	}
	if (!roomId.empty()) {
		pushUrl += "&room=" + urlEncode(roomId);
	}
	if (!salt.empty() && salt != DEFAULT_SALT) {
		pushUrl += "&salt=" + urlEncode(salt);
	}
	if (!wssHost.empty() && wssHost != DEFAULT_WSS_HOST) {
		pushUrl += "&wss=" + urlEncode(wssHost);
	}

	return pushUrl;
}

static std::string buildViewUrlFromSettings(obs_data_t *settings)
{
	if (!settings) {
		return "";
	}

	const std::string streamId = obs_data_get_string(settings, "stream_id");
	if (streamId.empty()) {
		return "";
	}

	const std::string password = obs_data_get_string(settings, "password");
	const std::string roomId = obs_data_get_string(settings, "room_id");
	const std::string salt = obs_data_get_string(settings, "salt");
	const std::string wssHost = obs_data_get_string(settings, "wss_host");

	std::string viewUrl = "https://vdo.ninja/?view=" + urlEncode(streamId);
	if (!password.empty()) {
		viewUrl += "&password=" + urlEncode(password);
	}
	if (!roomId.empty()) {
		viewUrl += "&room=" + urlEncode(roomId);
	}
	if (!salt.empty() && salt != DEFAULT_SALT) {
		viewUrl += "&salt=" + urlEncode(salt);
	}
	if (!wssHost.empty() && wssHost != DEFAULT_WSS_HOST) {
		viewUrl += "&wss=" + urlEncode(wssHost);
	}

	return viewUrl;
}

static std::string formatBytesHuman(uint64_t bytes)
{
	static const char *kUnits[] = {"B", "KB", "MB", "GB", "TB"};
	double value = static_cast<double>(bytes);
	size_t unit = 0;
	while (value >= 1024.0 && unit < (sizeof(kUnits) / sizeof(kUnits[0])) - 1) {
		value /= 1024.0;
		++unit;
	}

	std::ostringstream oss;
	oss.setf(std::ios::fixed);
	oss.precision(unit == 0 ? 0 : 2);
	oss << value << " " << kUnits[unit];
	return oss.str();
}

static bool copyTextToClipboard(const std::string &text)
{
#ifdef _WIN32
	if (!OpenClipboard(nullptr)) {
		return false;
	}

	EmptyClipboard();

	const int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
	if (wideLength <= 0) {
		CloseClipboard();
		return false;
	}

	const SIZE_T bytes = static_cast<SIZE_T>(wideLength) * sizeof(wchar_t);
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (!memory) {
		CloseClipboard();
		return false;
	}

	void *locked = GlobalLock(memory);
	if (!locked) {
		GlobalFree(memory);
		CloseClipboard();
		return false;
	}

	const int converted = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, static_cast<wchar_t *>(locked), wideLength);
	GlobalUnlock(memory);
	if (converted <= 0) {
		GlobalFree(memory);
		CloseClipboard();
		return false;
	}

	if (!SetClipboardData(CF_UNICODETEXT, memory)) {
		GlobalFree(memory);
		CloseClipboard();
		return false;
	}

	CloseClipboard();
	return true;
#else
	UNUSED_PARAMETER(text);
	return false;
#endif
}

bool activateVdoNinjaServiceFromSettings(obs_data_t *sourceSettings, bool generateStreamIdIfMissing,
                                                bool temporarySwitch)
{
	if (!sourceSettings) {
		return false;
	}

	obs_service_t *currentService = obs_frontend_get_streaming_service();
	if (temporarySwitch) {
		backupServiceForTemporaryRestore(currentService);
	} else {
		captureLastNonVdoServiceSnapshot(currentService);
		clearTemporaryServiceRestoreBackup();
	}

	obs_data_t *serviceSettings = obs_data_create();
	vdoninja_service_defaults(serviceSettings);
	obs_data_apply(serviceSettings, sourceSettings);

	const char *streamId = obs_data_get_string(serviceSettings, "stream_id");
	if ((!streamId || !*streamId) && generateStreamIdIfMissing) {
		const std::string generatedStreamId = generateSessionId();
		obs_data_set_string(serviceSettings, "stream_id", generatedStreamId.c_str());
		obs_data_set_string(sourceSettings, "stream_id", generatedStreamId.c_str());
	}

	syncCompatibilityServiceFields(serviceSettings);

	obs_service_t *newService = obs_service_create(kVdoNinjaServiceType, kVdoNinjaServiceName, serviceSettings, nullptr);
	obs_data_release(serviceSettings);
	if (!newService) {
		return false;
	}

	obs_frontend_set_streaming_service(newService);
	obs_frontend_save_streaming_service();
	configureProfileForVdoNinjaStreaming();
	obs_service_release(newService);
	return true;
}

namespace
{
static void updateControlCenterStatus(obs_data_t *settings, ControlCenterContext *ctx, const char *prefix = nullptr)
{
	if (!settings) {
		return;
	}

	obs_data_set_string(settings, "cc_push_url", buildPushUrlFromSettings(settings).c_str());
	obs_data_set_string(settings, "cc_view_url", buildViewUrlFromSettings(settings).c_str());

	std::ostringstream status;
	if (prefix && *prefix) {
		status << prefix << "\n";
	}

	const bool streamingActive = obs_frontend_streaming_active();
	status << "Streaming: " << (streamingActive ? "active" : "stopped") << "\n";

	std::ostringstream peers;
	peers << "Peer telemetry:\n";

	obs_output_t *output = obs_frontend_get_streaming_output();
	if (!output) {
		status << "Output: unavailable\n";
		peers << "(no active streaming output)";
		obs_data_set_string(settings, "cc_status", status.str().c_str());
		obs_data_set_string(settings, "cc_peer_stats", peers.str().c_str());
		return;
	}

	const char *outputId = obs_output_get_id(output);
	status << "Output ID: " << (outputId ? outputId : "(unknown)") << "\n";

	const uint64_t totalBytes = obs_output_get_total_bytes(output);
	const int connectTimeMs = obs_output_get_connect_time_ms(output);
	const int droppedFrames = obs_output_get_frames_dropped(output);
	const int totalFrames = obs_output_get_total_frames(output);
	const float congestion = obs_output_get_congestion(output);
	const char *lastError = obs_output_get_last_error(output);

	double bitrateKbps = 0.0;
	if (ctx) {
		const int64_t nowMs = currentTimeMs();
		if (ctx->previousSampleTimeMs > 0 && nowMs > ctx->previousSampleTimeMs && totalBytes >= ctx->previousTotalBytes) {
			const double elapsedSec = static_cast<double>(nowMs - ctx->previousSampleTimeMs) / 1000.0;
			const double deltaBytes = static_cast<double>(totalBytes - ctx->previousTotalBytes);
			if (elapsedSec > 0.0) {
				bitrateKbps = (deltaBytes * 8.0) / elapsedSec / 1000.0;
			}
		}
		ctx->previousTotalBytes = totalBytes;
		ctx->previousSampleTimeMs = nowMs;
	}

	status << "Sent: " << formatBytesHuman(totalBytes) << "\n";
	status << "Connect time: " << connectTimeMs << " ms\n";
	status << "Instant bitrate: " << static_cast<int>(bitrateKbps) << " kbps\n";
	status << "Dropped/total frames: " << droppedFrames << "/" << totalFrames << "\n";
	status << "Congestion: " << congestion << "\n";
	if (lastError && *lastError) {
		status << "Last error: " << lastError << "\n";
	}

	auto *typedOutput = (outputId && strcmp(outputId, "vdoninja_output") == 0)
	                        ? static_cast<VDONinjaOutput *>(obs_obj_get_data(output))
	                        : nullptr;
	if (typedOutput) {
		status << "Connected to signaling: " << (typedOutput->isConnected() ? "yes" : "no") << "\n";
		status << "Viewer count: " << typedOutput->getViewerCount() << "\n";

		const int64_t uptimeMs = typedOutput->getUptimeMs();
		status << "Uptime: " << (uptimeMs / 1000) << " s\n";

		const auto snapshots = typedOutput->getViewerSnapshots();
		if (snapshots.empty()) {
			peers << "(no peers)";
		} else {
			for (const auto &snapshot : snapshots) {
				peers << "- " << snapshot.uuid << " | role=" << snapshot.role << " | state=" << snapshot.state
				      << " | dc=" << (snapshot.hasDataChannel ? "yes" : "no");
				if (!snapshot.lastStats.empty()) {
					peers << " | stats=" << snapshot.lastStats;
				}
				if (snapshot.lastStatsTimestampMs > 0) {
					peers << " | stats_t=" << snapshot.lastStatsTimestampMs;
				}
				peers << "\n";
			}
		}
	} else {
		peers << "(stream output is not a VDO.Ninja output)";
	}

	obs_data_set_string(settings, "cc_status", status.str().c_str());
	obs_data_set_string(settings, "cc_peer_stats", peers.str().c_str());
}

static bool controlCenterFieldModified(void *priv, obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	auto *ctx = static_cast<ControlCenterContext *>(priv);
	updateControlCenterStatus(settings, ctx);
	return true;
}

static bool controlCenterLoadActiveClicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ControlCenterContext *>(data);
	if (!ctx || !ctx->source) {
		return false;
	}

	obs_data_t *settings = obs_source_get_settings(ctx->source);
	if (!settings) {
		return false;
	}

	obs_service_t *currentService = obs_frontend_get_streaming_service();
	seedVdoNinjaSettingsFromCurrentService(currentService, settings);
	updateControlCenterStatus(settings, ctx, "Loaded settings from active stream service.");

	obs_source_update(ctx->source, settings);
	obs_data_release(settings);
	return true;
}

static bool controlCenterApplyClicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ControlCenterContext *>(data);
	if (!ctx || !ctx->source) {
		return false;
	}

	obs_data_t *settings = obs_source_get_settings(ctx->source);
	if (!settings) {
		return false;
	}

	if (obs_frontend_streaming_active()) {
		updateControlCenterStatus(settings, ctx, "Cannot apply service settings while streaming is active.");
		obs_source_update(ctx->source, settings);
		obs_data_release(settings);
		return true;
	}

	const bool ok = activateVdoNinjaServiceFromSettings(settings, true, false);
	updateControlCenterStatus(settings, ctx, ok ? "VDO.Ninja stream service configured."
	                                            : "Failed to configure VDO.Ninja stream service.");
	obs_source_update(ctx->source, settings);
	obs_data_release(settings);
	return true;
}

static bool controlCenterStartClicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ControlCenterContext *>(data);
	if (!ctx || !ctx->source) {
		return false;
	}

	obs_data_t *settings = obs_source_get_settings(ctx->source);
	if (!settings) {
		return false;
	}

	if (!obs_frontend_streaming_active()) {
		const bool applied = activateVdoNinjaServiceFromSettings(settings, true, true);
		if (!applied) {
			updateControlCenterStatus(settings, ctx, "Unable to activate VDO.Ninja service; start aborted.");
			obs_source_update(ctx->source, settings);
			obs_data_release(settings);
			return true;
		}
		obs_frontend_streaming_start();
		updateControlCenterStatus(settings, ctx, "Requested streaming start.");
	} else {
		updateControlCenterStatus(settings, ctx, "Streaming already active.");
	}

	obs_source_update(ctx->source, settings);
	obs_data_release(settings);
	return true;
}

static bool controlCenterStopClicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ControlCenterContext *>(data);
	if (!ctx || !ctx->source) {
		return false;
	}

	obs_data_t *settings = obs_source_get_settings(ctx->source);
	if (!settings) {
		return false;
	}

	if (obs_frontend_streaming_active()) {
		obs_frontend_streaming_stop();
		updateControlCenterStatus(settings, ctx, "Requested streaming stop.");
	} else {
		updateControlCenterStatus(settings, ctx, "Streaming is already stopped.");
	}

	obs_source_update(ctx->source, settings);
	obs_data_release(settings);
	return true;
}

static bool controlCenterRefreshClicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ControlCenterContext *>(data);
	if (!ctx || !ctx->source) {
		return false;
	}

	obs_data_t *settings = obs_source_get_settings(ctx->source);
	if (!settings) {
		return false;
	}

	updateControlCenterStatus(settings, ctx, "Status refreshed.");
	obs_source_update(ctx->source, settings);
	obs_data_release(settings);
	return true;
}

static bool controlCenterCopyPushUrlClicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ControlCenterContext *>(data);
	if (!ctx || !ctx->source) {
		return false;
	}

	obs_data_t *settings = obs_source_get_settings(ctx->source);
	if (!settings) {
		return false;
	}

	const std::string pushUrl = buildPushUrlFromSettings(settings);
	if (pushUrl.empty()) {
		updateControlCenterStatus(settings, ctx, "No publish URL available yet. Set Stream ID first.");
	} else if (copyTextToClipboard(pushUrl)) {
		updateControlCenterStatus(settings, ctx, "Copied publish URL to clipboard.");
	} else {
		updateControlCenterStatus(settings, ctx, "Unable to copy publish URL to clipboard on this platform.");
	}

	obs_source_update(ctx->source, settings);
	obs_data_release(settings);
	return true;
}

static bool controlCenterCopyViewUrlClicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ControlCenterContext *>(data);
	if (!ctx || !ctx->source) {
		return false;
	}

	obs_data_t *settings = obs_source_get_settings(ctx->source);
	if (!settings) {
		return false;
	}

	const std::string viewUrl = buildViewUrlFromSettings(settings);
	if (viewUrl.empty()) {
		updateControlCenterStatus(settings, ctx, "No viewer URL available yet. Set Stream ID first.");
	} else if (copyTextToClipboard(viewUrl)) {
		updateControlCenterStatus(settings, ctx, "Copied viewer URL to clipboard.");
	} else {
		updateControlCenterStatus(settings, ctx, "Unable to copy viewer URL to clipboard on this platform.");
	}

	obs_source_update(ctx->source, settings);
	obs_data_release(settings);
	return true;
}

static const char *vdoninja_control_center_getname(void *)
{
	return tr("VDONinjaControlCenter", "VDO.Ninja Control Center");
}

static void *vdoninja_control_center_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new ControlCenterContext();
	ctx->source = source;

	const char *streamId = obs_data_get_string(settings, "stream_id");
	if (!streamId || !*streamId) {
		obs_service_t *currentService = obs_frontend_get_streaming_service();
		seedVdoNinjaSettingsFromCurrentService(currentService, settings);
	}

	updateControlCenterStatus(settings, ctx, "Control Center ready.");
	return ctx;
}

static void vdoninja_control_center_destroy(void *data)
{
	auto *ctx = static_cast<ControlCenterContext *>(data);
	delete ctx;
}

static void vdoninja_control_center_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<ControlCenterContext *>(data);
	updateControlCenterStatus(settings, ctx);
}

static uint32_t vdoninja_control_center_width(void *)
{
	return 0;
}

static uint32_t vdoninja_control_center_height(void *)
{
	return 0;
}

static obs_properties_t *vdoninja_control_center_properties(void *data)
{
	auto *ctx = static_cast<ControlCenterContext *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *intro = obs_properties_add_text(
	    props, "cc_intro", tr("ControlCenter.Intro", "Control Center"),
	    OBS_TEXT_INFO);
	obs_property_text_set_info_type(intro, OBS_TEXT_INFO_NORMAL);
	obs_property_text_set_info_word_wrap(intro, true);

	obs_property_t *streamId =
	    obs_properties_add_text(props, "stream_id", tr("StreamID", "Stream ID"), OBS_TEXT_DEFAULT);
	obs_property_t *roomId = obs_properties_add_text(props, "room_id", tr("RoomID", "Room ID"), OBS_TEXT_DEFAULT);
	obs_property_t *password =
	    obs_properties_add_text(props, "password", tr("Password", "Password"), OBS_TEXT_PASSWORD);
	obs_property_t *maxViewers =
	    obs_properties_add_int(props, "max_viewers", tr("MaxViewers", "Max Viewers"), 1, 50, 1);
	obs_property_t *wssHost = nullptr;
	obs_property_t *salt = nullptr;
	obs_property_t *forceTurn = nullptr;

	obs_property_t *modeNote = obs_properties_add_text(
	    props, "cc_mode_note",
	    tr("ControlCenter.ModeNote",
	       "Publishing uses OBS Start Streaming pipeline. Control Center Start/Stop are shortcuts for OBS Start/Stop Streaming "
	       "and cannot run in parallel with another stream destination. Ingest is separate and not auto-created from external push links."),
	    OBS_TEXT_INFO);
	obs_property_text_set_info_type(modeNote, OBS_TEXT_INFO_NORMAL);
	obs_property_text_set_info_word_wrap(modeNote, true);

	obs_properties_add_button2(props, "cc_load_active",
	                           tr("ControlCenter.LoadActive", "Load Active Service Settings"),
	                           controlCenterLoadActiveClicked, ctx);
	obs_properties_add_button2(props, "cc_apply",
	                           tr("ControlCenter.ApplyService", "Apply As Stream Service"),
	                           controlCenterApplyClicked, ctx);
	obs_properties_add_button2(props, "cc_start_publish",
	                           tr("ControlCenter.StartPublish", "Start Publishing"),
	                           controlCenterStartClicked, ctx);
	obs_properties_add_button2(props, "cc_stop_publish",
	                           tr("ControlCenter.StopPublish", "Stop Publishing"),
	                           controlCenterStopClicked, ctx);
	obs_properties_add_button2(props, "cc_refresh",
	                           tr("ControlCenter.Refresh", "Refresh Runtime Stats"),
	                           controlCenterRefreshClicked, ctx);

	obs_property_t *pushUrl =
	    obs_properties_add_text(props, "cc_push_url", tr("ControlCenter.PushURL", "Publish URL"), OBS_TEXT_INFO);
	obs_properties_add_button2(props, "cc_copy_push_url",
	                           tr("ControlCenter.CopyPushURL", "Copy Publish URL"),
	                           controlCenterCopyPushUrlClicked, ctx);
	obs_property_t *viewUrl =
	    obs_properties_add_text(props, "cc_view_url", tr("ControlCenter.ViewURL", "Viewer URL"), OBS_TEXT_INFO);
	obs_properties_add_button2(props, "cc_copy_view_url",
	                           tr("ControlCenter.CopyViewURL", "Copy Viewer URL"),
	                           controlCenterCopyViewUrlClicked, ctx);
	obs_property_t *status =
	    obs_properties_add_text(props, "cc_status", tr("ControlCenter.Status", "Runtime Status"), OBS_TEXT_INFO);
	obs_property_t *peerStats = obs_properties_add_text(
	    props, "cc_peer_stats", tr("ControlCenter.Peers", "Viewer/Peer Stats"), OBS_TEXT_INFO);

	obs_property_text_set_info_word_wrap(pushUrl, true);
	obs_property_text_set_info_word_wrap(viewUrl, true);
	obs_property_text_set_info_word_wrap(status, true);
	obs_property_text_set_info_word_wrap(peerStats, true);

	obs_properties_t *advanced = obs_properties_create();
	wssHost = obs_properties_add_text(advanced, "wss_host", tr("SignalingServer", "Signaling Server"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(
	    wssHost,
	    tr("SignalingServer.OptionalHelp",
	       "Optional. Leave blank to use default signaling server: wss://wss.vdo.ninja:443"));
	salt = obs_properties_add_text(advanced, "salt", tr("Salt", "Salt"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(
	    salt, tr("Salt.OptionalHelp", "Optional. Leave blank to use default salt: vdo.ninja"));
	obs_property_t *iceServers = obs_properties_add_text(
	    advanced, "custom_ice_servers", tr("CustomICEServers", "Custom STUN/TURN Servers"), OBS_TEXT_DEFAULT);
	obs_property_text_set_monospace(iceServers, true);
	obs_property_set_long_description(
	    iceServers, tr("CustomICEServers.Help",
	                   "Format: one server entry per item. Use ';' to separate multiple entries. "
	                   "Examples: stun:stun.l.google.com:19302; turn:turn.example.com:3478|user|pass. "
	                   "Leave empty to use built-in STUN defaults (Google + Cloudflare); no TURN is added automatically."));
	obs_property_t *iceHelp = obs_properties_add_text(
	    advanced, "custom_ice_servers_help",
	    tr("CustomICEServers.Help",
	       "Format: one server entry per item. Use ';' to separate multiple entries. "
	       "Examples: stun:stun.l.google.com:19302; turn:turn.example.com:3478|user|pass. "
	       "Leave empty to use built-in STUN defaults (Google + Cloudflare); no TURN is added automatically."),
	    OBS_TEXT_INFO);
	obs_property_text_set_info_type(iceHelp, OBS_TEXT_INFO_NORMAL);
	obs_property_text_set_info_word_wrap(iceHelp, true);
	forceTurn = obs_properties_add_bool(advanced, "force_turn", tr("ForceTURN", "Force TURN Relay"));
	obs_properties_add_group(props, "advanced", tr("AdvancedSettings", "Advanced Settings"), OBS_GROUP_NORMAL,
	                         advanced);

	obs_property_set_modified_callback2(streamId, controlCenterFieldModified, ctx);
	obs_property_set_modified_callback2(roomId, controlCenterFieldModified, ctx);
	obs_property_set_modified_callback2(password, controlCenterFieldModified, ctx);
	obs_property_set_modified_callback2(maxViewers, controlCenterFieldModified, ctx);
	obs_property_set_modified_callback2(wssHost, controlCenterFieldModified, ctx);
	obs_property_set_modified_callback2(salt, controlCenterFieldModified, ctx);
	obs_property_set_modified_callback2(forceTurn, controlCenterFieldModified, ctx);

	return props;
}

static void vdoninja_control_center_defaults(obs_data_t *settings)
{
	const std::string defaultStreamId = generateSessionId();
	obs_data_set_default_string(
	    settings, "cc_intro",
	    "Publish-first control center: configure and start VDO.Ninja publishing from OBS. External push links are not auto-ingested.");
	obs_data_set_default_string(
	    settings, "cc_mode_note",
	    "Publishing uses OBS Start Streaming pipeline. Control Center Start/Stop are shortcuts for OBS Start/Stop Streaming "
	    "and cannot run in parallel with another stream destination. Ingest is separate and not auto-created from external push links.");
	obs_data_set_default_string(settings, "stream_id", defaultStreamId.c_str());
	obs_data_set_default_string(settings, "room_id", "");
	obs_data_set_default_string(settings, "password", "");
	obs_data_set_default_string(settings, "wss_host", "");
	obs_data_set_default_string(settings, "salt", "");
	obs_data_set_default_string(settings, "custom_ice_servers", "");
	obs_data_set_default_string(
	    settings, "custom_ice_servers_help",
	    "Format: one server entry per item. Use ';' to separate multiple entries. "
	    "Examples: stun:stun.l.google.com:19302; turn:turn.example.com:3478|user|pass. "
	    "Leave empty to use built-in STUN defaults (Google + Cloudflare); no TURN is added automatically.");
	obs_data_set_default_int(settings, "max_viewers", 10);
	obs_data_set_default_bool(settings, "force_turn", false);
	obs_data_set_default_string(settings, "cc_push_url", "");
	obs_data_set_default_string(settings, "cc_view_url", "");
	obs_data_set_default_string(settings, "cc_status", "Press 'Refresh Runtime Stats' to sample live metrics.");
	obs_data_set_default_string(settings, "cc_peer_stats", "");
}

static obs_source_info vdoninja_control_center_source_info = {
    .id = "vdoninja_control_center",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = vdoninja_control_center_getname,
    .create = vdoninja_control_center_create,
    .destroy = vdoninja_control_center_destroy,
    .get_width = vdoninja_control_center_width,
    .get_height = vdoninja_control_center_height,
    .get_defaults = vdoninja_control_center_defaults,
    .get_properties = vdoninja_control_center_properties,
    .update = vdoninja_control_center_update,
};

static obs_source_t *getOrCreateControlCenterSource()
{
	if (gControlCenterSource) {
		return gControlCenterSource;
	}

	obs_data_t *settings = obs_data_create();
	vdoninja_control_center_defaults(settings);
	obs_service_t *currentService = obs_frontend_get_streaming_service();
	seedVdoNinjaSettingsFromCurrentService(currentService, settings);
	updateControlCenterStatus(settings, nullptr, "Control Center ready.");

	gControlCenterSource = obs_source_create_private(kVdoNinjaControlCenterSourceId, kVdoNinjaControlCenterSourceName,
	                                                 settings);
	obs_data_release(settings);
	return gControlCenterSource;
}

static void open_vdoninja_studio_callback(void *)
{
	if (g_vdo_dock) {
		g_vdo_dock->setVisible(!g_vdo_dock->isVisible());
	}
}

// Frontend event callback
static void frontend_event_callback(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		logInfo("Ensured VDO.Ninja streaming profile settings before streaming start");
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
		logInfo("Virtual camera started");
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		logInfo("Virtual camera stopped");
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		logInfo("Streaming started");
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		logInfo("Streaming stopped");
		{
			obs_service_t *currentService = obs_frontend_get_streaming_service();
			if (isVdoNinjaService(currentService) && restoreServiceFromTemporaryBackupIfNeeded()) {
				logInfo("Restored previous streaming service after temporary VDO.Ninja publish run");
			} else {
				captureLastNonVdoServiceSnapshot(currentService);
			}
		}
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		captureLastNonVdoServiceSnapshot(obs_frontend_get_streaming_service());
		ensureStreamingServiceExists();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		ensureRtmpCatalogHasVdoNinjaEntry();
		ensureStreamingServiceExists();
		captureLastNonVdoServiceSnapshot(obs_frontend_get_streaming_service());
		break;
	default:
		break;
	}
}

} // namespace

void vdo_dock_show_chat(const char *sender, const char *message)
{
	if (g_vdo_dock && sender && message) {
		g_vdo_dock->onChatReceived(QString::fromUtf8(sender), QString::fromUtf8(message));
	}
}

void vdo_handle_remote_control(const char *action, const char *value)
{
	if (!action || !*action) {
		return;
	}

	const std::string act(action);
	const std::string val(value ? value : "");

	if (act == "nextScene" || act == "prevScene") {
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		if (scenes.sources.num == 0) {
			obs_frontend_source_list_free(&scenes);
			return;
		}
		obs_source_t *current = obs_frontend_get_current_scene();
		int currentIdx = -1;
		for (size_t i = 0; i < scenes.sources.num; i++) {
			if (scenes.sources.array[i] == current) {
				currentIdx = static_cast<int>(i);
				break;
			}
		}
		obs_source_release(current);
		int newIdx = 0;
		if (currentIdx >= 0) {
			int count = static_cast<int>(scenes.sources.num);
			newIdx = act == "nextScene"
			         ? (currentIdx + 1) % count
			         : (currentIdx - 1 + count) % count;
		}
		obs_frontend_set_current_scene(scenes.sources.array[newIdx]);
		obs_frontend_source_list_free(&scenes);
	} else if ((act == "setScene" || act == "setCurrentScene") && !val.empty()) {
		obs_source_t *scene = obs_get_source_by_name(val.c_str());
		if (scene) {
			obs_frontend_set_current_scene(scene);
			obs_source_release(scene);
		}
	} else if (act == "startStreaming") {
		obs_frontend_streaming_start();
	} else if (act == "stopStreaming") {
		obs_frontend_streaming_stop();
	} else if (act == "startRecording") {
		obs_frontend_recording_start();
	} else if (act == "stopRecording") {
		obs_frontend_recording_stop();
	} else if (act == "startVirtualcam") {
		obs_frontend_start_virtualcam();
	} else if (act == "stopVirtualcam") {
		obs_frontend_stop_virtualcam();
	} else if (act == "mute" || act == "unmute") {
		obs_source_t *desktopAudio = obs_get_output_source(1);
		if (desktopAudio) {
			obs_source_set_muted(desktopAudio, act == "mute");
			obs_source_release(desktopAudio);
		}
	} else {
		logInfo("Unknown remote control action: %s", action);
	}
}

// Module load
bool obs_module_load(void)
{
	logInfo("Loading VDO.Ninja plugin v%s", PLUGIN_VERSION);

	// Register output
	obs_register_output(&vdoninja_output_info);
	logInfo("Registered VDO.Ninja output");

	// Register source
	obs_register_source(&vdoninja_source_info);
	logInfo("Registered VDO.Ninja source");

	// Register control center source (legacy UI host)
	obs_register_source(&vdoninja_control_center_source_info);
	logInfo("Registered VDO.Ninja Control Center source");

	// Register service
	registerVdoNinjaService();
	logInfo("Registered VDO.Ninja service");

	// Create and register Studio Dock
	g_vdo_dock = new VDONinjaDock();
	obs_frontend_add_custom_qdock("VDONinjaStudioDock", g_vdo_dock);
	logInfo("Registered VDO.Ninja Studio Dock");

	obs_frontend_add_tools_menu_item(tr("Tools.OpenStudio", "VDO.Ninja Studio"),
	                                 open_vdoninja_studio_callback, nullptr);
	logInfo("Registered VDO.Ninja Studio tools menu action");

	// Register frontend callback
	obs_frontend_add_event_callback(frontend_event_callback, nullptr);

	logInfo("VDO.Ninja plugin loaded successfully");
	return true;
}

// Module unload
void obs_module_unload(void)
{
	logInfo("Unloading VDO.Ninja plugin");

	obs_frontend_remove_event_callback(frontend_event_callback, nullptr);

	if (gControlCenterSource) {
		obs_source_release(gControlCenterSource);
		gControlCenterSource = nullptr;
	}
	
	// Dock is managed by OBS frontend if registered via add_dock, 
	// but we null out our pointer for safety.
	g_vdo_dock = nullptr;

	releaseServiceSnapshot(gTemporaryRestoreSnapshot);
	releaseServiceSnapshot(gLastNonVdoServiceSnapshot);

	logInfo("VDO.Ninja plugin unloaded");
}
