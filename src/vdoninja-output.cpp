/*
 * OBS VDO.Ninja Plugin
 * Output module implementation
 */

#include "vdoninja-output.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <obs-frontend-api.h>
#include <util/dstr.h>
#include <util/threading.h>

#include "plugin-main.h"
#include "vdoninja-utils.h"

namespace vdoninja
{

namespace
{

const char *tr(const char *key, const char *fallback)
{
	const char *localized = obs_module_text(key);
	if (!localized || !*localized || std::strcmp(localized, key) == 0) {
		return fallback;
	}
	return localized;
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

bool containsInsensitive(const std::string &value, const char *needle)
{
	if (!needle || !*needle) {
		return false;
	}

	std::string lowerValue = value;
	std::string lowerNeedle = needle;
	std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lowerValue.find(lowerNeedle) != std::string::npos;
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

void parseVdoKeyValue(const std::string &keyValue, std::string &streamId, std::string &password, std::string &roomId,
                      std::string &salt, std::string &wssHost)
{
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

		if (streamId.empty()) {
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
}

std::string codecToUrlValue(VideoCodec codec)
{
	switch (codec) {
	case VideoCodec::VP8:
		return "vp8";
	case VideoCodec::VP9:
		return "vp9";
	case VideoCodec::AV1:
		return "av1";
	case VideoCodec::H264:
	default:
		return "h264";
	}
}

const char *connectionStateToString(ConnectionState state)
{
	switch (state) {
	case ConnectionState::New:
		return "new";
	case ConnectionState::Connecting:
		return "connecting";
	case ConnectionState::Connected:
		return "connected";
	case ConnectionState::Disconnected:
		return "disconnected";
	case ConnectionState::Failed:
		return "failed";
	case ConnectionState::Closed:
	default:
		return "closed";
	}
}

const char *connectionTypeToString(ConnectionType type)
{
	switch (type) {
	case ConnectionType::Viewer:
		return "viewer";
	case ConnectionType::Publisher:
	default:
		return "publisher";
	}
}

constexpr const char *kPluginInfoVersion = "1.1.0";
constexpr size_t kMaxAudioMixes = 6;

bool validateOpusAudioEncoders(obs_output_t *output, std::string &nonOpusCodec)
{
	if (!output) {
		return true;
	}

	for (size_t i = 0; i < kMaxAudioMixes; ++i) {
		obs_encoder_t *audioEncoder = obs_output_get_audio_encoder(output, i);
		if (!audioEncoder) {
			continue;
		}

		const char *codec = obs_encoder_get_codec(audioEncoder);
		if (!codec || std::strcmp(codec, "opus") != 0) {
			nonOpusCodec = codec ? codec : "(unknown)";
			return false;
		}
	}

	return true;
}

} // namespace

// OBS output callbacks
static const char *vdoninja_output_getname(void *)
{
	return tr("VDONinjaOutput", "VDO.Ninja Output");
}

static void *vdoninja_output_create(obs_data_t *settings, obs_output_t *output)
{
	try {
		auto *vdo = new VDONinjaOutput(settings, output);
		return vdo;
	} catch (const std::exception &e) {
		logError("Failed to create VDO.Ninja output: %s", e.what());
		return nullptr;
	}
}

static void vdoninja_output_destroy(void *data)
{
	auto *vdo = static_cast<VDONinjaOutput *>(data);
	delete vdo;
}

static bool vdoninja_output_start(void *data)
{
	auto *vdo = static_cast<VDONinjaOutput *>(data);
	return vdo->start();
}

static void vdoninja_output_stop(void *data, uint64_t)
{
	auto *vdo = static_cast<VDONinjaOutput *>(data);
	vdo->stop();
}

static void vdoninja_output_data(void *data, encoder_packet *packet)
{
	auto *vdo = static_cast<VDONinjaOutput *>(data);
	vdo->data(packet);
}

static void vdoninja_output_update(void *data, obs_data_t *settings)
{
	auto *vdo = static_cast<VDONinjaOutput *>(data);
	vdo->update(settings);
}

static obs_properties_t *vdoninja_output_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "stream_id", tr("StreamID", "Stream ID"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "room_id", tr("RoomID", "Room ID"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "password", tr("Password", "Password"), OBS_TEXT_PASSWORD);

	obs_property_t *codec = obs_properties_add_list(props, "video_codec", tr("VideoCodec", "Video Codec"),
	                                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(codec, "H.264", static_cast<int>(VideoCodec::H264));

	obs_properties_add_int(props, "bitrate", tr("Bitrate", "Bitrate (kbps)"), 500, 50000, 100);
	obs_properties_add_int(props, "max_viewers", tr("MaxViewers", "Max Viewers"), 1, 50, 1);
	obs_properties_add_bool(props, "enable_data_channel", tr("EnableDataChannel", "Enable Data Channel"));
	obs_properties_add_bool(props, "auto_reconnect", tr("AutoReconnect", "Auto Reconnect"));

	obs_properties_add_bool(props, "auto_inbound_enabled", tr("AutoInbound.Enabled", "Auto Manage Inbound Streams"));
	obs_properties_add_text(props, "auto_inbound_room_id", tr("AutoInbound.RoomID", "Inbound Room ID"),
	                        OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "auto_inbound_password", tr("AutoInbound.Password", "Inbound Room Password"),
	                        OBS_TEXT_PASSWORD);
	obs_properties_add_text(props, "auto_inbound_target_scene",
	                        tr("AutoInbound.TargetScene", "Target Scene (optional)"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "auto_inbound_source_prefix", tr("AutoInbound.SourcePrefix", "Source Prefix"),
	                        OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "auto_inbound_base_url", tr("AutoInbound.BaseUrl", "Base Playback URL"),
	                        OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, "auto_inbound_remove_on_disconnect",
	                        tr("AutoInbound.RemoveOnDisconnect", "Remove Source On Disconnect"));
	obs_properties_add_bool(props, "auto_inbound_switch_scene",
	                        tr("AutoInbound.SwitchScene", "Switch To Scene On New Stream"));
	obs_properties_add_int(props, "auto_inbound_width", tr("AutoInbound.Width", "Inbound Source Width"), 320, 4096, 1);
	obs_properties_add_int(props, "auto_inbound_height", tr("AutoInbound.Height", "Inbound Source Height"), 240, 2160,
	                       1);

	obs_property_t *layoutMode =
	    obs_properties_add_list(props, "auto_inbound_layout_mode", tr("AutoInbound.LayoutMode", "Inbound Layout"),
	                            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(layoutMode, tr("AutoInbound.Layout.None", "None"),
	                          static_cast<int>(AutoLayoutMode::None));
	obs_property_list_add_int(layoutMode, tr("AutoInbound.Layout.Grid", "Grid"),
	                          static_cast<int>(AutoLayoutMode::Grid));

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

static void vdoninja_output_defaults(obs_data_t *settings)
{
	const std::string defaultStreamId = generateSessionId();
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
	obs_data_set_default_int(settings, "video_codec", static_cast<int>(VideoCodec::H264));
	obs_data_set_default_int(settings, "bitrate", 4000);
	obs_data_set_default_int(settings, "max_viewers", 10);
	obs_data_set_default_bool(settings, "enable_data_channel", true);
	obs_data_set_default_bool(settings, "auto_reconnect", true);
	obs_data_set_default_bool(settings, "force_turn", false);
	obs_data_set_default_bool(settings, "auto_inbound_enabled", false);
	obs_data_set_default_string(settings, "auto_inbound_room_id", "");
	obs_data_set_default_string(settings, "auto_inbound_password", "");
	obs_data_set_default_string(settings, "auto_inbound_target_scene", "");
	obs_data_set_default_string(settings, "auto_inbound_source_prefix", "VDO");
	obs_data_set_default_string(settings, "auto_inbound_base_url", "https://vdo.ninja");
	obs_data_set_default_bool(settings, "auto_inbound_remove_on_disconnect", true);
	obs_data_set_default_bool(settings, "auto_inbound_switch_scene", false);
	obs_data_set_default_int(settings, "auto_inbound_layout_mode", static_cast<int>(AutoLayoutMode::Grid));
	obs_data_set_default_int(settings, "auto_inbound_width", 1920);
	obs_data_set_default_int(settings, "auto_inbound_height", 1080);
}

static uint64_t vdoninja_output_total_bytes(void *data)
{
	auto *vdo = static_cast<VDONinjaOutput *>(data);
	return vdo->getTotalBytes();
}

static int vdoninja_output_connect_time(void *data)
{
	auto *vdo = static_cast<VDONinjaOutput *>(data);
	return vdo->getConnectTime();
}

// Output info structure
obs_output_info vdoninja_output_info = {
    .id = "vdoninja_output",
    .flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE,
    .get_name = vdoninja_output_getname,
    .create = vdoninja_output_create,
    .destroy = vdoninja_output_destroy,
    .start = vdoninja_output_start,
    .stop = vdoninja_output_stop,
    .encoded_packet = vdoninja_output_data,
    .update = vdoninja_output_update,
    .get_defaults = vdoninja_output_defaults,
    .get_properties = vdoninja_output_properties,
    .get_total_bytes = vdoninja_output_total_bytes,
    .get_connect_time_ms = vdoninja_output_connect_time,
    .encoded_video_codecs = "h264",
    .encoded_audio_codecs = "opus",
    .protocols = "VDO.Ninja",
};

// Implementation

VDONinjaOutput::VDONinjaOutput(obs_data_t *settings, obs_output_t *output) : output_(output)
{
	loadSettings(settings);

	signaling_ = std::make_unique<VDONinjaSignaling>();
	peerManager_ = std::make_unique<VDONinjaPeerManager>();
	autoSceneManager_ = std::make_unique<VDOAutoSceneManager>();

	logInfo("VDO.Ninja output created");
}

VDONinjaOutput::~VDONinjaOutput()
{
	stop();
	logInfo("VDO.Ninja output destroyed");
}

void VDONinjaOutput::loadSettings(obs_data_t *settings)
{
	obs_data_t *serviceSettings = nullptr;
	if (output_) {
		obs_service_t *service = obs_output_get_service(output_);
		if (service) {
			serviceSettings = obs_service_get_settings(service);
		}
	}

	auto getStringSetting = [&](const char *key) -> std::string {
		std::string value;
		if (settings && (obs_data_has_user_value(settings, key) || !serviceSettings)) {
			const char *raw = obs_data_get_string(settings, key);
			if (raw) {
				value = raw;
			}
		}
		if (value.empty() && serviceSettings) {
			const char *raw = obs_data_get_string(serviceSettings, key);
			if (raw) {
				value = raw;
			}
		}
		return value;
	};

	auto getIntSetting = [&](const char *key, int fallback) -> int {
		if (settings && obs_data_has_user_value(settings, key)) {
			return static_cast<int>(obs_data_get_int(settings, key));
		}
		if (serviceSettings && obs_data_has_user_value(serviceSettings, key)) {
			return static_cast<int>(obs_data_get_int(serviceSettings, key));
		}
		if (settings) {
			return static_cast<int>(obs_data_get_int(settings, key));
		}
		if (serviceSettings) {
			return static_cast<int>(obs_data_get_int(serviceSettings, key));
		}
		return fallback;
	};

	auto getBoolSetting = [&](const char *key, bool fallback) -> bool {
		if (settings && obs_data_has_user_value(settings, key)) {
			return obs_data_get_bool(settings, key);
		}
		if (serviceSettings && obs_data_has_user_value(serviceSettings, key)) {
			return obs_data_get_bool(serviceSettings, key);
		}
		if (settings) {
			return obs_data_get_bool(settings, key);
		}
		if (serviceSettings) {
			return obs_data_get_bool(serviceSettings, key);
		}
		return fallback;
	};

	settings_.streamId = getStringSetting("stream_id");
	settings_.roomId = getStringSetting("room_id");
	settings_.password = getStringSetting("password");
	settings_.wssHost = getStringSetting("wss_host");
	std::string keySalt;
	const std::string streamKey = getStringSetting("key");
	const std::string serviceServer = getStringSetting("server");

	parseVdoKeyValue(streamKey, settings_.streamId, settings_.password, settings_.roomId, keySalt, settings_.wssHost);
	if (!keySalt.empty()) {
		settings_.salt = keySalt;
	}
	if (settings_.wssHost.empty() && !serviceServer.empty() &&
	    (startsWithInsensitive(serviceServer, "wss://") || startsWithInsensitive(serviceServer, "ws://"))) {
		settings_.wssHost = serviceServer;
	}

	const std::string configuredSalt = getStringSetting("salt");
	if (!configuredSalt.empty()) {
		settings_.salt = trim(configuredSalt);
	}
	settings_.customIceServers = parseIceServers(getStringSetting("custom_ice_servers"));

	if (settings_.wssHost.empty()) {
		settings_.wssHost = DEFAULT_WSS_HOST;
	}
	if (settings_.salt.empty()) {
		settings_.salt = DEFAULT_SALT;
	}

	const int configuredVideoCodec = getIntSetting("video_codec", static_cast<int>(VideoCodec::H264));
	settings_.videoCodec = VideoCodec::H264;
	if (configuredVideoCodec != static_cast<int>(VideoCodec::H264)) {
		logWarning("Only H.264 video is currently supported; overriding configured video codec to H.264");
	}
	settings_.quality.bitrate = getIntSetting("bitrate", 4000) * 1000;
	settings_.maxViewers = getIntSetting("max_viewers", 10);
	if (settings_.maxViewers <= 0) {
		settings_.maxViewers = 10;
	}
	settings_.enableDataChannel = getBoolSetting("enable_data_channel", true);
	settings_.autoReconnect = getBoolSetting("auto_reconnect", true);
	settings_.forceTurn = getBoolSetting("force_turn", false);
	settings_.enableRemote = getBoolSetting("enable_remote", false);

	settings_.autoInbound.enabled = getBoolSetting("auto_inbound_enabled", false);
	settings_.autoInbound.roomId = getStringSetting("auto_inbound_room_id");
	settings_.autoInbound.password = getStringSetting("auto_inbound_password");
	settings_.autoInbound.targetScene = getStringSetting("auto_inbound_target_scene");
	settings_.autoInbound.sourcePrefix = getStringSetting("auto_inbound_source_prefix");
	settings_.autoInbound.baseUrl = getStringSetting("auto_inbound_base_url");
	settings_.autoInbound.removeOnDisconnect = getBoolSetting("auto_inbound_remove_on_disconnect", true);
	settings_.autoInbound.switchToSceneOnNewStream = getBoolSetting("auto_inbound_switch_scene", false);
	settings_.autoInbound.layoutMode =
	    static_cast<AutoLayoutMode>(getIntSetting("auto_inbound_layout_mode", static_cast<int>(AutoLayoutMode::Grid)));
	settings_.autoInbound.width = getIntSetting("auto_inbound_width", 1920);
	settings_.autoInbound.height = getIntSetting("auto_inbound_height", 1080);

	if (settings_.autoInbound.sourcePrefix.empty()) {
		settings_.autoInbound.sourcePrefix = "VDO";
	}
	if (settings_.autoInbound.baseUrl.empty()) {
		settings_.autoInbound.baseUrl = "https://vdo.ninja";
	}
	if (settings_.autoInbound.password.empty()) {
		settings_.autoInbound.password = settings_.password;
	}
	// Pass salt and room to auto-inbound for URL building
	settings_.autoInbound.salt = settings_.salt;
	if (settings_.autoInbound.roomId.empty()) {
		settings_.autoInbound.roomId = settings_.roomId;
	}

	if (serviceSettings) {
		obs_data_release(serviceSettings);
	}
}

void VDONinjaOutput::update(obs_data_t *settings)
{
	std::lock_guard<std::mutex> lock(settingsMutex_);
	loadSettings(settings);
}

std::string VDONinjaOutput::buildInitialInfoMessage() const
{
	OutputSettings snap;
	{
		std::lock_guard<std::mutex> lock(settingsMutex_);
		snap = settings_;
	}

	JsonBuilder info;
	info.add("label", snap.streamId);
	info.add("version", kPluginInfoVersion);
	info.add("remote", snap.enableRemote);
	info.add("obs_control", snap.enableRemote);
	info.add("proaudio_init", false);
	info.add("recording_audio_pipeline", true);
	info.add("playback_audio_pipeline", true);
	info.add("playback_audio_volume_meter", true);
	info.add("codec_url", codecToUrlValue(snap.videoCodec));
	info.add("audio_codec_url", "opus");
	info.add("vb_url", snap.quality.bitrate / 1000);
	info.add("maxviewers_url", snap.maxViewers);

	obs_video_info videoInfo = {};
	if (obs_get_video_info(&videoInfo)) {
		const int fps = videoInfo.fps_den > 0
		                    ? static_cast<int>((videoInfo.fps_num + (videoInfo.fps_den / 2)) / videoInfo.fps_den)
		                    : 0;
		const int width = static_cast<int>(videoInfo.output_width ? videoInfo.output_width : videoInfo.base_width);
		const int height = static_cast<int>(videoInfo.output_height ? videoInfo.output_height : videoInfo.base_height);
		if (width > 0) {
			info.add("video_init_width", width);
		}
		if (height > 0) {
			info.add("video_init_height", height);
		}
		if (fps > 0) {
			info.add("video_init_frameRate", fps);
		}
	}

	obs_audio_info audioInfo = {};
	if (obs_get_audio_info(&audioInfo)) {
		const uint32_t channels = get_audio_channels(audioInfo.speakers);
		info.add("stereo_url", channels >= 2);
		if (audioInfo.samples_per_sec > 0) {
			info.add("playback_audio_samplerate", static_cast<int>(audioInfo.samples_per_sec));
		}
	}

	JsonBuilder payload;
	payload.addRaw("info", info.build());
	if (snap.enableRemote) {
		payload.add("remote", true);
	}
	return payload.build();
}

std::string VDONinjaOutput::buildObsStateMessage() const
{
	// Build obsState.details matching VDO.Ninja's browser dock format.
	// The viewer needs controlLevel >= 4 to show remote control buttons.
	// controlLevel 5 = ALL (full control).
	JsonBuilder details;
	details.add("controlLevel", 5);

	// Include current scene and scene list so the viewer can show scene buttons
	obs_source_t *currentScene = obs_frontend_get_current_scene();
	if (currentScene) {
		const char *sceneName = obs_source_get_name(currentScene);
		JsonBuilder currentSceneObj;
		currentSceneObj.add("name", sceneName ? sceneName : "");
		details.addRaw("currentScene", currentSceneObj.build());
		obs_source_release(currentScene);
	}

	struct obs_frontend_source_list sceneList = {};
	obs_frontend_get_scenes(&sceneList);
	std::string scenesArray = "[";
	for (size_t i = 0; i < sceneList.sources.num; i++) {
		const char *name = obs_source_get_name(sceneList.sources.array[i]);
		if (i > 0) scenesArray += ",";
		// JSON-escape the scene name
		std::string nameStr = name ? name : "";
		std::string escaped = "\"";
		for (char c : nameStr) {
			if (c == '"') escaped += "\\\"";
			else if (c == '\\') escaped += "\\\\";
			else escaped += c;
		}
		escaped += "\"";
		scenesArray += escaped;
	}
	scenesArray += "]";
	obs_frontend_source_list_free(&sceneList);
	details.addRaw("scenes", scenesArray);

	JsonBuilder obsState;
	// Mirror the OBS browser-dock obsState shape so guest tally/remote UI works.
	obsState.add("visibility", true);
	obsState.add("sourceActive", true);
	obsState.add("streaming", obs_frontend_streaming_active());
	obsState.add("recording", obs_frontend_recording_active());
	obsState.add("virtualcam", obs_frontend_virtualcam_active());
	obsState.addRaw("details", details.build());

	JsonBuilder msg;
	msg.addRaw("obsState", obsState.build());
	return msg.build();
}

void VDONinjaOutput::sendObsStateToPeer(const std::string &uuid)
{
	if (!peerManager_ || uuid.empty()) {
		return;
	}

	OutputSettings snap;
	{
		std::lock_guard<std::mutex> lock(settingsMutex_);
		snap = settings_;
	}
	if (!snap.enableRemote) {
		return;
	}

	peerManager_->sendDataToPeer(uuid, buildObsStateMessage());
}

void VDONinjaOutput::queueObsStateToPeer(const std::string &uuid)
{
	if (uuid.empty()) {
		return;
	}

	struct ObsStateTaskData {
		VDONinjaOutput *self;
		std::string uuid;
	};

	auto *task = new ObsStateTaskData{this, uuid};
	obs_queue_task(OBS_TASK_UI,
	               [](void *param) {
		               std::unique_ptr<ObsStateTaskData> data(static_cast<ObsStateTaskData *>(param));
		               if (!data || !data->self) {
			               return;
		               }
		               data->self->sendObsStateToPeer(data->uuid);
	               },
	               task, false);
}

void VDONinjaOutput::sendInitialPeerInfo(const std::string &uuid)
{
	if (!peerManager_ || uuid.empty()) {
		return;
	}

	peerManager_->sendDataToPeer(uuid, buildInitialInfoMessage());
	// Build/send OBS state from UI thread (OBS frontend APIs are UI-affine).
	queueObsStateToPeer(uuid);
}

void VDONinjaOutput::primeViewerWithCachedKeyframe(const std::string &uuid)
{
	if (!peerManager_ || uuid.empty()) {
		return;
	}

	std::vector<uint8_t> keyframeCopy;
	uint32_t keyframeTimestamp = 0;
	{
		std::lock_guard<std::mutex> lock(keyframeCacheMutex_);
		if (cachedKeyframe_.empty()) {
			return;
		}
		keyframeCopy = cachedKeyframe_;
		keyframeTimestamp = cachedKeyframeTimestamp_;
	}

	if (peerManager_->sendVideoFrameToPeer(uuid, keyframeCopy.data(), keyframeCopy.size(), keyframeTimestamp, true)) {
		logInfo("Primed viewer %s with cached keyframe (%zu bytes)", uuid.c_str(), keyframeCopy.size());
	}
}

bool VDONinjaOutput::start()
{
	if (running_) {
		logWarning("Output already running");
		return false;
	}

	if (settings_.streamId.empty()) {
		logError("Stream ID is required");
		obs_output_signal_stop(output_, OBS_OUTPUT_INVALID_STREAM);
		return false;
	}

	if (!obs_output_can_begin_data_capture(output_, 0)) {
		logError("Output cannot begin data capture");
		return false;
	}

	std::string nonOpusCodec;
	if (!validateOpusAudioEncoders(output_, nonOpusCodec)) {
		const std::string error =
		    "VDO.Ninja requires Opus audio. Open Tools -> VDO.Ninja Control Center, then retry Start Streaming.";
		logError("Refusing to start: active audio encoder codec is '%s' (Opus required)", nonOpusCodec.c_str());
		obs_output_set_last_error(output_, error.c_str());
		obs_output_signal_stop(output_, OBS_OUTPUT_ERROR);
		return false;
	}

	if (!obs_output_initialize_encoders(output_, 0)) {
		logError("Failed to initialize output encoders");
		obs_output_signal_stop(output_, OBS_OUTPUT_ERROR);
		return false;
	}

	running_ = true;
	startTimeMs_ = currentTimeMs();
	capturing_ = false;
	totalBytes_ = 0;
	connected_ = false;
	{
		std::lock_guard<std::mutex> lock(keyframeCacheMutex_);
		cachedKeyframe_.clear();
		cachedKeyframeTimestamp_ = 0;
	}

	if (startStopThread_.joinable()) {
		startStopThread_.join();
	}

	// Snapshot settings under lock for the start thread
	OutputSettings settingsSnap;
	{
		std::lock_guard<std::mutex> lock(settingsMutex_);
		settingsSnap = settings_;
	}

	startStopThread_ = std::thread(&VDONinjaOutput::startThread, this, settingsSnap);

	return true;
}

void VDONinjaOutput::startThread(OutputSettings settingsSnap)
{
	logInfo("Starting VDO.Ninja output...");

	// Initialize peer manager
	peerManager_->initialize(signaling_.get());
	peerManager_->setVideoCodec(settingsSnap.videoCodec);
	peerManager_->setAudioCodec(settingsSnap.audioCodec);
	peerManager_->setBitrate(settingsSnap.quality.bitrate);
	peerManager_->setEnableDataChannel(settingsSnap.enableDataChannel);
	peerManager_->setIceServers(settingsSnap.customIceServers);
	peerManager_->setForceTurn(settingsSnap.forceTurn);
	signaling_->setSalt(settingsSnap.salt);

	if (autoSceneManager_) {
		autoSceneManager_->configure(settingsSnap.autoInbound);
		std::vector<std::string> ownIds = {settingsSnap.streamId,
		                                   hashStreamId(settingsSnap.streamId, settingsSnap.password, settingsSnap.salt),
		                                   hashStreamId(settingsSnap.streamId, DEFAULT_PASSWORD, settingsSnap.salt)};
		autoSceneManager_->setOwnStreamIds(ownIds);
		if (settingsSnap.autoInbound.enabled) {
			autoSceneManager_->start();
		}
	}

	// Set up callbacks
	signaling_->setOnConnected([this, settingsSnap]() {
		logInfo("Connected to signaling server");

		const std::string roomToJoin =
		    !settingsSnap.autoInbound.roomId.empty() ? settingsSnap.autoInbound.roomId : settingsSnap.roomId;
		const std::string roomPassword =
		    !settingsSnap.autoInbound.password.empty() ? settingsSnap.autoInbound.password : settingsSnap.password;

		// Join room for inbound orchestration and/or publishing presence.
		if (!roomToJoin.empty()) {
			signaling_->joinRoom(roomToJoin, roomPassword);
		}

		// Start publishing
		signaling_->publishStream(settingsSnap.streamId, settingsSnap.password);
		peerManager_->startPublishing(settingsSnap.maxViewers);

		connected_ = true;
		connectTimeMs_ = currentTimeMs() - startTimeMs_;

		if (!capturing_) {
			if (obs_output_begin_data_capture(output_, 0)) {
				capturing_ = true;
			} else {
				logError("Failed to begin OBS data capture");
				obs_output_signal_stop(output_, OBS_OUTPUT_ERROR);
				running_ = false;
				connected_ = false;
			}
		}
	});

	signaling_->setOnDisconnected([this, settingsSnap]() {
		logInfo("Disconnected from signaling server");
		connected_ = false;

		if (running_ && settingsSnap.autoReconnect) {
			logInfo("Will attempt to reconnect...");
		}
	});

	signaling_->setOnError([this](const std::string &error) {
		logError("Signaling error: %s", error.c_str());
		obs_output_set_last_error(output_, error.c_str());

		const bool streamIdConflict =
		    containsInsensitive(error, "already in use") || containsInsensitive(error, "already claimed");
		if (streamIdConflict && running_) {
			logError("Stopping publish due to signaling conflict (stream/room already claimed)");
			obs_output_signal_stop(output_, OBS_OUTPUT_ERROR);
		}
	});

	signaling_->setOnRoomJoined([this, settingsSnap](const std::vector<std::string> &members) {
		if (autoSceneManager_ && settingsSnap.autoInbound.enabled) {
			autoSceneManager_->onRoomListing(members);
		}
	});

	signaling_->setOnStreamAdded([this, settingsSnap](const std::string &streamId, const std::string &) {
		if (autoSceneManager_ && settingsSnap.autoInbound.enabled) {
			autoSceneManager_->onStreamAdded(streamId);
		}
	});

	signaling_->setOnStreamRemoved([this, settingsSnap](const std::string &streamId, const std::string &) {
		if (autoSceneManager_ && settingsSnap.autoInbound.enabled) {
			autoSceneManager_->onStreamRemoved(streamId);
		}
	});

	peerManager_->setOnPeerConnected([this](const std::string &uuid) {
		logInfo("Viewer connected: %s (total: %d)", uuid.c_str(), peerManager_->getViewerCount());
		primeViewerWithCachedKeyframe(uuid);
	});

	peerManager_->setOnPeerDisconnected([this](const std::string &uuid) {
		{
			std::lock_guard<std::mutex> lock(telemetryMutex_);
			lastPeerStats_.erase(uuid);
			lastPeerStatsTimestampMs_.erase(uuid);
		}
		logInfo("Viewer disconnected: %s (total: %d)", uuid.c_str(), peerManager_->getViewerCount());
	});

	peerManager_->setOnDataChannel(
	    [this](const std::string &uuid, std::shared_ptr<rtc::DataChannel>) { sendInitialPeerInfo(uuid); });

	peerManager_->setOnDataChannelMessage([this, settingsSnap](const std::string &uuid, const std::string &message) {
		dataChannel_.handleMessage(uuid, message);

		const DataMessage parsed = dataChannel_.parseMessage(message);
		if (parsed.type == DataMessageType::RequestKeyframe) {
			logInfo("Viewer %s requested keyframe over data channel", uuid.c_str());
			primeViewerWithCachedKeyframe(uuid);
		}

		if (parsed.type == DataMessageType::Stats) {
			std::lock_guard<std::mutex> lock(telemetryMutex_);
			lastPeerStats_[uuid] = parsed.data.empty() ? message : parsed.data;
			lastPeerStatsTimestampMs_[uuid] = currentTimeMs();
		}

		if (settingsSnap.enableRemote) {
			bool wantsObsState = parsed.type == DataMessageType::RemoteControl;
			try {
				JsonParser json(message);
				if (json.hasKey("getOBSState") && json.getBool("getOBSState")) {
					wantsObsState = true;
				}
			} catch (const std::exception &) {
			}

			if (wantsObsState) {
				queueObsStateToPeer(uuid);
			}
		}

		if (autoSceneManager_ && settingsSnap.autoInbound.enabled) {
			const std::string whepUrl = dataChannel_.extractWhepPlaybackUrl(message);
			if (!whepUrl.empty()) {
				logInfo("Discovered WHEP playback URL from %s", uuid.c_str());
				autoSceneManager_->onStreamAdded(whepUrl);
			}
		}
	});

	// Set up chat callback to forward to dock
	dataChannel_.setOnChatMessage([](const std::string &senderId, const std::string &message) {
		struct ChatData {
			std::string sender;
			std::string message;
		};
		auto *data = new ChatData{senderId, message};
		obs_queue_task(OBS_TASK_UI, [](void *param) {
			auto *cd = static_cast<ChatData *>(param);
			vdo_dock_show_chat(cd->sender.c_str(), cd->message.c_str());
			delete cd;
		}, data, false);
	});

	// Set up remote control callback
	if (settingsSnap.enableRemote) {
		dataChannel_.setOnRemoteControl([](const std::string &action, const std::string &value) {
			struct RemoteData {
				std::string action;
				std::string value;
			};
			auto *data = new RemoteData{action, value};
			obs_queue_task(OBS_TASK_UI, [](void *param) {
				auto *rd = static_cast<RemoteData *>(param);
				vdo_handle_remote_control(rd->action.c_str(), rd->value.c_str());
				delete rd;
			}, data, false);
		});
	}

	// Configure reconnection
	signaling_->setAutoReconnect(settingsSnap.autoReconnect, DEFAULT_RECONNECT_ATTEMPTS);

	// Connect to signaling server
	if (!signaling_->connect(settingsSnap.wssHost)) {
		logError("Failed to connect to signaling server");
		if (autoSceneManager_) {
			autoSceneManager_->stop();
		}
		obs_output_signal_stop(output_, OBS_OUTPUT_CONNECT_FAILED);
		running_ = false;
		return;
	}

	logInfo("VDO.Ninja output started successfully");
}

void VDONinjaOutput::stop()
{
	bool expected = false;
	if (!stopping_.compare_exchange_strong(expected, true)) {
		logDebug("VDO.Ninja output stop already in progress");
		return;
	}

	const bool wasRunning = running_.exchange(false);
	const bool wasCapturing = capturing_.load();
	if (!wasRunning && !wasCapturing) {
		// Still join the thread in case startThread failed and set running_=false
		// but is still cleaning up.
		if (startStopThread_.joinable()) {
			startStopThread_.join();
		}
		stopping_ = false;
		return;
	}

	connected_ = false;

	logInfo("Stopping VDO.Ninja output...");

	if (autoSceneManager_) {
		autoSceneManager_->stop();
	}

	// Clear all callbacks before disconnect to prevent dangling `this` captures
	// from firing during teardown. This includes peer manager's signaling callbacks.
	signaling_->setOnConnected(nullptr);
	signaling_->setOnDisconnected(nullptr);
	signaling_->setOnError(nullptr);
	signaling_->setOnRoomJoined(nullptr);
	signaling_->setOnStreamAdded(nullptr);
	signaling_->setOnStreamRemoved(nullptr);
	signaling_->setOnOffer(nullptr);
	signaling_->setOnAnswer(nullptr);
	signaling_->setOnOfferRequest(nullptr);
	signaling_->setOnIceCandidate(nullptr);
	peerManager_->setOnPeerConnected(nullptr);
	peerManager_->setOnPeerDisconnected(nullptr);
	peerManager_->setOnDataChannel(nullptr);
	peerManager_->setOnDataChannelMessage(nullptr);
	dataChannel_.setOnChatMessage(nullptr);
	dataChannel_.setOnRemoteControl(nullptr);
	dataChannel_.setOnTallyChange(nullptr);

	// Stop publishing
	peerManager_->stopPublishing();
	{
		std::lock_guard<std::mutex> lock(telemetryMutex_);
		lastPeerStats_.clear();
		lastPeerStatsTimestampMs_.clear();
	}
	{
		std::lock_guard<std::mutex> lock(keyframeCacheMutex_);
		cachedKeyframe_.clear();
		cachedKeyframeTimestamp_ = 0;
	}

	// Unpublish stream
	if (signaling_->isPublishing()) {
		signaling_->unpublishStream();
	}

	// Leave room
	if (signaling_->isInRoom()) {
		signaling_->leaveRoom();
	}

	// Disconnect
	signaling_->disconnect();

	// Wait for start thread to finish
	if (startStopThread_.joinable()) {
		startStopThread_.join();
	}

	// End data capture
	if (capturing_) {
		obs_output_end_data_capture(output_);
		capturing_ = false;
	}

	stopping_ = false;
	logInfo("VDO.Ninja output stopped");
}

void VDONinjaOutput::data(encoder_packet *packet)
{
	if (!running_ || !connected_)
		return;

	if (packet->type == OBS_ENCODER_VIDEO) {
		processVideoPacket(packet);
	} else if (packet->type == OBS_ENCODER_AUDIO) {
		processAudioPacket(packet);
	}

	totalBytes_ += packet->size;
}

void VDONinjaOutput::processVideoPacket(encoder_packet *packet)
{
	bool keyframe = packet->keyframe;
	uint32_t timestamp = static_cast<uint32_t>(packet->pts * 90); // Convert to 90kHz clock

	if (keyframe && packet->data && packet->size > 0) {
		std::lock_guard<std::mutex> lock(keyframeCacheMutex_);
		cachedKeyframe_.assign(packet->data, packet->data + packet->size);
		cachedKeyframeTimestamp_ = timestamp;
	}

	peerManager_->sendVideoFrame(packet->data, packet->size, timestamp, keyframe);
}

void VDONinjaOutput::processAudioPacket(encoder_packet *packet)
{
	uint32_t timestamp = static_cast<uint32_t>(packet->pts * 48); // Convert to 48kHz clock

	peerManager_->sendAudioFrame(packet->data, packet->size, timestamp);
}

uint64_t VDONinjaOutput::getTotalBytes() const
{
	return totalBytes_;
}

int VDONinjaOutput::getConnectTime() const
{
	return static_cast<int>(connectTimeMs_);
}

int VDONinjaOutput::getViewerCount() const
{
	return peerManager_ ? peerManager_->getViewerCount() : 0;
}

int VDONinjaOutput::getMaxViewers() const
{
	return peerManager_ ? peerManager_->getMaxViewers() : 0;
}

TallyState VDONinjaOutput::getAggregatedTally() const
{
	TallyState aggregated;
	auto tallies = dataChannel_.getAllPeerTallies();
	for (const auto &pair : tallies) {
		if (pair.second.program) {
			aggregated.program = true;
		}
		if (pair.second.preview) {
			aggregated.preview = true;
		}
	}
	return aggregated;
}

bool VDONinjaOutput::isRemoteControlEnabled() const
{
	std::lock_guard<std::mutex> lock(settingsMutex_);
	return settings_.enableRemote;
}

bool VDONinjaOutput::isRunning() const
{
	return running_;
}

bool VDONinjaOutput::isConnected() const
{
	return connected_;
}

int64_t VDONinjaOutput::getUptimeMs() const
{
	if (startTimeMs_ <= 0) {
		return 0;
	}

	const int64_t now = currentTimeMs();
	return now > startTimeMs_ ? (now - startTimeMs_) : 0;
}

OutputSettings VDONinjaOutput::getSettingsSnapshot() const
{
	std::lock_guard<std::mutex> lock(settingsMutex_);
	return settings_;
}

std::vector<VDONinjaOutput::ViewerRuntimeSnapshot> VDONinjaOutput::getViewerSnapshots() const
{
	std::vector<ViewerRuntimeSnapshot> snapshots;
	if (!peerManager_) {
		return snapshots;
	}

	const std::vector<PeerSnapshot> peers = peerManager_->getPeerSnapshots();
	snapshots.reserve(peers.size());

	std::lock_guard<std::mutex> lock(telemetryMutex_);
	for (const auto &peer : peers) {
		ViewerRuntimeSnapshot snapshot;
		snapshot.uuid = peer.uuid;
		snapshot.streamId = peer.streamId;
		snapshot.role = connectionTypeToString(peer.type);
		snapshot.state = connectionStateToString(peer.state);
		snapshot.hasDataChannel = peer.hasDataChannel;

		auto statsIt = lastPeerStats_.find(peer.uuid);
		if (statsIt != lastPeerStats_.end()) {
			snapshot.lastStats = statsIt->second;
		}

		auto statsTsIt = lastPeerStatsTimestampMs_.find(peer.uuid);
		if (statsTsIt != lastPeerStatsTimestampMs_.end()) {
			snapshot.lastStatsTimestampMs = statsTsIt->second;
		}

		snapshots.emplace_back(std::move(snapshot));
	}

	return snapshots;
}

} // namespace vdoninja
