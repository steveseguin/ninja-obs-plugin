/*
 * OBS VDO.Ninja Plugin
 * Output module implementation
 */

#include "vdoninja-output.h"

#include <obs-frontend-api.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

#include <util/config-file.h>
#include <util/dstr.h>
#include <util/threading.h>

#include "plugin-main.h"
#include "vdoninja-h264-profile.h"
#include "vdoninja-utils.h"

namespace vdoninja
{

namespace
{

constexpr size_t kMaxQueuedMediaFrames = 240;
constexpr int kRemoteStatsIntervalMs = 3000;

// The service clamps the encoder to a 2s keyframe interval, so a sustained gap
// well past that means the cap was bypassed (for example via the advanced
// "ignore streaming service setting recommendations" toggle).
constexpr int64_t kKeyframeIntervalWarnMs = 4500;

// How often the rolling publish summary is flushed to the log. Short enough that
// a brief reproduction still produces a couple of samples, long enough that a
// multi-hour stream does not drown out everything else in the log.
constexpr int64_t kPublishSummaryIntervalMs = 30000;
constexpr int64_t kBitrateAdaptationIntervalMs = 1000;
constexpr int64_t kAdaptivePacerSettleDelayMs = 1500;
constexpr auto kRecentRembMaximumAge = std::chrono::milliseconds(3000);

int64_t steadyTimeMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

void updateAtomicMaximum(std::atomic<uint64_t> &target, uint64_t value)
{
	uint64_t current = target.load(std::memory_order_relaxed);
	while (current < value &&
	       !target.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

int resolveVideoEncoderBitrate(obs_output_t *output, int fallbackBitsPerSecond)
{
	if (!output) {
		return fallbackBitsPerSecond;
	}

	obs_encoder_t *encoder = obs_output_get_video_encoder(output);
	if (!encoder) {
		return fallbackBitsPerSecond;
	}

	obs_data_t *encoderSettings = obs_encoder_get_settings(encoder);
	if (!encoderSettings) {
		return fallbackBitsPerSecond;
	}
	const int64_t bitrateKbps = obs_data_get_int(encoderSettings, "bitrate");
	obs_data_release(encoderSettings);
	if (bitrateKbps <= 0 || bitrateKbps > std::numeric_limits<int>::max() / 1000) {
		return fallbackBitsPerSecond;
	}
	return static_cast<int>(bitrateKbps * 1000);
}

const char *tr(const char *key, const char *fallback)
{
	const char *localized = obs_module_text(key);
	if (!localized || !*localized || std::strcmp(localized, key) == 0) {
		return fallback;
	}
	return localized;
}

template <typename Fn> void runNoexceptCallback(const char *context, Fn &&fn)
{
	try {
		fn();
	} catch (const std::exception &e) {
		logError("%s threw exception: %s", context, e.what());
	} catch (...) {
		logError("%s threw unknown exception", context);
	}
}

template <typename T, typename Fn> T runNoexceptCallbackValue(const char *context, T fallback, Fn &&fn)
{
	try {
		return fn();
	} catch (const std::exception &e) {
		logError("%s threw exception: %s", context, e.what());
	} catch (...) {
		logError("%s threw unknown exception", context);
	}
	return fallback;
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

bool looksLikeJsonContainer(const std::string &value)
{
	const std::string normalized = trim(value);
	if (normalized.empty()) {
		return false;
	}
	return normalized.front() == '{' || normalized.front() == '[';
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

constexpr const char *kPluginInfoVersion = PLUGIN_VERSION;
constexpr size_t kMaxAudioMixes = 6;

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

bool rebindOutputAudioEncodersToOpus(obs_output_t *output, std::string &errorMessage)
{
	if (!output) {
		return true;
	}

	const std::string opusEncoderId = findAudioEncoderIdForCodec("opus");
	if (opusEncoderId.empty()) {
		errorMessage = "Opus audio encoder is unavailable in this OBS build.";
		return false;
	}

	audio_t *audio = obs_get_audio();
	if (!audio) {
		errorMessage = "OBS audio subsystem is unavailable.";
		return false;
	}

	for (size_t i = 0; i < kMaxAudioMixes; ++i) {
		obs_encoder_t *audioEncoder = obs_output_get_audio_encoder(output, i);
		if (!audioEncoder) {
			continue;
		}

		const char *codec = obs_encoder_get_codec(audioEncoder);
		if (codec && std::strcmp(codec, "opus") == 0) {
			continue;
		}

		const size_t mixerIndex = obs_encoder_get_mixer_index(audioEncoder);
		const char *encoderName = obs_encoder_get_name(audioEncoder);
		const std::string name =
		    (encoderName && *encoderName) ? encoderName : ("vdoninja_stream_audio_" + std::to_string(i));

		obs_data_t *encoderSettings = obs_data_create();
		obs_data_t *existingSettings = obs_encoder_get_settings(audioEncoder);
		if (existingSettings) {
			const int64_t bitrate = obs_data_get_int(existingSettings, "bitrate");
			if (bitrate > 0) {
				obs_data_set_int(encoderSettings, "bitrate", bitrate);
			}
			obs_data_release(existingSettings);
		}

		obs_encoder_t *opusEncoder =
		    obs_audio_encoder_create(opusEncoderId.c_str(), name.c_str(), encoderSettings, mixerIndex, nullptr);
		obs_data_release(encoderSettings);
		if (!opusEncoder) {
			logError("Failed to create Opus encoder '%s' for output audio index %zu", opusEncoderId.c_str(), i);
			errorMessage = "Unable to create Opus audio encoder for VDO.Ninja streaming.";
			return false;
		}

		obs_encoder_set_audio(opusEncoder, audio);
		obs_output_set_audio_encoder(output, opusEncoder, i);
		obs_encoder_release(opusEncoder);

		logInfo("Rebound output audio encoder index %zu to Opus (%s), mixer index %zu", i, opusEncoderId.c_str(),
		        mixerIndex);
	}

	return true;
}

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

size_t getPreferredStreamAudioTrackIndex()
{
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile) {
		return 0;
	}

	const char *outputModeRaw = config_get_string(profile, "Output", "Mode");
	const bool advancedOutput = outputModeRaw && std::strcmp(outputModeRaw, "Advanced") == 0;

	uint64_t oneBasedTrackIndex = 0;
	if (advancedOutput) {
		oneBasedTrackIndex = config_get_uint(profile, "AdvOut", "TrackIndex");
	} else {
		oneBasedTrackIndex = config_get_uint(profile, "SimpleOutput", "TrackIndex");
	}

	if (oneBasedTrackIndex == 0 || oneBasedTrackIndex > kMaxAudioMixes) {
		return 0;
	}

	return static_cast<size_t>(oneBasedTrackIndex - 1);
}

size_t resolveOutputAudioTrackIndex(obs_output_t *output)
{
	const size_t preferredTrackIdx = getPreferredStreamAudioTrackIndex();
	if (!output) {
		return preferredTrackIdx;
	}

	obs_encoder_t *preferred = obs_output_get_audio_encoder(output, preferredTrackIdx);
	if (preferred) {
		return preferredTrackIdx;
	}

	for (size_t i = 0; i < kMaxAudioMixes; ++i) {
		if (obs_output_get_audio_encoder(output, i)) {
			return i;
		}
	}

	return preferredTrackIdx;
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
	} catch (...) {
		logError("Failed to create VDO.Ninja output: unknown exception");
		return nullptr;
	}
}

static void vdoninja_output_destroy(void *data)
{
	runNoexceptCallback("vdoninja_output_destroy", [data]() {
		auto *vdo = static_cast<VDONinjaOutput *>(data);
		delete vdo;
	});
}

static bool vdoninja_output_start(void *data)
{
	return runNoexceptCallbackValue<bool>("vdoninja_output_start", false, [data]() {
		auto *vdo = static_cast<VDONinjaOutput *>(data);
		return vdo->start();
	});
}

static void vdoninja_output_stop(void *data, uint64_t)
{
	runNoexceptCallback("vdoninja_output_stop", [data]() {
		auto *vdo = static_cast<VDONinjaOutput *>(data);
		vdo->stop();
	});
}

static void vdoninja_output_data(void *data, encoder_packet *packet)
{
	runNoexceptCallback("vdoninja_output_data", [data, packet]() {
		auto *vdo = static_cast<VDONinjaOutput *>(data);
		vdo->data(packet);
	});
}

static void vdoninja_output_update(void *data, obs_data_t *settings)
{
	runNoexceptCallback("vdoninja_output_update", [data, settings]() {
		auto *vdo = static_cast<VDONinjaOutput *>(data);
		vdo->update(settings);
	});
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
	    wssHost, tr("SignalingServer.OptionalHelp",
	                "Optional. Leave blank to use default signaling server: wss://wss.vdo.ninja:443. "
	                "Alternate fallback: wss://proxywss.rtc.ninja:443"));
	obs_property_t *salt = obs_properties_add_text(advanced, "salt", tr("Salt", "Salt"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(salt,
	                                  tr("Salt.OptionalHelp", "Optional. Leave blank to use default salt: vdo.ninja"));
	obs_property_t *iceServers = obs_properties_add_text(
	    advanced, "custom_ice_servers", tr("CustomICEServers", "Custom STUN/TURN Servers"), OBS_TEXT_DEFAULT);
	obs_property_text_set_monospace(iceServers, true);
	obs_property_set_long_description(
	    iceServers,
	    tr("CustomICEServers.Help",
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
	obs_property_t *protection = obs_properties_add_list(advanced, "video_protection_mode",
	                                                     tr("VideoProtection", "Packet Duplication (Experimental)"),
	                                                     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(protection, tr("VideoProtection.Off", "Off"), static_cast<int>(VideoProtectionMode::Off));
	obs_property_list_add_int(protection, tr("VideoProtection.Low", "Low — keyframes, up to 20% extra (best effort)"),
	                          static_cast<int>(VideoProtectionMode::Low));
	obs_property_list_add_int(
	    protection, tr("VideoProtection.Medium", "Medium — keyframes + 25% deltas, up to 50% extra (best effort)"),
	    static_cast<int>(VideoProtectionMode::Medium));
	obs_property_list_add_int(protection,
	                          tr("VideoProtection.High", "High — all packets, up to 100% extra (best effort)"),
	                          static_cast<int>(VideoProtectionMode::High));
	obs_property_set_long_description(
	    protection,
	    tr("VideoProtection.Description",
	       "Opt-in paced copies of RTP packets, delayed so both copies are less likely to share one loss burst. "
	       "Off is the compatibility default. This is packet duplication, not negotiated RTP RED or FEC, and it "
	       "uses additional upload bandwidth. Copies yield to live media and can expire rather than delay the "
	       "stream."));
	obs_property_t *audioRed =
	    obs_properties_add_bool(advanced, "audio_red", tr("AudioRed", "Audio RED (Experimental)"));
	obs_property_set_long_description(
	    audioRed,
	    tr("AudioRed.Description",
	       "Opt in to negotiated RFC 2198 audio redundancy. Compatible viewers receive the current and previous "
	       "Opus frame in one audio packet; other viewers fall back to plain Opus. This adds audio bandwidth and "
	       "remains off by default."));
	obs_property_t *adaptiveBitrate = obs_properties_add_bool(
	    advanced, "adaptive_bitrate", tr("AdaptiveBitrate", "Adaptive Bitrate from REMB (Experimental)"));
	obs_property_set_long_description(
	    adaptiveBitrate,
	    tr("AdaptiveBitrate.Description",
	       "Opt in to conservative browser-feedback adaptation. The lowest fresh REMB estimate across all viewers "
	       "controls the OBS encoder and RTP pacer. Unsupported encoders fail closed, and the original bitrate is "
	       "restored when streaming stops."));
	obs_properties_add_int(advanced, "adaptive_bitrate_min",
	                       tr("AdaptiveBitrate.Minimum", "Minimum Adaptive Bitrate (kbps)"), 100, 10000, 100);
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
	obs_data_set_default_int(settings, "video_protection_mode", static_cast<int>(VideoProtectionMode::Off));
	obs_data_set_default_bool(settings, "audio_red", false);
	obs_data_set_default_bool(settings, "adaptive_bitrate", false);
	obs_data_set_default_int(settings, "adaptive_bitrate_min", 500);
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
	return runNoexceptCallbackValue<uint64_t>("vdoninja_output_total_bytes", 0, [data]() {
		auto *vdo = static_cast<VDONinjaOutput *>(data);
		return vdo->getTotalBytes();
	});
}

static int vdoninja_output_connect_time(void *data)
{
	return runNoexceptCallbackValue<int>("vdoninja_output_connect_time", 0, [data]() {
		auto *vdo = static_cast<VDONinjaOutput *>(data);
		return vdo->getConnectTime();
	});
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
	callbackState_ = std::make_shared<AsyncCallbackState<VDONinjaOutput>>();
	callbackState_->owner.store(this, std::memory_order_release);

	logInfo("VDO.Ninja output created");
}

VDONinjaOutput::~VDONinjaOutput()
{
	stop();
	drainAsyncCallbacks();
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
		// Prefer output settings first; they are the source-of-truth snapshot used
		// to start this output. Some OBS service fields (notably password-like
		// fields) can be omitted/redacted when read back from service settings.
		std::string value;
		if (settings && (obs_data_has_user_value(settings, key) || !serviceSettings)) {
			const char *raw = obs_data_get_string(settings, key);
			if (raw) {
				value = raw;
			}
		}

		if (value.empty() && serviceSettings && obs_data_has_user_value(serviceSettings, key)) {
			const char *raw = obs_data_get_string(serviceSettings, key);
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

		if (value.empty() && settings) {
			const char *raw = obs_data_get_string(settings, key);
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
	if (settings_.password.empty()) {
		logInfo("No explicit password configured; using default VDO.Ninja password hashing");
	} else if (isPasswordDisabledToken(settings_.password)) {
		logInfo("Explicit password disable token detected; publishing without stream hashing/encryption");
	} else {
		logInfo("Explicit password configured; using custom VDO.Ninja hashing/encryption");
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
	settings_.videoProtectionMode =
	    videoProtectionModeFromInt(getIntSetting("video_protection_mode", static_cast<int>(VideoProtectionMode::Off)));
	settings_.enableAudioRed = getBoolSetting("audio_red", false);
	settings_.enableAdaptiveBitrate = getBoolSetting("adaptive_bitrate", false);
	const int minimumAdaptiveKbps = std::clamp(getIntSetting("adaptive_bitrate_min", 500), 100, 10000);
	settings_.minimumAdaptiveBitrate = minimumAdaptiveKbps * 1000;
	settings_.enableRemote = false;

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
	settings_.autoInbound.wssHost = settings_.wssHost;
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
	info.add("video_muted_init", false);
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
		if (i > 0)
			scenesArray += ",";
		// JSON-escape the scene name
		std::string nameStr = name ? name : "";
		std::string escaped = "\"";
		for (char c : nameStr) {
			if (c == '"')
				escaped += "\\\"";
			else if (c == '\\')
				escaped += "\\\\";
			else
				escaped += c;
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

std::string VDONinjaOutput::buildRemoteStatsMessage(const std::string &requestingUuid) const
{
	JsonBuilder remoteStats;
	const int64_t now = currentTimeMs();

	for (const ViewerRuntimeSnapshot &snapshot : getViewerSnapshots()) {
		if (snapshot.uuid.empty() || snapshot.uuid == requestingUuid) {
			continue;
		}

		JsonBuilder peerStats;
		peerStats.add("role", snapshot.role);
		peerStats.add("state", snapshot.state);
		peerStats.add("hasDataChannel", snapshot.hasDataChannel);
		if (!snapshot.streamId.empty()) {
			peerStats.add("streamID", snapshot.streamId);
		}

		if (snapshot.lastStatsTimestampMs > 0) {
			peerStats.add("lastStatsAgeMs", now - snapshot.lastStatsTimestampMs);
			if (looksLikeJsonContainer(snapshot.lastStats)) {
				peerStats.addRaw("reported", snapshot.lastStats);
			} else if (!snapshot.lastStats.empty()) {
				peerStats.add("reported", snapshot.lastStats);
			}
		}

		remoteStats.addRaw(snapshot.uuid, peerStats.build());
	}

	JsonBuilder message;
	message.addRaw("remoteStats", remoteStats.build());
	return message.build();
}

std::string VDONinjaOutput::buildConnectionMapMessage(const std::string &requestingUuid) const
{
	OutputSettings snap;
	{
		std::lock_guard<std::mutex> lock(settingsMutex_);
		snap = settings_;
	}

	const std::string localUuid = signaling_ ? signaling_->getLocalUUID() : snap.streamId;
	const std::string label = snap.streamId.empty() ? "OBS Publisher" : snap.streamId;
	std::string connections = "[";
	bool firstConnection = true;
	for (const ViewerRuntimeSnapshot &viewer : getViewerSnapshots()) {
		if (viewer.uuid.empty()) {
			continue;
		}
		if (!firstConnection) {
			connections += ",";
		}
		firstConnection = false;

		JsonBuilder connection;
		connection.add("peerUUID", viewer.uuid);
		connection.add("peerStreamID", viewer.streamId.empty() ? viewer.uuid : viewer.streamId);
		connection.add("direction", "outgoing");
		connection.add("state", viewer.state);
		connection.add("bandwidth", -1);
		connection.add("audioEnabled", viewer.audioSendEnabled);
		connection.add("videoEnabled", viewer.videoSendEnabled);
		connection.add("nackCount", 0);
		connection.add("pliCount", 0);
		connection.add("candidateType", "unknown");
		connection.add("hasDataChannel", viewer.hasDataChannel);
		connections += connection.build();
	}
	connections += "]";

	JsonBuilder connectionMap;
	connectionMap.add("uuid", localUuid.empty() ? label : localUuid);
	connectionMap.add("streamID", snap.streamId);
	connectionMap.add("label", label);
	connectionMap.add("browser", "OBS VDO.Ninja Plugin");
	connectionMap.addRaw("connections", connections);
	connectionMap.add("requesterUUID", requestingUuid);

	JsonBuilder message;
	message.addRaw("connectionMap", connectionMap.build());
	return message.build();
}

void VDONinjaOutput::sendRemoteStatsSnapshotToPeer(const std::string &uuid)
{
	if (!peerManager_ || uuid.empty()) {
		return;
	}
	peerManager_->sendDataToPeer(uuid, buildRemoteStatsMessage(uuid));
}

void VDONinjaOutput::sendRejectedControlToPeer(const std::string &uuid, const std::string &controlName)
{
	if (!peerManager_ || uuid.empty() || controlName.empty()) {
		return;
	}

	JsonBuilder message;
	message.add("rejected", controlName);
	peerManager_->sendDataToPeer(uuid, message.build());
}

void VDONinjaOutput::addRemoteStatsSubscriber(const std::string &uuid)
{
	if (uuid.empty()) {
		return;
	}

	bool shouldStartWorker = false;
	{
		std::lock_guard<std::mutex> lock(remoteStatsMutex_);
		remoteStatsSubscribers_.insert(uuid);
		if (!remoteStatsWorkerRunning_) {
			remoteStatsWorkerRunning_ = true;
			shouldStartWorker = true;
		}
	}

	if (shouldStartWorker) {
		if (remoteStatsThread_.joinable()) {
			remoteStatsThread_.join();
		}
		remoteStatsThread_ = std::thread(&VDONinjaOutput::remoteStatsThread, this);
	}
	remoteStatsCv_.notify_all();
}

void VDONinjaOutput::removeRemoteStatsSubscriber(const std::string &uuid)
{
	if (uuid.empty()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(remoteStatsMutex_);
		remoteStatsSubscribers_.erase(uuid);
	}
	remoteStatsCv_.notify_all();
}

void VDONinjaOutput::stopRemoteStatsWorker()
{
	{
		std::lock_guard<std::mutex> lock(remoteStatsMutex_);
		remoteStatsWorkerRunning_ = false;
		remoteStatsSubscribers_.clear();
	}
	remoteStatsCv_.notify_all();

	if (remoteStatsThread_.joinable()) {
		remoteStatsThread_.join();
	}
}

void VDONinjaOutput::remoteStatsThread()
{
	std::unique_lock<std::mutex> lock(remoteStatsMutex_);
	while (remoteStatsWorkerRunning_) {
		if (remoteStatsSubscribers_.empty()) {
			remoteStatsCv_.wait(lock,
			                    [this]() { return !remoteStatsWorkerRunning_ || !remoteStatsSubscribers_.empty(); });
			continue;
		}

		const bool wokeForStateChange =
		    remoteStatsCv_.wait_for(lock, std::chrono::milliseconds(kRemoteStatsIntervalMs),
		                            [this]() { return !remoteStatsWorkerRunning_ || remoteStatsSubscribers_.empty(); });
		if (wokeForStateChange) {
			continue;
		}

		std::vector<std::string> subscribers(remoteStatsSubscribers_.begin(), remoteStatsSubscribers_.end());
		lock.unlock();
		for (const std::string &uuid : subscribers) {
			sendRemoteStatsSnapshotToPeer(uuid);
		}
		lock.lock();
	}
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
		std::shared_ptr<AsyncCallbackState<VDONinjaOutput>> callbackState;
		std::string uuid;
	};

	// Never wait on the UI thread here: this runs on RTC callback threads, and
	// stop()/teardown runs on the UI thread while resetting those callbacks, so a
	// blocking queue round-trip deadlocks the whole frontend.
	auto *task = new ObsStateTaskData{callbackState_, uuid};
	obs_queue_task(
	    OBS_TASK_UI,
	    [](void *param) {
		    auto *data = static_cast<ObsStateTaskData *>(param);
		    if (!data) {
			    return;
		    }
		    {
			    AsyncCallbackGuard<VDONinjaOutput> guard(data->callbackState.get());
			    if (guard) {
				    guard.owner()->sendObsStateToPeer(data->uuid);
			    }
		    }
		    delete data;
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

	// Only viewers still waiting on their first keyframe. The cached IDR provides
	// an immediate still image, but the peer gate keeps live deltas suppressed
	// until a complete live IDR establishes the current prediction chain.
	if (peerManager_->sendVideoFrameToPeer(uuid, keyframeCopy.data(), keyframeCopy.size(), keyframeTimestamp, true,
	                                       true)) {
		keyframeRequestsPrimed_.fetch_add(1, std::memory_order_relaxed);
		logInfo("Primed viewer %s with cached keyframe (%zu bytes)", uuid.c_str(), keyframeCopy.size());
	}
}

void VDONinjaOutput::noteKeyframeRequest(const std::string &uuid, const char *transport)
{
	keyframeRequests_.fetch_add(1, std::memory_order_relaxed);

	// A viewer on a lossy link asks about once a second for as long as the loss
	// lasts, so logging every request buries the rest of the log. Record the first
	// one for its timestamp and peer id; the rest are counted into the summary.
	if (!loggedFirstKeyframeRequest_.exchange(true, std::memory_order_relaxed)) {
		logInfo("Viewer %s requested a keyframe over %s; further requests are counted in the publish summary",
		        uuid.c_str(), transport);
	}
}

void VDONinjaOutput::startPublishSummaryWorker()
{
	stopPublishSummaryWorker(false);

	{
		std::lock_guard<std::mutex> lock(publishSummaryMutex_);
		publishSummaryWorkerRunning_ = true;
	}
	publishSummaryThread_ = std::thread(&VDONinjaOutput::publishSummaryThread, this);
}

void VDONinjaOutput::stopPublishSummaryWorker(bool flush)
{
	{
		std::lock_guard<std::mutex> lock(publishSummaryMutex_);
		publishSummaryWorkerRunning_ = false;
	}
	publishSummaryCv_.notify_all();

	if (publishSummaryThread_.joinable()) {
		publishSummaryThread_.join();
	}

	if (flush) {
		maybeLogPublishSummary(true);
	}
}

void VDONinjaOutput::publishSummaryThread()
{
	std::unique_lock<std::mutex> lock(publishSummaryMutex_);
	while (publishSummaryWorkerRunning_) {
		if (lastPublishSummaryMs_ == 0) {
			publishSummaryCv_.wait(lock,
			                       [this]() { return !publishSummaryWorkerRunning_ || lastPublishSummaryMs_ != 0; });
			continue;
		}

		const int64_t elapsedMs = steadyTimeMs() - lastPublishSummaryMs_;
		const int64_t summaryWaitMs = std::max<int64_t>(1, kPublishSummaryIntervalMs - elapsedMs);
		const int64_t waitMs = std::min<int64_t>(kBitrateAdaptationIntervalMs, summaryWaitMs);
		const bool stopping = publishSummaryCv_.wait_for(lock, std::chrono::milliseconds(waitMs),
		                                                 [this]() { return !publishSummaryWorkerRunning_; });
		if (stopping) {
			break;
		}

		lock.unlock();
		maybeAdaptBitrate();
		maybeLogPublishSummary();
		lock.lock();
	}
}

void VDONinjaOutput::configureBitrateAdaptation(const OutputSettings &settings, int encoderBitrateBitsPerSecond)
{
	adaptiveBitrateEnabled_ = false;
	bitrateController_.reset();
	originalEncoderBitrate_ = std::max(encoderBitrateBitsPerSecond, 1);
	currentEncoderBitrate_ = originalEncoderBitrate_;
	pendingPacerBitrate_ = 0;
	pendingPacerBitrateDueMs_ = 0;

	if (!settings.enableAdaptiveBitrate) {
		return;
	}

	obs_encoder_t *encoder = output_ ? obs_output_get_video_encoder(output_) : nullptr;
	if (!encoder) {
		logWarning("Adaptive bitrate requested, but no active OBS video encoder was available; leaving it disabled");
		return;
	}
	if ((obs_encoder_get_caps(encoder) & OBS_ENCODER_CAP_DYN_BITRATE) == 0) {
		logWarning("Adaptive bitrate requested, but encoder '%s' does not support dynamic bitrate; leaving it disabled",
		           obs_encoder_get_id(encoder));
		return;
	}

	BitrateControllerConfig controllerConfig;
	controllerConfig.maximumBitrateBitsPerSecond = static_cast<uint64_t>(originalEncoderBitrate_);
	controllerConfig.minimumBitrateBitsPerSecond =
	    static_cast<uint64_t>(std::min(originalEncoderBitrate_, std::max(settings.minimumAdaptiveBitrate, 100000)));
	bitrateController_ = std::make_unique<BitrateController>(controllerConfig);
	adaptiveBitrateEnabled_ = true;
	const char *encoderId = obs_encoder_get_id(encoder);
	logInfo("Adaptive bitrate enabled for encoder '%s': %d-%d kbps, minimum fresh REMB across all viewers",
	        encoderId ? encoderId : "(unknown)", static_cast<int>(controllerConfig.minimumBitrateBitsPerSecond / 1000U),
	        originalEncoderBitrate_ / 1000);
}

void VDONinjaOutput::configureH264ProfileLevelId()
{
	obs_encoder_t *encoder = output_ ? obs_output_get_video_encoder(output_) : nullptr;
	std::optional<std::string> profileLevelId;
	if (encoder) {
		uint8_t *extraData = nullptr;
		size_t extraDataSize = 0;
		if (obs_encoder_get_extra_data(encoder, &extraData, &extraDataSize) && extraData && extraDataSize > 0) {
			profileLevelId = deriveH264ProfileLevelId(extraData, extraDataSize);
		}
	}

	bool usedFallback = false;
	if (!profileLevelId) {
		uint32_t width = encoder ? obs_encoder_get_width(encoder) : 0;
		uint32_t height = encoder ? obs_encoder_get_height(encoder) : 0;
		obs_video_info videoInfo = {};
		const bool hasVideoInfo = obs_get_video_info(&videoInfo);
		if (width == 0 && hasVideoInfo) {
			width = videoInfo.output_width;
		}
		if (height == 0 && hasVideoInfo) {
			height = videoInfo.output_height;
		}
		const uint32_t fpsNumerator = hasVideoInfo ? videoInfo.fps_num : 30;
		const uint32_t fpsDenominator = hasVideoInfo ? videoInfo.fps_den : 1;
		profileLevelId = fallbackH264ProfileLevelId(width, height, fpsNumerator, fpsDenominator);
		usedFallback = true;
	}

	{
		std::lock_guard<std::mutex> lock(h264ProfileMutex_);
		h264ProfileLevelId_ = *profileLevelId;
	}
	peerManager_->setH264ProfileLevelId(*profileLevelId);
	logInfo("H.264 encoder profile-level-id=%s (%s); SDP uses the WebRTC compatibility profile",
	        profileLevelId->c_str(),
	        usedFallback ? "resolution/FPS fallback until SPS is available" : "derived from encoder SPS");
}

void VDONinjaOutput::maybeAdaptBitrate()
{
	if (!adaptiveBitrateEnabled_ || !bitrateController_ || !running_ || !peerManager_) {
		return;
	}

	maybeSettleAdaptivePacer();
	const std::optional<uint64_t> estimate = peerManager_->minimumRecentRembBitrate(kRecentRembMaximumAge);
	const std::optional<uint64_t> target = bitrateController_->observe(estimate);
	if (!target || *target == 0 || *target > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
		return;
	}
	applyAdaptiveBitrate(*target, estimate.value_or(0));
}

void VDONinjaOutput::maybeSettleAdaptivePacer()
{
	if (pendingPacerBitrate_ <= 0 || pendingPacerBitrateDueMs_ <= 0 || steadyTimeMs() < pendingPacerBitrateDueMs_ ||
	    !peerManager_) {
		return;
	}

	const int settledBitrate = pendingPacerBitrate_;
	pendingPacerBitrate_ = 0;
	pendingPacerBitrateDueMs_ = 0;
	peerManager_->setBitrate(settledBitrate);
	logInfo("Adaptive bitrate settled RTP pacers to %d kbps after the encoder drain interval", settledBitrate / 1000);
}

void VDONinjaOutput::applyAdaptiveBitrate(uint64_t bitrateBitsPerSecond, uint64_t estimateBitsPerSecond)
{
	obs_encoder_t *encoder = output_ ? obs_output_get_video_encoder(output_) : nullptr;
	if (!encoder || (obs_encoder_get_caps(encoder) & OBS_ENCODER_CAP_DYN_BITRATE) == 0) {
		if (pendingPacerBitrate_ > 0 && peerManager_) {
			peerManager_->setBitrate(pendingPacerBitrate_);
		}
		pendingPacerBitrate_ = 0;
		pendingPacerBitrateDueMs_ = 0;
		adaptiveBitrateEnabled_ = false;
		logWarning("Adaptive bitrate stopped because the active encoder no longer supports dynamic updates");
		return;
	}

	const int targetKbps =
	    std::max(1, static_cast<int>(std::min<uint64_t>(bitrateBitsPerSecond / 1000U,
	                                                    static_cast<uint64_t>(std::numeric_limits<int>::max()))));
	const int targetBitsPerSecond = targetKbps * 1000;
	if (targetBitsPerSecond == currentEncoderBitrate_) {
		return;
	}

	const bool decreasing = targetBitsPerSecond < currentEncoderBitrate_;
	if (!decreasing) {
		// Give the scheduler enough capacity before the encoder begins
		// producing at the higher rate.
		pendingPacerBitrate_ = 0;
		pendingPacerBitrateDueMs_ = 0;
		peerManager_->setBitrate(targetBitsPerSecond);
	}

	obs_data_t *update = obs_data_create();
	obs_data_set_int(update, "bitrate", targetKbps);
	obs_encoder_update(encoder, update);
	obs_data_release(update);
	currentEncoderBitrate_ = targetBitsPerSecond;
	if (decreasing) {
		// Dynamic encoders can emit pre-change frames for a short time. Keep
		// the previous pacer rate long enough to drain those frames instead of
		// converting the encoder transition into seconds of queued latency.
		pendingPacerBitrate_ = targetBitsPerSecond;
		pendingPacerBitrateDueMs_ = steadyTimeMs() + kAdaptivePacerSettleDelayMs;
		logInfo(
		    "Adaptive bitrate changed OBS encoder to %d kbps (minimum REMB %llu kbps); RTP pacers will settle after "
		    "the drain interval",
		    targetKbps, static_cast<unsigned long long>(estimateBitsPerSecond / 1000U));
	} else {
		logInfo("Adaptive bitrate changed OBS encoder and RTP pacers to %d kbps (minimum REMB %llu kbps)", targetKbps,
		        static_cast<unsigned long long>(estimateBitsPerSecond / 1000U));
	}
}

void VDONinjaOutput::restoreEncoderBitrate()
{
	const bool shouldRestore =
	    originalEncoderBitrate_ > 0 && currentEncoderBitrate_ > 0 && currentEncoderBitrate_ != originalEncoderBitrate_;
	adaptiveBitrateEnabled_ = false;
	bitrateController_.reset();
	pendingPacerBitrate_ = 0;
	pendingPacerBitrateDueMs_ = 0;
	if (!shouldRestore) {
		originalEncoderBitrate_ = 0;
		currentEncoderBitrate_ = 0;
		return;
	}

	obs_encoder_t *encoder = output_ ? obs_output_get_video_encoder(output_) : nullptr;
	if (encoder) {
		obs_data_t *update = obs_data_create();
		obs_data_set_int(update, "bitrate", originalEncoderBitrate_ / 1000);
		obs_encoder_update(encoder, update);
		obs_data_release(update);
		logInfo("Restored OBS encoder bitrate to %d kbps after adaptive session", originalEncoderBitrate_ / 1000);
	}
	originalEncoderBitrate_ = 0;
	currentEncoderBitrate_ = 0;
}

void VDONinjaOutput::resetPublishTelemetry()
{
	lastKeyframeWallClockMs_ = 0;
	longKeyframeGaps_ = 0;
	loggedKeyframeIntervalWarning_ = false;
	{
		std::lock_guard<std::mutex> lock(publishSummaryMutex_);
		lastPublishSummaryMs_ = 0;
		summaryVideoFrames_ = 0;
		summaryVideoBytes_ = 0;
		summaryKeyframes_ = 0;
		summaryKeyframeBytes_ = 0;
		summaryMaxKeyframeBytes_ = 0;
		summaryAudioBytes_ = 0;
		audioTimestampSteps_.reset();
	}
	maxMediaQueueDepth_.store(0, std::memory_order_relaxed);
	maxAudioQueueDelayMs_.store(0, std::memory_order_relaxed);
	keyframeRequests_.store(0, std::memory_order_relaxed);
	keyframeRequestsPrimed_.store(0, std::memory_order_relaxed);
	loggedFirstKeyframeRequest_.store(false, std::memory_order_relaxed);
}

void VDONinjaOutput::maybeLogPublishSummary(bool force)
{
	const int64_t nowMs = steadyTimeMs();
	int64_t elapsedMs = 0;
	uint64_t videoFrames = 0;
	uint64_t videoBytes = 0;
	uint64_t keyframes = 0;
	uint64_t keyframeBytes = 0;
	uint64_t maxKeyframeBytes = 0;
	uint64_t audioBytes = 0;
	RtpTimestampStepStats audioTimestampStats;
	{
		std::lock_guard<std::mutex> lock(publishSummaryMutex_);
		if (lastPublishSummaryMs_ == 0) {
			lastPublishSummaryMs_ = nowMs;
			return;
		}

		elapsedMs = nowMs - lastPublishSummaryMs_;
		if (elapsedMs <= 0 || (!force && elapsedMs < kPublishSummaryIntervalMs)) {
			return;
		}
		lastPublishSummaryMs_ = nowMs;

		// A row of zeroes says nothing that the surrounding stop/start lines do
		// not. Reset the empty interval so a later rate never spans two windows.
		if (summaryVideoFrames_ == 0) {
			summaryAudioBytes_ = 0;
			audioTimestampSteps_.takeInterval();
			return;
		}

		videoFrames = summaryVideoFrames_;
		videoBytes = summaryVideoBytes_;
		keyframes = summaryKeyframes_;
		keyframeBytes = summaryKeyframeBytes_;
		maxKeyframeBytes = summaryMaxKeyframeBytes_;
		audioBytes = summaryAudioBytes_;
		audioTimestampStats = audioTimestampSteps_.takeInterval();

		summaryVideoFrames_ = 0;
		summaryVideoBytes_ = 0;
		summaryKeyframes_ = 0;
		summaryKeyframeBytes_ = 0;
		summaryMaxKeyframeBytes_ = 0;
		summaryAudioBytes_ = 0;
	}

	const double seconds = static_cast<double>(elapsedMs) / 1000.0;
	const double fps = static_cast<double>(videoFrames) / seconds;
	const double videoKbps = static_cast<double>(videoBytes) * 8.0 / seconds / 1000.0;
	const double audioKbps = static_cast<double>(audioBytes) * 8.0 / seconds / 1000.0;

	double keyframeIntervalSec = 0.0;
	double avgKeyframeKb = 0.0;
	if (keyframes > 0) {
		keyframeIntervalSec = seconds / static_cast<double>(keyframes);
		avgKeyframeKb = static_cast<double>(keyframeBytes) / static_cast<double>(keyframes) / 1024.0;
	}

	// Track encoded keyframe size separately from the paced RTP burst so logs can
	// distinguish encoder output from network egress behavior.
	const double avgFrameBytes =
	    videoFrames > 0 ? static_cast<double>(videoBytes) / static_cast<double>(videoFrames) : 0.0;
	const double burstRatio = avgFrameBytes > 0.0 ? static_cast<double>(maxKeyframeBytes) / avgFrameBytes : 0.0;

	size_t queueDepth = 0;
	{
		std::lock_guard<std::mutex> lock(mediaSendMutex_);
		queueDepth = mediaSendQueue_.size();
	}
	const uint64_t maxQueueDepth =
	    std::max<uint64_t>(queueDepth, maxMediaQueueDepth_.exchange(0, std::memory_order_relaxed));
	const uint64_t maxAudioQueueDelayMs = maxAudioQueueDelayMs_.exchange(0, std::memory_order_relaxed);

	const uint64_t requests = keyframeRequests_.exchange(0, std::memory_order_relaxed);
	const uint64_t primed = keyframeRequestsPrimed_.exchange(0, std::memory_order_relaxed);
	const RtcpFeedbackStats feedbackStats = peerManager_ ? peerManager_->takeVideoFeedbackStats() : RtcpFeedbackStats{};
	const RtpPacerStats pacerStats = peerManager_ ? peerManager_->takeVideoPacerStats() : RtpPacerStats{};
	const RtpSendStats audioSendStats = peerManager_ ? peerManager_->takeAudioSendStats() : RtpSendStats{};
	const AudioRedStats audioRedStats = peerManager_ ? peerManager_->takeAudioRedStats() : AudioRedStats{};
	const double maxAudioTimestampStepMs = static_cast<double>(audioTimestampStats.maxForwardStep) * 1000.0 / 48000.0;
	const double maxReceiverLossPercent = static_cast<double>(feedbackStats.maxFractionLost) * 100.0 / 256.0;
	const double maxReceiverJitterMs = static_cast<double>(feedbackStats.maxJitterTicks) * 1000.0 / 90000.0;

	logInfo(
	    "Publish: %.1f fps, %.0f kbps video, %.0f kbps audio, keyframe every %.1fs (avg %.0f KB, max %.0f KB, "
	    "%.0fx avg frame), %d viewers, queue %zu, dropped %llu, keyframe requests %llu (%llu primed), "
	    "RTCP NACK %llu msgs/%llu packets, PLI %llu, FIR %llu, RR %llu, loss max %.1f%%, RTT max %llu ms, "
	    "jitter max %.1f ms, REMB %llu (min %llu/max %llu kbps), malformed %llu, NACK cache %llu hit/%llu miss, "
	    "repair %llu queued/%llu sent/%llu dropped/%llu expired/%llu failed, "
	    "duplicate %llu queued/%llu sent (%.0f KB)/%llu dropped (%llu expired)/%llu failed, "
	    "pacer max batch %.0f KB, queued %.0f KB (max %.0f KB), delay %llu ms, dropped %llu, send errors %llu, "
	    "frames %llu (keyframes %llu, failed %llu), send max %llu ms (keyframe %llu ms), "
	    "audio packets %llu, RTP max step %.1f ms (large %llu, non-forward %llu), queue max %llu, delay %llu ms, "
	    "dropped %llu, sent %llu, send errors %llu, audio RED %llu packets (%llu redundant/%llu primary-only, "
	    "%.0f KB redundant)",
	    fps, videoKbps, audioKbps, keyframeIntervalSec, avgKeyframeKb, static_cast<double>(maxKeyframeBytes) / 1024.0,
	    burstRatio, peerManager_ ? peerManager_->getViewerCount() : 0, queueDepth,
	    static_cast<unsigned long long>(droppedMediaFrames_.load(std::memory_order_relaxed)),
	    static_cast<unsigned long long>(requests), static_cast<unsigned long long>(primed),
	    static_cast<unsigned long long>(feedbackStats.nackMessages),
	    static_cast<unsigned long long>(feedbackStats.nackRequestedPackets),
	    static_cast<unsigned long long>(feedbackStats.pliMessages),
	    static_cast<unsigned long long>(feedbackStats.firMessages),
	    static_cast<unsigned long long>(feedbackStats.receiverReports), maxReceiverLossPercent,
	    static_cast<unsigned long long>(feedbackStats.maxRttMs), maxReceiverJitterMs,
	    static_cast<unsigned long long>(feedbackStats.rembMessages),
	    static_cast<unsigned long long>(feedbackStats.minRembBitrateBps / 1000U),
	    static_cast<unsigned long long>(feedbackStats.maxRembBitrateBps / 1000U),
	    static_cast<unsigned long long>(feedbackStats.malformedPackets),
	    static_cast<unsigned long long>(feedbackStats.nackCacheHits),
	    static_cast<unsigned long long>(feedbackStats.nackCacheMisses),
	    static_cast<unsigned long long>(feedbackStats.retransmissionsQueued),
	    static_cast<unsigned long long>(feedbackStats.retransmissionsSent),
	    static_cast<unsigned long long>(feedbackStats.retransmissionsDropped),
	    static_cast<unsigned long long>(feedbackStats.retransmissionsExpired),
	    static_cast<unsigned long long>(feedbackStats.retransmissionSendFailures),
	    static_cast<unsigned long long>(pacerStats.queuedDuplicates),
	    static_cast<unsigned long long>(pacerStats.sentDuplicates),
	    static_cast<double>(pacerStats.sentDuplicateBytes) / 1024.0,
	    static_cast<unsigned long long>(pacerStats.droppedDuplicates),
	    static_cast<unsigned long long>(pacerStats.expiredDuplicates),
	    static_cast<unsigned long long>(pacerStats.failedDuplicates),
	    static_cast<double>(pacerStats.maxBatchBytes) / 1024.0, static_cast<double>(pacerStats.queuedBytes) / 1024.0,
	    static_cast<double>(pacerStats.maxQueuedBytes) / 1024.0,
	    static_cast<unsigned long long>(pacerStats.maxPacketDelayMs),
	    static_cast<unsigned long long>(pacerStats.droppedFrames),
	    static_cast<unsigned long long>(pacerStats.sendFailures),
	    static_cast<unsigned long long>(pacerStats.sentFrames),
	    static_cast<unsigned long long>(pacerStats.sentKeyframes),
	    static_cast<unsigned long long>(pacerStats.failedFrames),
	    static_cast<unsigned long long>(pacerStats.maxFrameSendDurationMs),
	    static_cast<unsigned long long>(pacerStats.maxKeyframeSendDurationMs),
	    static_cast<unsigned long long>(audioTimestampStats.packets), maxAudioTimestampStepMs,
	    static_cast<unsigned long long>(audioTimestampStats.largeSteps),
	    static_cast<unsigned long long>(audioTimestampStats.nonForwardSteps),
	    static_cast<unsigned long long>(maxQueueDepth), static_cast<unsigned long long>(maxAudioQueueDelayMs),
	    static_cast<unsigned long long>(droppedAudioMediaFrames_.load(std::memory_order_relaxed)),
	    static_cast<unsigned long long>(audioSendStats.sentPackets),
	    static_cast<unsigned long long>(audioSendStats.sendFailures),
	    static_cast<unsigned long long>(audioRedStats.packets),
	    static_cast<unsigned long long>(audioRedStats.packetsWithRedundancy),
	    static_cast<unsigned long long>(audioRedStats.primaryOnlyPackets),
	    static_cast<double>(audioRedStats.redundantBytes) / 1024.0);
}

bool VDONinjaOutput::start()
{
	std::lock_guard<std::mutex> startStopLock(startStopMutex_);

	if (running_) {
		logWarning("Output already running");
		return false;
	}

	std::string streamIdSnapshot;
	{
		std::lock_guard<std::mutex> lock(settingsMutex_);
		streamIdSnapshot = settings_.streamId;
	}
	if (streamIdSnapshot.empty()) {
		logError("Stream ID is required");
		obs_output_set_last_error(output_, "Stream ID is required for VDO.Ninja publishing.");
		return false;
	}

	if (!obs_output_can_begin_data_capture(output_, 0)) {
		logError("Output cannot begin data capture");
		obs_output_set_last_error(output_, "Unable to begin OBS data capture for VDO.Ninja output.");
		return false;
	}

	std::string rebindError;
	if (!rebindOutputAudioEncodersToOpus(output_, rebindError)) {
		logError("Unable to enforce Opus audio encoders before start: %s", rebindError.c_str());
		obs_output_set_last_error(output_, rebindError.c_str());
		return false;
	}

	std::string nonOpusCodec;
	if (!validateOpusAudioEncoders(output_, nonOpusCodec)) {
		const std::string error = "VDO.Ninja requires Opus audio. "
		                          "Set the streaming audio encoder to Opus (Settings -> Output), then retry Go Live.";
		logError("Refusing to start: active audio encoder codec is '%s' (Opus required)", nonOpusCodec.c_str());
		obs_output_set_last_error(output_, error.c_str());
		return false;
	}

	if (!obs_output_initialize_encoders(output_, 0)) {
		logError("Failed to initialize output encoders");
		obs_output_set_last_error(output_, "Failed to initialize OBS encoders for VDO.Ninja output.");
		return false;
	}
	configureH264ProfileLevelId();

	selectedAudioTrackIdx_ = resolveOutputAudioTrackIndex(output_);
	droppedAudioPacketsOtherTracks_ = 0;
	logInfo("Publishing OBS audio track index %zu only", selectedAudioTrackIdx_);

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
	hasLastVideoRtpTimestamp_ = false;
	lastVideoRtpTimestamp_ = 0;
	hasLastAudioRtpTimestamp_ = false;
	lastAudioRtpTimestamp_ = 0;
	resetPublishTelemetry();
	droppedMediaFrames_ = 0;
	droppedAudioMediaFrames_ = 0;

	// Snapshot settings under lock for the start thread
	OutputSettings settingsSnap;
	{
		std::lock_guard<std::mutex> lock(settingsMutex_);
		settingsSnap = settings_;
	}
	const int configuredBitrate = settingsSnap.quality.bitrate;
	settingsSnap.quality.bitrate = resolveVideoEncoderBitrate(output_, configuredBitrate);
	if (settingsSnap.quality.bitrate != configuredBitrate) {
		logInfo("Using active video encoder bitrate %d kbps for RTP pacing (service setting: %d kbps)",
		        settingsSnap.quality.bitrate / 1000, configuredBitrate / 1000);
	}
	configureBitrateAdaptation(settingsSnap, settingsSnap.quality.bitrate);

	startMediaSendWorker();
	startPublishSummaryWorker();

	if (startStopThread_.joinable()) {
		startStopThread_.join();
	}

	startStopThread_ = std::thread(&VDONinjaOutput::startThread, this, settingsSnap);

	return true;
}

void VDONinjaOutput::startThread(OutputSettings settingsSnap)
{
	try {
		logInfo("Starting VDO.Ninja output...");
		const auto callbackState = callbackState_;

		// Initialize peer manager
		peerManager_->initialize(signaling_.get());
		peerManager_->setVideoCodec(settingsSnap.videoCodec);
		peerManager_->setAudioCodec(settingsSnap.audioCodec);
		peerManager_->setBitrate(settingsSnap.quality.bitrate);
		peerManager_->setVideoProtectionMode(settingsSnap.videoProtectionMode);
		peerManager_->setAudioRedEnabled(settingsSnap.enableAudioRed);
		peerManager_->setEnableDataChannel(settingsSnap.enableDataChannel);
		peerManager_->setIceServers(settingsSnap.customIceServers);
		peerManager_->setForceTurn(settingsSnap.forceTurn);
		signaling_->setSalt(settingsSnap.salt);

		if (autoSceneManager_) {
			autoSceneManager_->configure(settingsSnap.autoInbound);
			std::vector<std::string> ownIds = {
			    settingsSnap.streamId, hashStreamId(settingsSnap.streamId, settingsSnap.password, settingsSnap.salt),
			    hashStreamId(settingsSnap.streamId, DEFAULT_PASSWORD, settingsSnap.salt)};
			autoSceneManager_->setOwnStreamIds(ownIds);
			if (settingsSnap.autoInbound.enabled) {
				autoSceneManager_->start();
			}
		}

		// Set up callbacks
		signaling_->setOnConnected([callbackState, settingsSnap]() {
			AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaOutput *self = guard.owner();
			logInfo("Connected to signaling server");

			const std::string roomToJoin =
			    !settingsSnap.autoInbound.roomId.empty() ? settingsSnap.autoInbound.roomId : settingsSnap.roomId;
			const std::string roomPassword =
			    !settingsSnap.autoInbound.password.empty() ? settingsSnap.autoInbound.password : settingsSnap.password;

			// Join room for inbound orchestration and/or publishing presence.
			if (!roomToJoin.empty()) {
				self->signaling_->joinRoom(roomToJoin, roomPassword);
			}

			// Start publishing
			self->signaling_->publishStream(settingsSnap.streamId, settingsSnap.password);
			self->peerManager_->startPublishing(settingsSnap.maxViewers);

			self->connected_ = true;
			self->connectTimeMs_ = currentTimeMs() - self->startTimeMs_;

			if (!self->capturing_) {
				if (obs_output_begin_data_capture(self->output_, 0)) {
					self->capturing_ = true;
				} else {
					logError("Failed to begin OBS data capture");
					obs_output_signal_stop(self->output_, OBS_OUTPUT_ERROR);
					self->running_ = false;
					self->connected_ = false;
				}
			}
		});

		signaling_->setOnDisconnected([callbackState, settingsSnap]() {
			AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaOutput *self = guard.owner();
			logInfo("Disconnected from signaling server; existing viewer media remains active during reconnect");

			if (self->running_ && settingsSnap.autoReconnect) {
				logInfo("Will attempt to reconnect...");
			}
		});

		signaling_->setOnError([callbackState](const std::string &error) {
			AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaOutput *self = guard.owner();
			logError("Signaling error: %s", error.c_str());
			obs_output_set_last_error(self->output_, error.c_str());

			const bool streamIdConflict =
			    containsInsensitive(error, "already in use") || containsInsensitive(error, "already claimed");
			if (streamIdConflict && self->running_) {
				const std::string conflictMessage =
				    "Stream ID is already in use. Choose a different Stream ID, then retry Start Streaming.";
				obs_output_set_last_error(self->output_, conflictMessage.c_str());
				self->signaling_->setAutoReconnect(false, 0);
				logError("Stopping publish due to signaling conflict (stream/room already claimed)");
				obs_output_signal_stop(self->output_, OBS_OUTPUT_ERROR);
			}
		});

		signaling_->setOnRoomJoined([callbackState, settingsSnap](const std::vector<std::string> &members) {
			AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaOutput *self = guard.owner();
			if (self->autoSceneManager_ && settingsSnap.autoInbound.enabled) {
				self->autoSceneManager_->onRoomListing(members);
			}
		});

		signaling_->setOnStreamAdded([callbackState, settingsSnap](const std::string &streamId, const std::string &) {
			AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaOutput *self = guard.owner();
			if (self->autoSceneManager_ && settingsSnap.autoInbound.enabled) {
				self->autoSceneManager_->onStreamAdded(streamId);
			}
		});

		signaling_->setOnStreamRemoved([callbackState, settingsSnap](const std::string &streamId, const std::string &) {
			AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaOutput *self = guard.owner();
			if (self->autoSceneManager_ && settingsSnap.autoInbound.enabled) {
				self->autoSceneManager_->onStreamRemoved(streamId);
			}
		});

		peerManager_->setOnPeerConnected([callbackState](const PeerEventIdentity &identity) {
			AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaOutput *self = guard.owner();
			const std::string &uuid = identity.uuid;
			const auto currentIdentity = self->peerManager_->getPeerIdentity(uuid);
			if (!currentIdentity || currentIdentity->generation != identity.generation ||
			    currentIdentity->session != identity.session) {
				return;
			}
			logInfo("Viewer connected: %s (total: %d)", uuid.c_str(), self->peerManager_->getViewerCount());
			self->primeViewerWithCachedKeyframe(uuid);
		});

		peerManager_->setOnPeerDisconnected([callbackState](const PeerEventIdentity &identity) {
			AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaOutput *self = guard.owner();
			const std::string &uuid = identity.uuid;
			const auto currentIdentity = self->peerManager_->getPeerIdentity(uuid);
			if (currentIdentity &&
			    (currentIdentity->generation != identity.generation || currentIdentity->session != identity.session)) {
				return;
			}
			{
				std::lock_guard<std::mutex> lock(self->telemetryMutex_);
				self->lastPeerStats_.erase(uuid);
				self->lastPeerStatsTimestampMs_.erase(uuid);
			}
			self->removeRemoteStatsSubscriber(uuid);
			logInfo("Viewer disconnected: %s (total: %d)", uuid.c_str(), self->peerManager_->getViewerCount());
		});
		peerManager_->setOnKeyframeRequest([callbackState](const std::string &uuid) {
			AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			if (!guard) {
				return;
			}
			guard.owner()->noteKeyframeRequest(uuid, "RTCP");
			guard.owner()->primeViewerWithCachedKeyframe(uuid);
		});

		peerManager_->setOnDataChannel(
		    [callbackState](const PeerEventIdentity &identity, std::shared_ptr<rtc::DataChannel>) {
			    AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			    if (!guard) {
				    return;
			    }
			    VDONinjaOutput *self = guard.owner();
			    const auto currentIdentity = self->peerManager_->getPeerIdentity(identity.uuid);
			    if (!currentIdentity || currentIdentity->generation != identity.generation ||
			        currentIdentity->session != identity.session) {
				    return;
			    }
			    self->sendInitialPeerInfo(identity.uuid);
		    });

		peerManager_->setOnDataChannelMessage([callbackState, settingsSnap](const PeerEventIdentity &identity,
		                                                                    const std::string &message) {
			AsyncCallbackGuard<VDONinjaOutput> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaOutput *self = guard.owner();
			const std::string &uuid = identity.uuid;
			const auto currentIdentity = self->peerManager_->getPeerIdentity(uuid);
			if (!currentIdentity || currentIdentity->generation != identity.generation ||
			    currentIdentity->session != identity.session) {
				return;
			}
			self->dataChannel_.handleMessage(uuid, message);

			const DataMessage parsed = self->dataChannel_.parseMessage(message);
			if (parsed.type == DataMessageType::Signaling) {
				if (self->signaling_) {
					self->signaling_->processIncomingMessage(self->dataChannel_.prepareSignalingMessage(message, uuid));
				}
				return;
			}

			if (parsed.type == DataMessageType::Ping) {
				if (self->peerManager_) {
					self->peerManager_->sendDataToPeer(uuid, self->dataChannel_.createPongMessage(parsed.data));
				}
				return;
			}

			if (parsed.type == DataMessageType::IceRestartRequest) {
				if (self->peerManager_) {
					self->peerManager_->requestIceRestart(uuid);
				}
				return;
			}

			if (parsed.type == DataMessageType::MeshControl) {
				const MeshControlUpdate mesh = self->dataChannel_.parseMeshControl(message);
				if (mesh.hasReconnectPeer) {
					self->sendRejectedControlToPeer(uuid, "reconnectPeer");
				}
				if (mesh.hasGetConnectionMap) {
					self->sendRejectedControlToPeer(uuid, "getConnectionMap");
				}
				return;
			}

			if (self->dataChannel_.hasKeyframeRequest(message)) {
				self->noteKeyframeRequest(uuid, "data channel");
				self->peerManager_->notePeerKeyframeRequest(uuid);
				self->primeViewerWithCachedKeyframe(uuid);
			}

			if (parsed.type == DataMessageType::Stats) {
				std::lock_guard<std::mutex> lock(self->telemetryMutex_);
				self->lastPeerStats_[uuid] = parsed.data.empty() ? message : parsed.data;
				self->lastPeerStatsTimestampMs_[uuid] = currentTimeMs();
			}

			if (parsed.type == DataMessageType::StatsRequest && self->peerManager_) {
				if (parsed.statsRequestMode == StatsRequestMode::ContinuousStop) {
					self->removeRemoteStatsSubscriber(uuid);
				} else if (settingsSnap.enableRemote) {
					if (parsed.statsRequestMode == StatsRequestMode::ContinuousStart) {
						self->addRemoteStatsSubscriber(uuid);
					}
					self->sendRemoteStatsSnapshotToPeer(uuid);
				} else {
					self->removeRemoteStatsSubscriber(uuid);
					self->peerManager_->sendDataToPeer(uuid, R"({"remoteStats":{}})");
				}
			}

			const RecoveryControlUpdate recovery = self->dataChannel_.parseRecoveryControl(message);
			const bool hasRecoveryControl = recovery.hasRefreshVideo || recovery.hasRefreshMicrophone ||
			                                recovery.hasRefreshConnection || recovery.hasRefreshAll ||
			                                recovery.hasRestartWhip;
			if (hasRecoveryControl) {
				const bool refreshVideo = (recovery.hasRefreshVideo && recovery.refreshVideo) ||
				                          (recovery.hasRefreshAll && recovery.refreshAll);
				const bool refreshConnection = (recovery.hasRefreshConnection && recovery.refreshConnection) ||
				                               (recovery.hasRefreshAll && recovery.refreshAll);

				if (!settingsSnap.enableRemote || !self->peerManager_) {
					self->sendRejectedControlToPeer(uuid, self->dataChannel_.recoveryControlRejectionName(message));
					return;
				}

				if (refreshVideo) {
					logInfo("Viewer %s requested publisher video refresh over data channel", uuid.c_str());
					self->noteKeyframeRequest(uuid, "refreshVideo");
					self->peerManager_->notePeerKeyframeRequest(uuid);
					self->primeViewerWithCachedKeyframe(uuid);
				}
				if (refreshConnection) {
					const std::vector<PeerSnapshot> peerSnapshots = self->peerManager_->getPeerSnapshots();
					size_t requestedRestarts = 0;
					size_t eligiblePublisherPeers = 0;
					for (const PeerSnapshot &snapshot : peerSnapshots) {
						if (snapshot.type != ConnectionType::Publisher) {
							continue;
						}
						eligiblePublisherPeers++;
						if (self->peerManager_->requestIceRestart(snapshot.uuid)) {
							requestedRestarts++;
						}
					}
					logInfo("Viewer %s requested publisher-wide connection refresh over data channel; "
					        "started ICE repair for %zu/%zu peer(s)",
					        uuid.c_str(), requestedRestarts, eligiblePublisherPeers);
				}
				if (recovery.hasRefreshMicrophone && recovery.refreshMicrophone) {
					logInfo("Viewer %s requested microphone refresh, rejected by native OBS publisher output",
					        uuid.c_str());
					self->sendRejectedControlToPeer(uuid, "refreshMicrophone");
				}
				if (recovery.hasRestartWhip && recovery.restartWhip) {
					logInfo("Viewer %s requested WHIP restart, rejected by native OBS publisher output", uuid.c_str());
					self->sendRejectedControlToPeer(uuid, "restartWhip");
				}
			}

			if (self->peerManager_) {
				const MediaControlUpdate mediaControl = self->dataChannel_.parseMediaControl(message);
				if (mediaControl.hasVideoBitrate || mediaControl.hasAudioBitrate) {
					bool videoBecameEnabled = false;
					const bool videoEnabled = mediaControl.videoBitrateKbps != 0;
					const bool audioEnabled = mediaControl.audioBitrateKbps != 0;
					if (self->peerManager_->setPeerMediaSendEnabled(uuid, mediaControl.hasVideoBitrate, videoEnabled,
					                                                mediaControl.hasAudioBitrate, audioEnabled,
					                                                &videoBecameEnabled)) {
						if (mediaControl.hasVideoBitrate) {
							logInfo("Viewer %s requested video bitrate %d kbps; publisher video send is %s",
							        uuid.c_str(), mediaControl.videoBitrateKbps, videoEnabled ? "enabled" : "disabled");
						}
						if (mediaControl.hasAudioBitrate) {
							logInfo("Viewer %s requested audio bitrate %d kbps; publisher audio send is %s",
							        uuid.c_str(), mediaControl.audioBitrateKbps, audioEnabled ? "enabled" : "disabled");
						}
						if (videoBecameEnabled) {
							self->primeViewerWithCachedKeyframe(uuid);
						}
					}
				}
			}

			const std::string unsupportedControl = self->dataChannel_.unsupportedControlName(message);
			if (!unsupportedControl.empty()) {
				logInfo("Viewer %s requested unsupported VDO.Ninja control %s over data channel", uuid.c_str(),
				        unsupportedControl.c_str());
				self->sendRejectedControlToPeer(uuid, unsupportedControl);
				return;
			}

			if (parsed.type == DataMessageType::Hangup) {
				logInfo("Viewer %s requested output hangup over data channel; rejected without director identity",
				        uuid.c_str());
				self->sendRejectedControlToPeer(uuid, "hangup");
				return;
			}

			if (parsed.type == DataMessageType::PeerBye) {
				logInfo("Viewer %s sent data-channel bye; retiring peer", uuid.c_str());
				{
					std::lock_guard<std::mutex> lock(self->telemetryMutex_);
					self->lastPeerStats_.erase(uuid);
					self->lastPeerStatsTimestampMs_.erase(uuid);
				}
				self->removeRemoteStatsSubscriber(uuid);
				if (self->peerManager_) {
					self->peerManager_->disconnectPeer(identity);
				}
				return;
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
					self->queueObsStateToPeer(uuid);
				}
			}

			if (self->autoSceneManager_ && settingsSnap.autoInbound.enabled) {
				const std::string playbackHint = self->dataChannel_.extractInboundPlaybackHint(message);
				if (!playbackHint.empty()) {
					logInfo("Discovered inbound browser-source hint from %s", uuid.c_str());
					self->autoSceneManager_->onStreamAdded(playbackHint);
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
			obs_queue_task(
			    OBS_TASK_UI,
			    [](void *param) {
				    auto *cd = static_cast<ChatData *>(param);
				    vdo_dock_show_chat(cd->sender.c_str(), cd->message.c_str());
				    delete cd;
			    },
			    data, false);
		});

		// Set up remote control callback
		if (settingsSnap.enableRemote) {
			dataChannel_.setOnRemoteControl([](const std::string &action, const std::string &value) {
				struct RemoteData {
					std::string action;
					std::string value;
				};
				auto *data = new RemoteData{action, value};
				obs_queue_task(
				    OBS_TASK_UI,
				    [](void *param) {
					    auto *rd = static_cast<RemoteData *>(param);
					    vdo_handle_remote_control(rd->action.c_str(), rd->value.c_str());
					    delete rd;
				    },
				    data, false);
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
	} catch (const std::exception &e) {
		logError("VDO.Ninja output start thread crashed: %s", e.what());
		obs_output_set_last_error(output_, e.what());
		connected_ = false;
		running_ = false;
		obs_output_signal_stop(output_, OBS_OUTPUT_ERROR);
	} catch (...) {
		logError("VDO.Ninja output start thread crashed: unknown exception");
		obs_output_set_last_error(output_, "VDO.Ninja output thread crashed unexpectedly.");
		connected_ = false;
		running_ = false;
		obs_output_signal_stop(output_, OBS_OUTPUT_ERROR);
	}
}

void VDONinjaOutput::stop()
{
	std::lock_guard<std::mutex> startStopLock(startStopMutex_);

	const bool wasRunning = running_.exchange(false);
	const bool wasCapturing = capturing_.load();
	if (!wasRunning && !wasCapturing) {
		connected_ = false;
		stopPublishSummaryWorker(true);
		stopRemoteStatsWorker();
		stopMediaSendWorker();
		// Still join the thread in case startThread failed and set running_=false
		// but is still cleaning up.
		if (startStopThread_.joinable()) {
			startStopThread_.join();
		}
		restoreEncoderBitrate();
		return;
	}

	connected_ = false;

	logInfo("Stopping VDO.Ninja output...");

	stopPublishSummaryWorker(true);
	stopRemoteStatsWorker();
	stopMediaSendWorker();

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
	signaling_->setOnIceRestartRequest(nullptr);
	signaling_->setOnIceCandidate(nullptr);
	signaling_->setOnPeerCleanup(nullptr);
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
	restoreEncoderBitrate();
	{
		std::lock_guard<std::mutex> lock(keyframeCacheMutex_);
		cachedKeyframe_.clear();
		cachedKeyframeTimestamp_ = 0;
	}
	hasLastVideoRtpTimestamp_ = false;
	lastVideoRtpTimestamp_ = 0;
	hasLastAudioRtpTimestamp_ = false;
	lastAudioRtpTimestamp_ = 0;
	resetPublishTelemetry();

	logInfo("VDO.Ninja output stopped");
}

void VDONinjaOutput::startMediaSendWorker()
{
	stopMediaSendWorker();

	{
		std::lock_guard<std::mutex> lock(mediaSendMutex_);
		mediaSendQueue_.clear();
		mediaSendWorkerRunning_ = true;
	}
	mediaSendThread_ = std::thread(&VDONinjaOutput::mediaSendThread, this);
}

void VDONinjaOutput::stopMediaSendWorker()
{
	{
		std::lock_guard<std::mutex> lock(mediaSendMutex_);
		mediaSendWorkerRunning_ = false;
		mediaSendQueue_.clear();
	}
	mediaSendCv_.notify_all();

	if (mediaSendThread_.joinable()) {
		mediaSendThread_.join();
	}
}

void VDONinjaOutput::enqueueMediaFrame(QueuedMediaFrame frame)
{
	if (frame.payload.empty()) {
		return;
	}
	frame.queuedAtMs = static_cast<uint64_t>(steadyTimeMs());

	uint64_t dropped = 0;
	bool droppedVideo = false;
	{
		std::lock_guard<std::mutex> lock(mediaSendMutex_);
		if (!mediaSendWorkerRunning_) {
			return;
		}
		while (mediaSendQueue_.size() >= kMaxQueuedMediaFrames) {
			if (mediaSendQueue_.front().type == MediaFrameType::Audio) {
				droppedAudioMediaFrames_.fetch_add(1, std::memory_order_relaxed);
			} else {
				droppedVideo = true;
			}
			mediaSendQueue_.pop_front();
			dropped = ++droppedMediaFrames_;
		}
		if (droppedVideo && peerManager_) {
			// Close every peer's decode gate before the sender worker can
			// dequeue anything newer than the missing frame. This drop happens
			// before RTP sequence assignment, so the receiver cannot NACK it.
			peerManager_->requireLiveKeyframeForAll();
		}
		mediaSendQueue_.push_back(std::move(frame));
		updateAtomicMaximum(maxMediaQueueDepth_, static_cast<uint64_t>(mediaSendQueue_.size()));
	}

	if (dropped != 0 && (dropped == 1 || (dropped % 300) == 0)) {
		logWarning("Dropped VDO.Ninja media frame because send queue is saturated (dropped=%llu)",
		           static_cast<unsigned long long>(dropped));
	}

	mediaSendCv_.notify_one();
}

void VDONinjaOutput::mediaSendThread()
{
	for (;;) {
		QueuedMediaFrame frame;
		{
			std::unique_lock<std::mutex> lock(mediaSendMutex_);
			mediaSendCv_.wait(lock, [this]() { return !mediaSendQueue_.empty() || !mediaSendWorkerRunning_; });
			if (mediaSendQueue_.empty() && !mediaSendWorkerRunning_) {
				break;
			}
			frame = std::move(mediaSendQueue_.front());
			mediaSendQueue_.pop_front();
		}

		if (!peerManager_ || !connected_.load()) {
			continue;
		}

		try {
			if (frame.type == MediaFrameType::Video) {
				peerManager_->sendVideoFrame(frame.payload.data(), frame.payload.size(), frame.timestamp,
				                             frame.keyframe);
			} else {
				const uint64_t nowMs = static_cast<uint64_t>(steadyTimeMs());
				const uint64_t queueDelayMs = nowMs >= frame.queuedAtMs ? nowMs - frame.queuedAtMs : 0;
				updateAtomicMaximum(maxAudioQueueDelayMs_, queueDelayMs);
				peerManager_->sendAudioFrame(frame.payload.data(), frame.payload.size(), frame.timestamp);
			}
		} catch (const std::exception &e) {
			logError("VDO.Ninja media sender failed: %s", e.what());
		} catch (...) {
			logError("VDO.Ninja media sender failed with unknown exception");
		}
	}
}

void VDONinjaOutput::drainAsyncCallbacks()
{
	if (!callbackState_) {
		return;
	}

	AsyncCallbackGuard<VDONinjaOutput>::detach(callbackState_.get());
	if (!AsyncCallbackGuard<VDONinjaOutput>::waitForIdle(callbackState_.get(), 10000)) {
		logWarning("Timed out waiting for VDO.Ninja output callbacks to drain during teardown");
	}
	callbackState_.reset();
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

namespace
{

uint32_t timestampFromPacket(const encoder_packet *packet, double rtpClockRate)
{
	if (!packet || rtpClockRate <= 0.0) {
		return 0;
	}

	// OBS fills DTS in microseconds; this is the most stable source across
	// encoders and avoids ambiguity in encoder-specific timebase units.
	if (packet->dts_usec > 0) {
		const double dtsSeconds = static_cast<double>(packet->dts_usec) / 1000000.0;
		return static_cast<uint32_t>(std::llround(dtsSeconds * rtpClockRate));
	}

	if (packet->timebase_num > 0 && packet->timebase_den > 0) {
		const double ptsSeconds = (static_cast<double>(packet->pts) * static_cast<double>(packet->timebase_num)) /
		                          static_cast<double>(packet->timebase_den);
		return static_cast<uint32_t>(std::llround(ptsSeconds * rtpClockRate));
	}

	// Legacy fallback when timebase metadata is unavailable.
	if (rtpClockRate == 90000.0) {
		return static_cast<uint32_t>(packet->pts * 90);
	}
	if (rtpClockRate == 48000.0) {
		return static_cast<uint32_t>(packet->pts * 48);
	}
	return 0;
}

uint32_t sanitizeMonotonicTimestamp(uint32_t candidate, bool &hasLast, uint32_t &last, uint32_t fallbackStep)
{
	if (!hasLast) {
		hasLast = true;
		last = candidate;
		return candidate;
	}

	// Guard against non-monotonic or degenerate encoder timestamps.
	if (candidate <= last) {
		candidate = last + fallbackStep;
	}

	last = candidate;
	return candidate;
}

} // namespace

void VDONinjaOutput::processVideoPacket(encoder_packet *packet)
{
	if (!packet || !packet->data || packet->size == 0) {
		return;
	}

	bool keyframe = packet->keyframe;
	uint32_t timestamp = timestampFromPacket(packet, 90000.0);
	timestamp = sanitizeMonotonicTimestamp(timestamp, hasLastVideoRtpTimestamp_, lastVideoRtpTimestamp_, 3000);

	if (keyframe && packet->data && packet->size > 0) {
		std::lock_guard<std::mutex> lock(keyframeCacheMutex_);
		cachedKeyframe_.assign(packet->data, packet->data + packet->size);
		cachedKeyframeTimestamp_ = timestamp;
	}

	if (keyframe) {
		if (const auto profileLevelId = deriveH264ProfileLevelId(packet->data, packet->size)) {
			bool changed = false;
			{
				std::lock_guard<std::mutex> lock(h264ProfileMutex_);
				if (h264ProfileLevelId_ != *profileLevelId) {
					h264ProfileLevelId_ = *profileLevelId;
					changed = true;
				}
			}
			if (changed) {
				peerManager_->setH264ProfileLevelId(*profileLevelId);
				logInfo("Updated H.264 profile-level-id to %s from live SPS for future viewer offers",
				        profileLevelId->c_str());
			}
		}

		const int64_t nowMs = currentTimeMs();
		if (lastKeyframeWallClockMs_ != 0) {
			const int64_t gapMs = nowMs - lastKeyframeWallClockMs_;
			if (gapMs > kKeyframeIntervalWarnMs) {
				// Require a second oversized gap so a one-off encoder hiccup
				// cannot send someone chasing a setting that is already fine.
				if (++longKeyframeGaps_ >= 2 && !loggedKeyframeIntervalWarning_) {
					loggedKeyframeIntervalWarning_ = true;
					logWarning("Encoder is emitting keyframes about every %.1f seconds. Viewers cannot recover from "
					           "packet loss until the next keyframe, which looks like a periodic freeze. Set Settings "
					           "-> Output -> Streaming -> Keyframe Interval to 1-2 seconds; 0 (auto) leaves it near 8 "
					           "seconds.",
					           static_cast<double>(gapMs) / 1000.0);
				}
			} else {
				longKeyframeGaps_ = 0;
			}
		}
		lastKeyframeWallClockMs_ = nowMs;
	}

	QueuedMediaFrame frame;
	frame.type = MediaFrameType::Video;
	frame.payload.assign(packet->data, packet->data + packet->size);
	frame.timestamp = timestamp;
	frame.keyframe = keyframe;
	enqueueMediaFrame(std::move(frame));

	bool startSummaryInterval = false;
	{
		std::lock_guard<std::mutex> lock(publishSummaryMutex_);
		if (lastPublishSummaryMs_ == 0) {
			lastPublishSummaryMs_ = steadyTimeMs();
			startSummaryInterval = true;
		}
		summaryVideoFrames_++;
		summaryVideoBytes_ += packet->size;
		if (keyframe) {
			summaryKeyframes_++;
			summaryKeyframeBytes_ += packet->size;
			if (static_cast<uint64_t>(packet->size) > summaryMaxKeyframeBytes_) {
				summaryMaxKeyframeBytes_ = static_cast<uint64_t>(packet->size);
			}
		}
	}
	if (startSummaryInterval) {
		publishSummaryCv_.notify_one();
	}
}

void VDONinjaOutput::processAudioPacket(encoder_packet *packet)
{
	if (!packet || !packet->data || packet->size == 0) {
		return;
	}

	const size_t packetTrackIdx = static_cast<size_t>(packet->track_idx);
	if (packetTrackIdx != selectedAudioTrackIdx_) {
		const uint64_t droppedCount = ++droppedAudioPacketsOtherTracks_;
		if (droppedCount == 1) {
			logWarning("Ignoring OBS audio track %zu packet; publishing track %zu only (dropped=%llu)", packetTrackIdx,
			           selectedAudioTrackIdx_, static_cast<unsigned long long>(droppedCount));
		} else if ((droppedCount % 500) == 0) {
			logDebug("Still dropping non-selected OBS audio tracks (selected=%zu, dropped=%llu)",
			         selectedAudioTrackIdx_, static_cast<unsigned long long>(droppedCount));
		}
		return;
	}

	uint32_t timestamp = timestampFromPacket(packet, 48000.0);
	{
		std::lock_guard<std::mutex> lock(publishSummaryMutex_);
		audioTimestampSteps_.observe(timestamp);
		summaryAudioBytes_ += packet->size;
	}
	timestamp = sanitizeMonotonicTimestamp(timestamp, hasLastAudioRtpTimestamp_, lastAudioRtpTimestamp_, 960);

	QueuedMediaFrame frame;
	frame.type = MediaFrameType::Audio;
	frame.payload.assign(packet->data, packet->data + packet->size);
	frame.timestamp = timestamp;
	enqueueMediaFrame(std::move(frame));
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
		snapshot.audioSendEnabled = peer.audioSendEnabled;
		snapshot.videoSendEnabled = peer.videoSendEnabled;

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
