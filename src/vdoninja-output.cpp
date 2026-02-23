/*
 * OBS VDO.Ninja Plugin
 * Output module implementation
 */

#include "vdoninja-output.h"

#include <cctype>
#include <cstring>

#include <util/dstr.h>
#include <util/threading.h>

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

	obs_properties_t *advanced = obs_properties_create();
	obs_properties_add_text(advanced, "wss_host", tr("SignalingServer", "Signaling Server"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(advanced, "salt", tr("Salt", "Salt"), OBS_TEXT_DEFAULT);
	obs_property_t *iceServers = obs_properties_add_text(
	    advanced, "custom_ice_servers", tr("CustomICEServers", "Custom STUN/TURN Servers"), OBS_TEXT_MULTILINE);
	obs_property_text_set_monospace(iceServers, true);
	obs_properties_add_bool(advanced, "force_turn", tr("ForceTURN", "Force TURN Relay"));
	obs_properties_add_group(props, "advanced", tr("AdvancedSettings", "Advanced Settings"), OBS_GROUP_NORMAL,
	                         advanced);

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

	return props;
}

static void vdoninja_output_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "stream_id", "");
	obs_data_set_default_string(settings, "room_id", "");
	obs_data_set_default_string(settings, "password", "");
	obs_data_set_default_string(settings, "wss_host", DEFAULT_WSS_HOST);
	obs_data_set_default_string(settings, "salt", DEFAULT_SALT);
	obs_data_set_default_string(settings, "custom_ice_servers", "");
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
	stop(false);
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
	if (settings_.wssHost.empty() && !serviceServer.empty()) {
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
	settings_.enableDataChannel = getBoolSetting("enable_data_channel", true);
	settings_.autoReconnect = getBoolSetting("auto_reconnect", true);
	settings_.forceTurn = getBoolSetting("force_turn", false);

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

	if (serviceSettings) {
		obs_data_release(serviceSettings);
	}
}

void VDONinjaOutput::update(obs_data_t *settings)
{
	loadSettings(settings);
}

std::string VDONinjaOutput::buildInitialInfoMessage() const
{
	JsonBuilder info;
	info.add("label", settings_.streamId);
	info.add("version", kPluginInfoVersion);
	info.add("obs_control", false);
	info.add("proaudio_init", false);
	info.add("recording_audio_pipeline", true);
	info.add("playback_audio_pipeline", true);
	info.add("playback_audio_volume_meter", true);
	info.add("codec_url", codecToUrlValue(settings_.videoCodec));
	info.add("audio_codec_url", "opus");
	info.add("vb_url", settings_.quality.bitrate / 1000);
	info.add("maxviewers_url", settings_.maxViewers);

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
	return payload.build();
}

void VDONinjaOutput::sendInitialPeerInfo(const std::string &uuid)
{
	if (!peerManager_ || uuid.empty()) {
		return;
	}

	peerManager_->sendDataToPeer(uuid, buildInitialInfoMessage());
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
		    "VDO.Ninja requires Opus audio. Open Tools -> Configure VDO.Ninja, then retry Start Streaming.";
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
	{
		std::lock_guard<std::mutex> lock(keyframeCacheMutex_);
		cachedKeyframe_.clear();
		cachedKeyframeTimestamp_ = 0;
	}

	if (startStopThread_.joinable()) {
		startStopThread_.join();
	}

	startStopThread_ = std::thread(&VDONinjaOutput::startThread, this);

	return true;
}

void VDONinjaOutput::startThread()
{
	logInfo("Starting VDO.Ninja output...");

	// Initialize peer manager
	peerManager_->initialize(signaling_.get());
	peerManager_->setVideoCodec(settings_.videoCodec);
	peerManager_->setAudioCodec(settings_.audioCodec);
	peerManager_->setBitrate(settings_.quality.bitrate);
	peerManager_->setEnableDataChannel(settings_.enableDataChannel);
	peerManager_->setIceServers(settings_.customIceServers);
	peerManager_->setForceTurn(settings_.forceTurn);
	signaling_->setSalt(settings_.salt);

	if (autoSceneManager_) {
		autoSceneManager_->configure(settings_.autoInbound);
		std::vector<std::string> ownIds = {settings_.streamId,
		                                   hashStreamId(settings_.streamId, settings_.password, settings_.salt),
		                                   hashStreamId(settings_.streamId, DEFAULT_PASSWORD, settings_.salt)};
		autoSceneManager_->setOwnStreamIds(ownIds);
		if (settings_.autoInbound.enabled) {
			autoSceneManager_->start();
		}
	}

	// Set up callbacks
	signaling_->setOnConnected([this]() {
		logInfo("Connected to signaling server");

		const std::string roomToJoin =
		    !settings_.autoInbound.roomId.empty() ? settings_.autoInbound.roomId : settings_.roomId;
		const std::string roomPassword =
		    !settings_.autoInbound.password.empty() ? settings_.autoInbound.password : settings_.password;

		// Join room for inbound orchestration and/or publishing presence.
		if (!roomToJoin.empty()) {
			signaling_->joinRoom(roomToJoin, roomPassword);
		}

		// Start publishing
		signaling_->publishStream(settings_.streamId, settings_.password);
		peerManager_->startPublishing(settings_.maxViewers);

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

	signaling_->setOnDisconnected([this]() {
		logInfo("Disconnected from signaling server");
		connected_ = false;

		if (running_ && settings_.autoReconnect) {
			logInfo("Will attempt to reconnect...");
		}
	});

	signaling_->setOnError([this](const std::string &error) { logError("Signaling error: %s", error.c_str()); });

	signaling_->setOnRoomJoined([this](const std::vector<std::string> &members) {
		if (autoSceneManager_ && settings_.autoInbound.enabled) {
			autoSceneManager_->onRoomListing(members);
		}
	});

	signaling_->setOnStreamAdded([this](const std::string &streamId, const std::string &) {
		if (autoSceneManager_ && settings_.autoInbound.enabled) {
			autoSceneManager_->onStreamAdded(streamId);
		}
	});

	signaling_->setOnStreamRemoved([this](const std::string &streamId, const std::string &) {
		if (autoSceneManager_ && settings_.autoInbound.enabled) {
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

	peerManager_->setOnDataChannelMessage([this](const std::string &uuid, const std::string &message) {
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

		if (autoSceneManager_ && settings_.autoInbound.enabled) {
			const std::string whepUrl = dataChannel_.extractWhepPlaybackUrl(message);
			if (!whepUrl.empty()) {
				logInfo("Discovered WHEP playback URL from %s", uuid.c_str());
				autoSceneManager_->onStreamAdded(whepUrl);
			}
		}
	});

	// Configure reconnection
	signaling_->setAutoReconnect(settings_.autoReconnect, DEFAULT_RECONNECT_ATTEMPTS);

	// Connect to signaling server
	if (!signaling_->connect(settings_.wssHost)) {
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

void VDONinjaOutput::stop(bool signal)
{
	if (!running_)
		return;

	running_ = false;
	connected_ = false;

	logInfo("Stopping VDO.Ninja output...");

	if (autoSceneManager_) {
		autoSceneManager_->stop();
	}

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

	if (signal) {
		obs_output_signal_stop(output_, OBS_OUTPUT_SUCCESS);
	}

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
