/*
 * OBS VDO.Ninja Plugin
 * Source module implementation
 */

#include "vdoninja-source.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <unordered_map>

#include <util/platform.h>
#include <util/threading.h>

#include <rtc/rtcpreceivingsession.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "vdoninja-utils.h"

namespace vdoninja
{

namespace
{

constexpr const char *kInternalNativeSourceId = "vdoninja_native_source_internal";
constexpr const char *kInternalNativeSourceSetting = "internal_native_receiver_source";
constexpr int kViewRequestTimeoutMs = 15000;
constexpr int kMinViewRequestGapMs = 1500;
const char *tr(const char *key, const char *fallback)
{
	const char *localized = obs_module_text(key);
	if (!localized || !*localized || std::strcmp(localized, key) == 0) {
		return fallback;
	}
	return localized;
}

template<typename Fn>
void runNoexceptCallback(const char *context, Fn &&fn)
{
	try {
		fn();
	} catch (const std::exception &e) {
		logError("%s threw exception: %s", context, e.what());
	} catch (...) {
		logError("%s threw unknown exception", context);
	}
}

template<typename T, typename Fn>
T runNoexceptCallbackValue(const char *context, T fallback, Fn &&fn)
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

obs_data_t *createBrowserSourceSettings(const std::string &url, uint32_t width, uint32_t height)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "url", url.c_str());
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	obs_data_set_int(settings, "fps", 30);
	obs_data_set_bool(settings, "reroute_audio", true);
	obs_data_set_bool(settings, "restart_when_active", false);
	obs_data_set_bool(settings, "shutdown", false);
	return settings;
}

obs_data_t *createNativeReceiverSourceSettings(const SourceSettings &sourceSettings, uint32_t width, uint32_t height)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, kInternalNativeSourceSetting, true);
	obs_data_set_bool(settings, "use_native_receiver", true);
	obs_data_set_string(settings, "stream_id", sourceSettings.streamId.c_str());
	obs_data_set_string(settings, "room_id", sourceSettings.roomId.c_str());
	obs_data_set_string(settings, "password", sourceSettings.password.c_str());
	obs_data_set_string(settings, "wss_host", sourceSettings.wssHost.c_str());
	obs_data_set_string(settings, "salt", sourceSettings.salt.c_str());
	obs_data_set_string(settings, "custom_ice_servers", sourceSettings.customIceServersText.c_str());
	obs_data_set_bool(settings, "enable_data_channel", sourceSettings.enableDataChannel);
	obs_data_set_bool(settings, "auto_reconnect", sourceSettings.autoReconnect);
	obs_data_set_bool(settings, "force_turn", sourceSettings.forceTurn);
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	return settings;
}

std::string toLowerCopy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
	               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value;
}

std::string ffmpegErrorString(int errorCode)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(errorCode, buffer, sizeof(buffer));
	return buffer;
}

const char *hardwareDeviceTypeName(AVHWDeviceType type)
{
	const char *name = av_hwdevice_get_type_name(type);
	return name ? name : "unknown";
}

enum AVPixelFormat choosePreferredHardwarePixelFormat(AVCodecContext *context, const enum AVPixelFormat *formats)
{
	if (!context || !formats) {
		return AV_PIX_FMT_NONE;
	}

	const auto preferredPixelFormat = context->opaque ? *static_cast<const int *>(context->opaque) : AV_PIX_FMT_NONE;
	if (preferredPixelFormat != AV_PIX_FMT_NONE) {
		for (const enum AVPixelFormat *fmt = formats; *fmt != AV_PIX_FMT_NONE; ++fmt) {
			if (*fmt == preferredPixelFormat) {
				return *fmt;
			}
		}
	}

	return avcodec_default_get_format(context, formats);
}

bool configureVideoHardwareDecoder(AVCodecContext *decoderContext, const AVCodec *codec, int &pixelFormat,
                                   std::string &deviceName)
{
#if defined(_WIN32)
	constexpr AVHWDeviceType preferredDeviceTypes[] = {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2};

	for (const AVHWDeviceType deviceType : preferredDeviceTypes) {
		const AVCodecHWConfig *matchingConfig = nullptr;
		for (int index = 0;; ++index) {
			const AVCodecHWConfig *config = avcodec_get_hw_config(codec, index);
			if (!config) {
				break;
			}
			if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0) {
				continue;
			}
			if (config->device_type != deviceType) {
				continue;
			}
			matchingConfig = config;
			break;
		}

		if (!matchingConfig) {
			continue;
		}

		AVBufferRef *deviceContext = nullptr;
		const int createResult = av_hwdevice_ctx_create(&deviceContext, deviceType, nullptr, nullptr, 0);
		if (createResult < 0) {
			logWarning("Failed to create %s hardware decode device: %s", hardwareDeviceTypeName(deviceType),
			           ffmpegErrorString(createResult).c_str());
			continue;
		}

		decoderContext->hw_device_ctx = deviceContext;
		pixelFormat = matchingConfig->pix_fmt;
		deviceName = hardwareDeviceTypeName(deviceType);
		decoderContext->get_format = choosePreferredHardwarePixelFormat;
		decoderContext->opaque = &pixelFormat;
		return true;
	}
#else
	UNUSED_PARAMETER(decoderContext);
	UNUSED_PARAMETER(codec);
	UNUSED_PARAMETER(pixelFormat);
	UNUSED_PARAMETER(deviceName);
#endif

	return false;
}

struct RtpPayloadView {
	size_t offset = 0;
	size_t size = 0;
	uint8_t payloadType = 0;
};

std::optional<RtpPayloadView> parseRtpPayloadView(const uint8_t *packetData, size_t packetSize)
{
	if (!packetData || packetSize < 12) {
		return std::nullopt;
	}

	const uint8_t version = static_cast<uint8_t>((packetData[0] >> 6) & 0x03);
	if (version != 2) {
		return std::nullopt;
	}

	const bool hasPadding = (packetData[0] & 0x20) != 0;
	const bool hasExtension = (packetData[0] & 0x10) != 0;
	const size_t csrcCount = static_cast<size_t>(packetData[0] & 0x0F);
	const uint8_t payloadType = static_cast<uint8_t>(packetData[1] & 0x7F);

	size_t headerSize = 12 + (csrcCount * 4);
	if (headerSize > packetSize) {
		return std::nullopt;
	}

	if (hasExtension) {
		if (headerSize + 4 > packetSize) {
			return std::nullopt;
		}
		const size_t extensionWords =
		    static_cast<size_t>((static_cast<uint16_t>(packetData[headerSize + 2]) << 8) |
		                        static_cast<uint16_t>(packetData[headerSize + 3]));
		headerSize += 4 + (extensionWords * 4);
		if (headerSize > packetSize) {
			return std::nullopt;
		}
	}

	size_t payloadSize = packetSize - headerSize;
	if (hasPadding) {
		const uint8_t paddingSize = packetData[packetSize - 1];
		if (paddingSize == 0 || paddingSize > payloadSize) {
			return std::nullopt;
		}
		payloadSize -= paddingSize;
	}

	if (payloadSize == 0) {
		return std::nullopt;
	}

	return RtpPayloadView{headerSize, payloadSize, payloadType};
}

std::optional<std::vector<uint8_t>> extractRedPrimaryPayload(const uint8_t *payload, size_t payloadSize)
{
	if (!payload || payloadSize < 2) {
		return std::nullopt;
	}

	size_t index = 0;
	while (index < payloadSize) {
		const uint8_t blockHeader = payload[index];
		if ((blockHeader & 0x80) == 0) {
			++index;
			if (index >= payloadSize) {
				return std::nullopt;
			}
			std::vector<uint8_t> primary(payloadSize - index);
			std::memcpy(primary.data(), payload + index, primary.size());
			return primary;
		}

		if (index + 4 > payloadSize) {
			return std::nullopt;
		}
		index += 4;
	}

	return std::nullopt;
}

bool mediaDescriptionHasCodec(const rtc::Description::Media &description, const std::string &codecName,
                              int *clockRate = nullptr, int *channels = nullptr)
{
	const std::string target = toLowerCopy(codecName);
	for (const int payloadType : description.payloadTypes()) {
		const auto *rtpMap = description.rtpMap(payloadType);
		if (!rtpMap) {
			continue;
		}

		if (toLowerCopy(rtpMap->format) != target) {
			continue;
		}

		if (clockRate) {
			*clockRate = rtpMap->clockRate;
		}
		if (channels) {
			if (rtpMap->encParams.empty()) {
				*channels = 0;
			} else {
				try {
					*channels = std::max(0, std::stoi(rtpMap->encParams));
				} catch (const std::exception &) {
					*channels = 0;
				}
			}
		}
		return true;
	}

	return false;
}

std::string describeMediaCodecs(const rtc::Description::Media &description)
{
	std::set<std::string> codecs;
	for (const int payloadType : description.payloadTypes()) {
		const auto *rtpMap = description.rtpMap(payloadType);
		if (!rtpMap || rtpMap->format.empty()) {
			continue;
		}
		codecs.insert(rtpMap->format);
	}

	if (codecs.empty()) {
		return "unknown";
	}

	std::string summary;
	for (const auto &codec : codecs) {
		if (!summary.empty()) {
			summary += ", ";
		}
		summary += codec;
	}
	return summary;
}

bool safeRequestKeyframe(const std::shared_ptr<rtc::Track> &track, const char *reasonTag)
{
	if (!track || !track->isOpen()) {
		return false;
	}

	try {
		return track->requestKeyframe();
	} catch (const std::exception &e) {
		logWarning("Failed to request video keyframe (%s): %s", reasonTag, e.what());
	} catch (...) {
		logWarning("Failed to request video keyframe (%s): unknown exception", reasonTag);
	}

	return false;
}

bool safeRequestBitrate(const std::shared_ptr<rtc::Track> &track, unsigned int bitrateBps, const char *reasonTag)
{
	if (!track || !track->isOpen() || bitrateBps == 0) {
		return false;
	}

	try {
		return track->requestBitrate(bitrateBps);
	} catch (const std::exception &e) {
		logWarning("Failed to request video bitrate (%s): %s", reasonTag, e.what());
	} catch (...) {
		logWarning("Failed to request video bitrate (%s): unknown exception", reasonTag);
	}

	return false;
}

speaker_layout speakerLayoutForChannels(int channels)
{
	return channels <= 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
}

void clearTrackCallbacks(const std::shared_ptr<rtc::Track> &track)
{
	if (!track) {
		return;
	}

	try {
		track->onFrame(nullptr);
	} catch (const std::exception &) {
	}
	try {
		track->onMessage(std::function<void(rtc::message_variant)> {});
	} catch (const std::exception &) {
	}
	try {
		track->onOpen(nullptr);
	} catch (const std::exception &) {
	}
	try {
		track->onClosed(nullptr);
	} catch (const std::exception &) {
	}
	try {
		track->onError(nullptr);
	} catch (const std::exception &) {
	}
	try {
		track->setMediaHandler(nullptr);
	} catch (const std::exception &) {
	}
}

void setNativeOnlyPropertiesVisible(obs_properties_t *props, bool visible)
{
	const char *propertyNames[] = {"enable_data_channel", "auto_reconnect", "custom_ice_servers",
	                               "custom_ice_servers_help", "force_turn"};
	for (const char *propertyName : propertyNames) {
		obs_property_t *property = obs_properties_get(props, propertyName);
		if (property) {
			obs_property_set_visible(property, visible);
		}
	}
}

bool vdoninja_source_native_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const bool useNativeReceiver = obs_data_get_bool(settings, "use_native_receiver");
	setNativeOnlyPropertiesVisible(props, useNativeReceiver);
	return true;
}

class InspectingReceivingSession : public rtc::RtcpReceivingSession {
public:
	using InspectCallback = std::function<void(const rtc::message_ptr &)>;

	explicit InspectingReceivingSession(InspectCallback callback) : callback_(std::move(callback)) {}

	void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override
	{
		if (callback_) {
			for (const auto &message : messages) {
				callback_(message);
			}
		}
		rtc::RtcpReceivingSession::incoming(messages, send);
	}

private:
	InspectCallback callback_;
};

class RtxRepairMediaHandler : public rtc::MediaHandler {
public:
	void media(const rtc::Description::Media &description) override
	{
		rtxPayloadTypes_.clear();
		for (const int payloadType : description.payloadTypes()) {
			const auto *rtpMap = description.rtpMap(payloadType);
			if (!rtpMap) {
				continue;
			}
			if (toLowerCopy(rtpMap->format) == "rtx") {
				for (const auto &fmtp : rtpMap->fmtps) {
					if (fmtp.rfind("apt=", 0) != 0) {
						continue;
					}
					try {
						const int originalPayloadType = std::stoi(fmtp.substr(4));
						rtxPayloadTypes_[static_cast<uint8_t>(payloadType)] =
						    static_cast<uint8_t>(originalPayloadType);
					} catch (const std::exception &) {
					}
				}
			}
		}
	}

	void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override
	{
		for (auto &message : messages) {
			if (!message || message->type != rtc::Message::Binary || message->size() < sizeof(rtc::RtpHeader)) {
				continue;
			}

			auto *rtpHeader = reinterpret_cast<rtc::RtpHeader *>(message->data());
			const auto rtxIt = rtxPayloadTypes_.find(rtpHeader->payloadType());
			if (rtxIt == rtxPayloadTypes_.end()) {
				continue;
			}

			const size_t headerSize = rtpHeader->getSize() + rtpHeader->getExtensionHeaderSize();
			if (message->size() < headerSize + sizeof(uint16_t)) {
				continue;
			}

			auto *rtxPacket = reinterpret_cast<rtc::RtpRtx *>(message->data());
			const size_t normalizedSize = rtxPacket->normalizePacket(message->size(), rtpHeader->ssrc(), rtxIt->second);
			message->resize(normalizedSize);
		}
		rtc::MediaHandler::incoming(messages, send);
	}

private:
	std::unordered_map<uint8_t, uint8_t> rtxPayloadTypes_;
};

} // namespace

void vdoninja_source_child_audio_capture(void *param, obs_source_t *source, const struct audio_data *audioData,
                                         bool muted)
{
	UNUSED_PARAMETER(source);
	if (!param) {
		return;
	}
	auto *state = static_cast<AsyncCallbackState<VDONinjaSource> *>(param);
	AsyncCallbackGuard<VDONinjaSource> guard(state);
	if (!guard) {
		return;
	}
	guard.owner()->onChildAudioCaptured(audioData, muted);
}

void vdoninja_source_child_audio_activate(void *param, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	if (!param) {
		return;
	}
	auto *state = static_cast<AsyncCallbackState<VDONinjaSource> *>(param);
	AsyncCallbackGuard<VDONinjaSource> guard(state);
	if (!guard) {
		return;
	}
	guard.owner()->onChildAudioActivated();
}

void vdoninja_source_child_audio_deactivate(void *param, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	if (!param) {
		return;
	}
	auto *state = static_cast<AsyncCallbackState<VDONinjaSource> *>(param);
	AsyncCallbackGuard<VDONinjaSource> guard(state);
	if (!guard) {
		return;
	}
	guard.owner()->onChildAudioDeactivated();
}

static const char *vdoninja_source_getname(void *)
{
	return tr("VDONinjaSource", "VDO.Ninja Source");
}

static const char *vdoninja_native_source_getname(void *)
{
	return "VDO.Ninja Native Receiver (Internal)";
}

static void *vdoninja_source_create(obs_data_t *settings, obs_source_t *source)
{
	try {
		return new VDONinjaSource(settings, source);
	} catch (const std::exception &e) {
		logError("Failed to create VDO.Ninja source: %s", e.what());
		return nullptr;
	} catch (...) {
		logError("Failed to create VDO.Ninja source: unknown exception");
		return nullptr;
	}
}

static void vdoninja_source_destroy(void *data)
{
	runNoexceptCallback("vdoninja_source_destroy", [data]() { delete static_cast<VDONinjaSource *>(data); });
}

static void vdoninja_source_update(void *data, obs_data_t *settings)
{
	runNoexceptCallback("vdoninja_source_update",
	                    [data, settings]() { static_cast<VDONinjaSource *>(data)->update(settings); });
}

static void vdoninja_source_activate(void *data)
{
	runNoexceptCallback("vdoninja_source_activate", [data]() { static_cast<VDONinjaSource *>(data)->activate(); });
}

static void vdoninja_source_deactivate(void *data)
{
	runNoexceptCallback("vdoninja_source_deactivate", [data]() { static_cast<VDONinjaSource *>(data)->deactivate(); });
}

static void vdoninja_source_show(void *data)
{
	runNoexceptCallback("vdoninja_source_show", [data]() { static_cast<VDONinjaSource *>(data)->show(); });
}

static void vdoninja_source_hide(void *data)
{
	runNoexceptCallback("vdoninja_source_hide", [data]() { static_cast<VDONinjaSource *>(data)->hide(); });
}

static void vdoninja_source_video_tick(void *data, float seconds)
{
	runNoexceptCallback("vdoninja_source_video_tick",
	                    [data, seconds]() { static_cast<VDONinjaSource *>(data)->videoTick(seconds); });
}

static void vdoninja_source_video_render(void *data, gs_effect_t *effect)
{
	runNoexceptCallback("vdoninja_source_video_render",
	                    [data, effect]() { static_cast<VDONinjaSource *>(data)->videoRender(effect); });
}

static uint32_t vdoninja_source_get_width(void *data)
{
	return runNoexceptCallbackValue<uint32_t>(
	    "vdoninja_source_get_width", 0, [data]() { return static_cast<VDONinjaSource *>(data)->getWidth(); });
}

static uint32_t vdoninja_source_get_height(void *data)
{
	return runNoexceptCallbackValue<uint32_t>(
	    "vdoninja_source_get_height", 0, [data]() { return static_cast<VDONinjaSource *>(data)->getHeight(); });
}

static void vdoninja_source_enum_active_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	runNoexceptCallback("vdoninja_source_enum_active_sources", [data, cb, param]() {
		VDONinjaSource *source = static_cast<VDONinjaSource *>(data);
		obs_source_t *child = source ? source->getActiveChildSource() : nullptr;
		if (child) {
			cb(source->obsSourceHandle(), child, param);
		}
	});
}

static obs_properties_t *vdoninja_source_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *note = obs_properties_add_text(
	    props, "experimental_note",
	    tr("VDONinjaSource.ModeNote",
	       "Default mode uses an internal Browser Source. Native Receiver (Experimental) uses the native H.264/Opus "
	       "WebRTC receive path with conservative retry backoff."),
	    OBS_TEXT_INFO);
	obs_property_text_set_info_type(note, OBS_TEXT_INFO_NORMAL);
	obs_property_text_set_info_word_wrap(note, true);

	obs_property_t *useNative =
	    obs_properties_add_bool(props, "use_native_receiver",
	                            tr("VDONinjaSource.UseNativeReceiver", "Use Native Receiver (Experimental)"));
	obs_property_set_long_description(
	    useNative,
	    tr("VDONinjaSource.UseNativeReceiver.Description",
	       "Unchecked uses the simple browser-backed viewer path. Checked enables the experimental native H.264/Opus "
	       "receiver path with slower retry/backoff after failures."));
	obs_property_set_modified_callback(useNative, vdoninja_source_native_mode_modified);

	obs_properties_add_text(props, "stream_id", tr("StreamID", "Stream ID"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "room_id", tr("RoomID", "Room ID"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "password", tr("Password", "Password"), OBS_TEXT_PASSWORD);
	obs_properties_add_bool(props, "enable_data_channel", tr("EnableDataChannel", "Enable Data Channel"));
	obs_properties_add_bool(props, "auto_reconnect", tr("AutoReconnect", "Auto Reconnect"));
	obs_properties_add_int(props, "width", tr("Width", "Width"), 320, 4096, 1);
	obs_properties_add_int(props, "height", tr("Height", "Height"), 240, 2160, 1);

	obs_properties_t *advanced = obs_properties_create();
	obs_property_t *wssHost =
	    obs_properties_add_text(advanced, "wss_host", tr("SignalingServer", "Signaling Server"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(
	    wssHost, tr("SignalingServer.OptionalHelp",
	                "Optional. Leave blank to use default signaling server: wss://wss.vdo.ninja:443"));
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
	obs_properties_add_group(props, "advanced", tr("AdvancedSettings", "Advanced Settings"), OBS_GROUP_NORMAL,
	                         advanced);
	setNativeOnlyPropertiesVisible(props, false);

	return props;
}

static void vdoninja_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(
	    settings, "experimental_note",
	    "Default mode uses an internal Browser Source. Native Receiver (Experimental) uses the native H.264/Opus "
	    "WebRTC receive path with conservative retry backoff.");
	obs_data_set_default_bool(settings, "use_native_receiver", false);
	obs_data_set_default_string(settings, "stream_id", "");
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
	obs_data_set_default_bool(settings, "enable_data_channel", true);
	obs_data_set_default_bool(settings, "auto_reconnect", true);
	obs_data_set_default_bool(settings, "force_turn", false);
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
}

obs_source_info vdoninja_source_info = {
    .id = "vdoninja_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = vdoninja_source_getname,
    .create = vdoninja_source_create,
    .destroy = vdoninja_source_destroy,
    .get_width = vdoninja_source_get_width,
    .get_height = vdoninja_source_get_height,
    .get_defaults = vdoninja_source_defaults,
    .get_properties = vdoninja_source_properties,
    .update = vdoninja_source_update,
    .activate = vdoninja_source_activate,
    .deactivate = vdoninja_source_deactivate,
    .show = vdoninja_source_show,
    .hide = vdoninja_source_hide,
    .video_tick = vdoninja_source_video_tick,
    .video_render = vdoninja_source_video_render,
    .enum_active_sources = vdoninja_source_enum_active_sources,
};

obs_source_info vdoninja_native_source_info = {
    .id = kInternalNativeSourceId,
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CAP_DISABLED,
    .get_name = vdoninja_native_source_getname,
    .create = vdoninja_source_create,
    .destroy = vdoninja_source_destroy,
    .get_width = vdoninja_source_get_width,
    .get_height = vdoninja_source_get_height,
    .get_defaults = vdoninja_source_defaults,
    .update = vdoninja_source_update,
    .activate = vdoninja_source_activate,
    .deactivate = vdoninja_source_deactivate,
    .show = vdoninja_source_show,
    .hide = vdoninja_source_hide,
    .video_tick = vdoninja_source_video_tick,
};

VDONinjaSource::VDONinjaSource(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	browserSourceName_ = "VDO.Ninja Source Child " + generateSessionId();
	nativeReceiverSourceName_ = "VDO.Ninja Native Child " + generateSessionId();
	callbackState_ = std::make_shared<AsyncCallbackState<VDONinjaSource>>();
	callbackState_->owner.store(this, std::memory_order_release);
	loadSettings(settings);
	if (isInternalNativeSource()) {
		signaling_ = std::make_unique<VDONinjaSignaling>();
		peerManager_ = std::make_unique<VDONinjaPeerManager>();
		logWarning("VDO.Ninja Source native receiver mode is experimental (H.264 video + Opus audio)");
	} else {
		updateWrapperChildSource();
		if (usingNativeReceiver()) {
			logWarning("VDO.Ninja Source wrapper created with experimental native receiver child enabled");
		} else {
			logInfo("VDO.Ninja Source created in browser-backed mode");
		}
	}
}

VDONinjaSource::~VDONinjaSource()
{
	if (isInternalNativeSource()) {
		disconnect();
		resetNativeState();
	} else {
		releaseChildSources();
	}
	drainAsyncCallbacks();

	logInfo("VDO.Ninja source destroyed");
}

void VDONinjaSource::loadSettings(obs_data_t *settings)
{
	internalNativeSource_ = obs_data_get_bool(settings, kInternalNativeSourceSetting);
	settings_.useNativeReceiver = internalNativeSource_ || obs_data_get_bool(settings, "use_native_receiver");
	settings_.streamId = trim(obs_data_get_string(settings, "stream_id"));
	settings_.roomId = trim(obs_data_get_string(settings, "room_id"));
	settings_.password = trim(obs_data_get_string(settings, "password"));
	settings_.wssHost = trim(obs_data_get_string(settings, "wss_host"));
	settings_.salt = trim(obs_data_get_string(settings, "salt"));
	settings_.customIceServersText = trim(obs_data_get_string(settings, "custom_ice_servers"));
	settings_.customIceServers = parseIceServers(settings_.customIceServersText);

	if (settings_.wssHost.empty()) {
		settings_.wssHost = DEFAULT_WSS_HOST;
	}
	if (settings_.salt.empty()) {
		settings_.salt = DEFAULT_SALT;
	}

	settings_.enableDataChannel = obs_data_get_bool(settings, "enable_data_channel");
	settings_.autoReconnect = obs_data_get_bool(settings, "auto_reconnect");
	settings_.forceTurn = obs_data_get_bool(settings, "force_turn");

	width_ = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
	height_ = static_cast<uint32_t>(obs_data_get_int(settings, "height"));
	if (width_ == 0) {
		width_ = 1920;
	}
	if (height_ == 0) {
		height_ = 1080;
	}
}

bool VDONinjaSource::isInternalNativeSource() const
{
	return internalNativeSource_;
}

bool VDONinjaSource::usingNativeReceiver() const
{
	return settings_.useNativeReceiver;
}

void VDONinjaSource::update(obs_data_t *settings)
{
	if (isInternalNativeSource()) {
		disconnect();
	} else {
		releaseChildSources();
	}

	loadSettings(settings);

	if (isInternalNativeSource()) {
		logWarning("VDO.Ninja Source native receiver mode is experimental (H.264 video + Opus audio)");
		if (active_.load()) {
			connect();
		}
	} else {
		updateWrapperChildSource();
	}
}

void VDONinjaSource::activate()
{
	if (active_.exchange(true)) {
		return;
	}

	if (isInternalNativeSource()) {
		connect();
	} else {
		updateWrapperChildSource();
		obs_source_t *child = getActiveChildSource();
		if (child) {
			syncChildLifecycleState(child);
		}
	}
}

void VDONinjaSource::deactivate()
{
	if (!active_.exchange(false)) {
		return;
	}

	if (isInternalNativeSource()) {
		disconnect();
	} else {
		obs_source_t *child = getActiveChildSource();
		if (child) {
			syncChildLifecycleState(child);
		}
	}
}

void VDONinjaSource::show()
{
	if (showing_.exchange(true)) {
		return;
	}

	if (!isInternalNativeSource()) {
		updateWrapperChildSource();
		obs_source_t *child = getActiveChildSource();
		if (child) {
			syncChildLifecycleState(child);
		}
	}
}

void VDONinjaSource::hide()
{
	if (!showing_.exchange(false)) {
		return;
	}

	if (!isInternalNativeSource()) {
		obs_source_t *child = getActiveChildSource();
		if (child) {
			syncChildLifecycleState(child);
		}
	}
}

void VDONinjaSource::connect()
{
	if (!isInternalNativeSource()) {
		return;
	}

	if (settings_.streamId.empty()) {
		logWarning("Stream ID is required");
		return;
	}

	if (connectionThread_.joinable()) {
		connectionThread_.join();
	}

	resetNativeState();
	resetViewRetryState();
	nativeRunning_ = true;
	lastVideoTime_ = 0;
	lastAudioTime_ = 0;
	lastKeyframeRequestTime_ = 0;
	logWarning("Use Native Receiver (Experimental) is enabled");
	connectionThread_ = std::thread(&VDONinjaSource::connectionThread, this);
}

void VDONinjaSource::disconnect()
{
	nativeRunning_ = false;
	connected_ = false;
	obs_source_set_audio_active(source_, false);
	resetViewRetryState();

	if (signaling_) {
		if (signaling_->isPublishing()) {
			signaling_->unpublishStream();
		}
		if (signaling_->isInRoom()) {
			signaling_->leaveRoom();
		}
		signaling_->disconnect();
		signaling_->setOnConnected(nullptr);
		signaling_->setOnDisconnected(nullptr);
		signaling_->setOnError(nullptr);
		signaling_->setOnStreamAdded(nullptr);
		signaling_->setOnPeerCleanup(nullptr);
	}

	if (peerManager_) {
		peerManager_->setOnPeerConnected(nullptr);
		peerManager_->setOnPeerDisconnected(nullptr);
		peerManager_->setOnTrack(nullptr);
		peerManager_->setOnDataChannel(nullptr);
		peerManager_->setOnDataChannelMessage(nullptr);
	}

	if (connectionThread_.joinable()) {
		connectionThread_.join();
	}

	resetNativeState();
}

void VDONinjaSource::connectionThread()
{
	try {
		logInfo("Connecting to VDO.Ninja stream: %s", settings_.streamId.c_str());
		const auto callbackState = callbackState_;

		peerManager_->initialize(signaling_.get());
		peerManager_->setEnableDataChannel(settings_.enableDataChannel);
		peerManager_->setIceServers(settings_.customIceServers);
		peerManager_->setForceTurn(settings_.forceTurn);
		signaling_->setSalt(settings_.salt);

	peerManager_->setOnTrack([callbackState](const std::string &uuid, TrackType type, std::shared_ptr<rtc::Track> track) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}

		if (type == TrackType::Video) {
			guard.owner()->onVideoTrack(uuid, track);
		} else {
			guard.owner()->onAudioTrack(uuid, track);
		}
	});

	peerManager_->setOnPeerConnected([callbackState](const std::string &uuid) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		VDONinjaSource *self = guard.owner();
		logInfo("Connected to publisher: %s", uuid.c_str());
		self->connected_ = true;
		self->cancelViewRetry();
		{
			std::lock_guard<std::mutex> lock(self->retryStateMutex_);
			self->viewRetryCount_ = 0;
			self->awaitingPeerConnection_ = false;
			self->suppressViewerRetry_ = false;
		}
		self->sendViewerPreferencesToPeer(uuid, "peer-connected");
		self->requestNativeTargetBitrate("peer-connected");
	});

	peerManager_->setOnPeerDisconnected([callbackState](const std::string &uuid) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		logInfo("Disconnected from publisher: %s", uuid.c_str());
		guard.owner()->handlePeerDisconnected(uuid);
	});

	peerManager_->setOnDataChannel([callbackState](const std::string &uuid, std::shared_ptr<rtc::DataChannel> dc) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		if (!dc) {
			return;
		}
		guard.owner()->sendViewerPreferencesToPeer(uuid, "datachannel-open");
	});

	peerManager_->setOnDataChannelMessage([callbackState](const std::string &uuid, const std::string &message) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		VDONinjaSource *self = guard.owner();
		constexpr size_t kMaxPreviewChars = 256;
		std::string preview = message;
		if (preview.size() > kMaxPreviewChars) {
			preview = preview.substr(0, kMaxPreviewChars) + "...(truncated)";
		}
		logInfo("Received source datachannel message from %s: %s", uuid.c_str(), preview.c_str());

		JsonParser raw(message);
		std::string targetUuid = raw.getString("UUID");
		if (targetUuid.empty()) {
			targetUuid = raw.getString("uuid");
		}
		const std::string targetSession = raw.getString("session");
		if (!targetUuid.empty()) {
			self->peerManager_->bindViewerSignalingDataChannel(uuid, targetUuid, targetSession);
		}

		if (self->signaling_ &&
		    (message.find("\"description\"") != std::string::npos || message.find("\"candidate\"") != std::string::npos ||
		     message.find("\"candidates\"") != std::string::npos || message.find("\"request\"") != std::string::npos ||
		     message.find("\"bye\"") != std::string::npos)) {
			self->signaling_->processIncomingMessage(message);
			if (!targetUuid.empty()) {
				self->sendViewerPreferencesToPeer(targetUuid, "resolved-media-peer");
			}
		}
	});

	signaling_->setOnConnected([callbackState]() {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		VDONinjaSource *self = guard.owner();
		logInfo("Connected to signaling server");

		if (!self->settings_.roomId.empty()) {
			self->signaling_->joinRoom(self->settings_.roomId, self->settings_.password);
		}

		self->requestViewStream("signaling-connected", true);
	});

	signaling_->setOnDisconnected([callbackState]() {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		logInfo("Disconnected from signaling server");
		guard.owner()->connected_ = false;
	});

	signaling_->setOnError([callbackState](const std::string &error) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		logError("Signaling error: %s", error.c_str());
		guard.owner()->handleSignalingAlert(error);
	});

	signaling_->setOnStreamAdded([callbackState](const std::string &streamId, const std::string &) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		VDONinjaSource *self = guard.owner();
		if (streamId == self->settings_.streamId ||
		    hashStreamId(self->settings_.streamId, self->settings_.password, self->settings_.salt) == streamId ||
		    hashStreamId(self->settings_.streamId, DEFAULT_PASSWORD, self->settings_.salt) == streamId) {
			logInfo("Target stream appeared in room, connecting...");
			self->requestViewStream("stream-added", false);
		}
	});
	signaling_->setOnPeerCleanup([callbackState](const std::string &uuid) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		guard.owner()->handlePeerCleanupSignal(uuid);
	});

		signaling_->setAutoReconnect(settings_.autoReconnect, DEFAULT_RECONNECT_ATTEMPTS);

		if (!signaling_->connect(settings_.wssHost)) {
			logError("Failed to connect to signaling server");
			nativeRunning_ = false;
			return;
		}

		while (nativeRunning_.load()) {
			serviceViewRetry();
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	} catch (const std::exception &e) {
		logError("Native receiver connection thread crashed: %s", e.what());
		connected_ = false;
		nativeRunning_ = false;
	} catch (...) {
		logError("Native receiver connection thread crashed: unknown exception");
		connected_ = false;
		nativeRunning_ = false;
	}
}

void VDONinjaSource::sendViewerPreferencesToPeer(const std::string &uuid, const char *reason)
{
	if (uuid.empty() || !peerManager_) {
		return;
	}

	const std::string preferences = buildViewerRequestMessage(width_, height_, !settings_.roomId.empty());
	peerManager_->sendDataToPeer(uuid, preferences);
	logInfo("Sent viewer preferences to %s (%s): %s", uuid.c_str(), reason ? reason : "unknown",
	        preferences.c_str());
}

void VDONinjaSource::requestNativeTargetBitrate(const char *reason)
{
	std::shared_ptr<rtc::Track> currentVideoTrack;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		currentVideoTrack = videoTrack_;
	}

	const unsigned int targetBitrateBps =
	    static_cast<unsigned int>(chooseViewerTargetBitrateKbps(width_, height_)) * 1000U;
	if (safeRequestBitrate(currentVideoTrack, targetBitrateBps, reason)) {
		logInfo("Requested native video REMB target of %u bps (%s)", targetBitrateBps,
		        reason ? reason : "unknown");
	}
}

void VDONinjaSource::requestViewStream(const char *reason, bool resetRetryCount)
{
	if (!nativeRunning_.load() || !signaling_ || !peerManager_ || settings_.streamId.empty() || !signaling_->isConnected()) {
		return;
	}

	const int64_t now = currentTimeMs();
	{
		std::lock_guard<std::mutex> lock(retryStateMutex_);
		if (!resetRetryCount && suppressViewerRetry_) {
			logWarning("Skipping native view retry (%s) because retries are suppressed by a server alert",
			           reason ? reason : "unknown");
			return;
		}
		if (!resetRetryCount && lastViewRequestTimeMs_ != 0 && now - lastViewRequestTimeMs_ < kMinViewRequestGapMs) {
			logDebug("Skipping native view request (%s); last request was %lld ms ago", reason ? reason : "unknown",
			         static_cast<long long>(now - lastViewRequestTimeMs_));
			return;
		}
		if (resetRetryCount) {
			viewRetryCount_ = 0;
		}
		lastViewRequestTimeMs_ = now;
		nextViewRetryTimeMs_ = 0;
		pendingViewRetryReason_.clear();
		awaitingPeerConnection_ = true;
	}

	if (signaling_->viewStream(settings_.streamId, settings_.password)) {
		peerManager_->startViewing(settings_.streamId);
		logInfo("Requested native stream playback (%s)", reason ? reason : "unknown");
	}
}

void VDONinjaSource::scheduleViewRetry(const char *reason, int delayMs, bool resetRetryCount)
{
	if (!nativeRunning_.load() || !settings_.autoReconnect || delayMs <= 0) {
		return;
	}

	const int64_t scheduledAt = currentTimeMs() + delayMs;
	bool updated = false;
	{
		std::lock_guard<std::mutex> lock(retryStateMutex_);
		if (suppressViewerRetry_) {
			return;
		}
		if (resetRetryCount) {
			viewRetryCount_ = 0;
		}
		if (nextViewRetryTimeMs_ == 0 || scheduledAt < nextViewRetryTimeMs_) {
			nextViewRetryTimeMs_ = scheduledAt;
			pendingViewRetryReason_ = reason ? reason : "retry";
			awaitingPeerConnection_ = false;
			updated = true;
		}
	}

	if (updated) {
		logInfo("Scheduling native view retry (%s) in %d ms", reason ? reason : "retry", delayMs);
	}
}

void VDONinjaSource::cancelViewRetry()
{
	std::lock_guard<std::mutex> lock(retryStateMutex_);
	nextViewRetryTimeMs_ = 0;
	pendingViewRetryReason_.clear();
	awaitingPeerConnection_ = false;
}

void VDONinjaSource::resetViewRetryState()
{
	std::lock_guard<std::mutex> lock(retryStateMutex_);
	viewRetryCount_ = 0;
	lastViewRequestTimeMs_ = 0;
	nextViewRetryTimeMs_ = 0;
	awaitingPeerConnection_ = false;
	suppressViewerRetry_ = false;
	pendingViewRetryReason_.clear();
}

void VDONinjaSource::serviceViewRetry()
{
	if (!nativeRunning_.load() || !settings_.autoReconnect || connected_.load()) {
		return;
	}

	const int64_t now = currentTimeMs();
	bool retryDue = false;
	{
		std::lock_guard<std::mutex> lock(retryStateMutex_);
		if (suppressViewerRetry_) {
			return;
		}
		if (awaitingPeerConnection_ && lastViewRequestTimeMs_ != 0 && nextViewRetryTimeMs_ == 0 &&
		    now - lastViewRequestTimeMs_ >= kViewRequestTimeoutMs) {
			nextViewRetryTimeMs_ = now + computeViewerRetryDelayMs(viewRetryCount_);
			pendingViewRetryReason_ = "no-offer-timeout";
			awaitingPeerConnection_ = false;
			logWarning("Native receiver did not get a peer within %d ms; backing off before retry", kViewRequestTimeoutMs);
		}
		if (nextViewRetryTimeMs_ != 0 && now >= nextViewRetryTimeMs_ && signaling_ && signaling_->isConnected()) {
			nextViewRetryTimeMs_ = 0;
			pendingViewRetryReason_.clear();
			++viewRetryCount_;
			retryDue = true;
		}
	}

	if (retryDue) {
		requestViewStream("scheduled-retry", false);
	}
}

void VDONinjaSource::handleSignalingAlert(const std::string &message)
{
	const SignalingAlertPolicy policy = classifySignalingAlert(message);
	if (policy.category == SignalingAlertCategory::None) {
		return;
	}

	if (policy.suppressViewerRetry) {
		std::lock_guard<std::mutex> lock(retryStateMutex_);
		suppressViewerRetry_ = true;
		awaitingPeerConnection_ = false;
		nextViewRetryTimeMs_ = 0;
		pendingViewRetryReason_.clear();
		logWarning("Suppressing native auto-retry due to signaling alert: %s", message.c_str());
		return;
	}

	if (policy.viewerRetryDelayMs > 0) {
		int retryCount = 0;
		{
			std::lock_guard<std::mutex> lock(retryStateMutex_);
			retryCount = viewRetryCount_;
		}
		scheduleViewRetry("server-alert", std::max(policy.viewerRetryDelayMs, computeViewerRetryDelayMs(retryCount)),
		                  false);
	}
}

void VDONinjaSource::handlePeerCleanupSignal(const std::string &uuid)
{
	if (uuid.empty()) {
		return;
	}

	logInfo("Native receiver got cleanup/bye for peer %s", uuid.c_str());
	if (peerManager_) {
		peerManager_->disconnectPeer(uuid);
	}
	handlePeerDisconnected(uuid);
}

void VDONinjaSource::onVideoTrack(const std::string &uuid, std::shared_ptr<rtc::Track> track)
{
	logInfo("Received video track from %s", uuid.c_str());

	if (!track) {
		return;
	}

	const rtc::Description::Media description = track->description();
	if (!mediaDescriptionHasCodec(description, "h264")) {
		logError("Native receiver only supports H.264 video today; offered codecs: %s",
		         describeMediaCodecs(description).c_str());
		return;
	}

	std::string payloadSummary;
	std::unordered_set<uint8_t> redPayloadTypes;
	for (const int payloadType : description.payloadTypes()) {
		const auto *rtpMap = description.rtpMap(payloadType);
		if (!rtpMap) {
			continue;
		}
		if (!payloadSummary.empty()) {
			payloadSummary += ", ";
		}
		payloadSummary += std::to_string(payloadType) + "=" + rtpMap->format;
		if (!rtpMap->fmtps.empty()) {
			payloadSummary += "(" + rtpMap->fmtps.front() + ")";
		}
		if (toLowerCopy(rtpMap->format) == "red") {
			redPayloadTypes.insert(static_cast<uint8_t>(payloadType));
		}
	}
	if (!payloadSummary.empty()) {
		logInfo("Native video payload map: %s", payloadSummary.c_str());
	}

	logInfo("Attaching native video receive callbacks (mid=%s, direction=%d)", track->mid().c_str(),
	        static_cast<int>(description.direction()));

	bool replacedExistingTrack = false;
	{
		std::scoped_lock stateLock(nativeStateMutex_, videoAssemblyMutex_, videoDecodeMutex_);
		if (videoTrack_ == track) {
			return;
		}
		if (!videoTrackPeerUuid_.empty() && videoTrackPeerUuid_ != uuid) {
			logWarning("Ignoring native video track from %s while peer %s remains active", uuid.c_str(),
			           videoTrackPeerUuid_.c_str());
			return;
		}
		replacedExistingTrack = (videoTrack_ != nullptr);
		clearTrackCallbacks(videoTrack_);
		videoTrack_ = track;
		videoTrackPeerUuid_ = uuid;
		videoRedPayloadTypes_ = redPayloadTypes;
		videoAssemblyBuffer_.clear();
		videoAssemblyTimestamp_ = 0;
		videoAssemblyActive_ = false;
		if (replacedExistingTrack) {
			logInfo("Replacing native video track for peer %s; resetting decoder state", uuid.c_str());
			resetVideoDecoder();
			loggedFirstVideoRtpPacket_ = false;
			loggedFirstVideoPacket_ = false;
			loggedFirstDecodedVideoFrame_ = false;
			lastVideoTime_ = 0;
			lastKeyframeRequestTime_ = 0;
		}
	}
	auto rtxFilter = std::make_shared<RtxRepairMediaHandler>();
	const auto callbackState = callbackState_;
	auto receivingSession =
	    std::make_shared<InspectingReceivingSession>([callbackState](const rtc::message_ptr &message) {
		    AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		    if (!guard) {
			    return;
		    }
		    VDONinjaSource *self = guard.owner();
		if (!message || message->type != rtc::Message::Binary || message->size() < sizeof(rtc::RtpHeader)) {
			return;
		}

		if (self->loggedFirstVideoRtpPacket_.exchange(true)) {
			return;
		}

		const auto *rtpHeader = reinterpret_cast<const rtc::RtpHeader *>(message->data());
		size_t headerSize = rtpHeader->getSize() + rtpHeader->getExtensionHeaderSize();
		if (message->size() < headerSize) {
			return;
		}

		size_t paddingSize = 0;
		if (rtpHeader->padding() && !message->empty()) {
			paddingSize = std::to_integer<uint8_t>(message->back());
		}
		if (message->size() <= headerSize + paddingSize) {
			return;
		}

		const size_t payloadSize = message->size() - headerSize - paddingSize;
		logInfo("Native receiver got first video RTP packet (pt=%u, bytes=%zu, marker=%u, rtp ts=%u)",
		        static_cast<unsigned>(rtpHeader->payloadType()), payloadSize, static_cast<unsigned>(rtpHeader->marker()),
		        rtpHeader->timestamp());
	});
	track->setMediaHandler(rtxFilter);
	track->chainMediaHandler(receivingSession);
	track->onMessage([callbackState, weakTrack = std::weak_ptr<rtc::Track>(track)](rtc::message_variant message) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		VDONinjaSource *self = guard.owner();
		const auto strongTrack = weakTrack.lock();
		if (!strongTrack) {
			return;
		}
		{
			std::lock_guard<std::mutex> stateLock(self->nativeStateMutex_);
			if (self->videoTrack_ != strongTrack) {
				return;
			}
		}

		if (!std::holds_alternative<rtc::binary>(message)) {
			return;
		}

		const auto &packet = std::get<rtc::binary>(message);
		self->processVideoRtpPacket(reinterpret_cast<const uint8_t *>(packet.data()), packet.size());
	});

	if (replacedExistingTrack && safeRequestKeyframe(track, "video-track-replaced")) {
		lastKeyframeRequestTime_ = currentTimeMs();
		logInfo("Requested video keyframe after replacing native video track");
	}
	requestNativeTargetBitrate("video-track-attached");
}

void VDONinjaSource::onAudioTrack(const std::string &uuid, std::shared_ptr<rtc::Track> track)
{
	logInfo("Received audio track from %s", uuid.c_str());

	if (!track) {
		return;
	}

	int sampleRate = 48000;
	int channels = 2;
	const rtc::Description::Media description = track->description();
	if (!mediaDescriptionHasCodec(description, "opus", &sampleRate, &channels)) {
		logError("Native receiver only supports Opus audio today; offered codecs: %s",
		         describeMediaCodecs(description).c_str());
		return;
	}
	logInfo("Attaching native audio receive callbacks (mid=%s, direction=%d, rate=%d, channels=%d)",
	        track->mid().c_str(), static_cast<int>(description.direction()), sampleRate, channels);

	bool replacedExistingTrack = false;
	{
		std::scoped_lock stateLock(nativeStateMutex_, audioDecodeMutex_);
		if (audioTrack_ == track) {
			return;
		}
		if (!videoTrackPeerUuid_.empty() && videoTrackPeerUuid_ != uuid) {
			logWarning("Ignoring native audio track from %s while video peer %s remains active", uuid.c_str(),
			           videoTrackPeerUuid_.c_str());
			return;
		}
		replacedExistingTrack = (audioTrack_ != nullptr);
		clearTrackCallbacks(audioTrack_);
		audioTrack_ = track;
		audioTrackPeerUuid_ = uuid;
		if (replacedExistingTrack) {
			logInfo("Replacing native audio track for peer %s; resetting decoder state", uuid.c_str());
			resetAudioDecoder();
			loggedFirstAudioPacket_ = false;
			loggedFirstDecodedAudioFrame_ = false;
			lastAudioTime_ = 0;
		}
	}
	audioSampleRate_ = sampleRate > 0 ? sampleRate : 48000;
	audioChannels_ = channels > 0 ? channels : 2;
	const auto callbackState = callbackState_;
	track->onMessage([callbackState, weakTrack = std::weak_ptr<rtc::Track>(track)](rtc::message_variant message) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		VDONinjaSource *self = guard.owner();
		const auto strongTrack = weakTrack.lock();
		if (!strongTrack) {
			return;
		}
		{
			std::lock_guard<std::mutex> stateLock(self->nativeStateMutex_);
			if (self->audioTrack_ != strongTrack) {
				return;
			}
		}
		if (!std::holds_alternative<rtc::binary>(message)) {
			return;
		}
		const auto &packet = std::get<rtc::binary>(message);
		self->processAudioRtpPacket(reinterpret_cast<const uint8_t *>(packet.data()), packet.size());
	});
}

void VDONinjaSource::processVideoData(const uint8_t *data, size_t size, uint32_t rtpTimestamp)
{
	if (!nativeRunning_.load() || !data || size == 0) {
		return;
	}

	if (!loggedFirstVideoPacket_.exchange(true)) {
		logInfo("Native receiver got first depacketized video payload (%zu bytes, rtp ts=%u)", size, rtpTimestamp);
	}

	std::shared_ptr<rtc::Track> currentVideoTrack;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		currentVideoTrack = videoTrack_;
	}

	std::lock_guard<std::mutex> lock(videoDecodeMutex_);
	if (!initializeVideoDecoder()) {
		return;
	}

	av_packet_unref(videoPacket_);
	const int allocResult = av_new_packet(videoPacket_, static_cast<int>(size));
	if (allocResult < 0) {
		logError("Failed to allocate H.264 packet buffer: %s", ffmpegErrorString(allocResult).c_str());
		return;
	}

	std::memcpy(videoPacket_->data, data, size);
	const int sendResult = avcodec_send_packet(videoDecoder_, videoPacket_);
	if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
		logWarning("Failed to submit H.264 packet: %s", ffmpegErrorString(sendResult).c_str());
		if (safeRequestKeyframe(currentVideoTrack, "send-packet-failure")) {
			lastKeyframeRequestTime_ = currentTimeMs();
		}
		return;
	}

	while (true) {
		const int receiveResult = avcodec_receive_frame(videoDecoder_, videoFrame_);
		if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
			break;
		}
		if (receiveResult < 0) {
			logWarning("Failed to decode H.264 frame: %s", ffmpegErrorString(receiveResult).c_str());
			if (safeRequestKeyframe(currentVideoTrack, "decode-failure")) {
				lastKeyframeRequestTime_ = currentTimeMs();
			}
			break;
		}

		const AVFrame *frameToOutput = videoFrame_;
		if (videoHwDecodeConfigured_ && videoHwPixelFormat_ != AV_PIX_FMT_NONE && !videoHwStatusLogged_) {
			if (videoFrame_->format == videoHwPixelFormat_) {
				logInfo("Native receiver is using hardware video decode via %s", videoHwDeviceName_.c_str());
			} else {
				logWarning("Native receiver opened %s hardware decode but decoder is returning software frames",
				           videoHwDeviceName_.c_str());
			}
			videoHwStatusLogged_ = true;
		}
		if (videoHwDecodeConfigured_ && videoHwPixelFormat_ != AV_PIX_FMT_NONE && videoFrame_->format == videoHwPixelFormat_) {
			av_frame_unref(videoTransferFrame_);
			const int transferResult = av_hwframe_transfer_data(videoTransferFrame_, videoFrame_, 0);
			if (transferResult < 0) {
				logWarning("Failed to transfer hardware-decoded H.264 frame from %s: %s; disabling hardware decode for this session",
				           videoHwDeviceName_.c_str(), ffmpegErrorString(transferResult).c_str());
				videoHwDecodeDisabled_ = true;
				resetVideoDecoder();
				if (safeRequestKeyframe(currentVideoTrack, "hw-transfer-failure")) {
					lastKeyframeRequestTime_ = currentTimeMs();
				}
				return;
			}
			av_frame_copy_props(videoTransferFrame_, videoFrame_);
			frameToOutput = videoTransferFrame_;
		}

		outputDecodedVideoFrame(frameToOutput, mapVideoTimestamp(rtpTimestamp));
		av_frame_unref(videoFrame_);
		if (frameToOutput == videoTransferFrame_) {
			av_frame_unref(videoTransferFrame_);
		}
	}

	lastVideoTime_ = currentTimeMs();
}

void VDONinjaSource::processVideoRtpPacket(const uint8_t *packetData, size_t packetSize)
{
	if (!nativeRunning_.load() || !packetData || packetSize < sizeof(rtc::RtpHeader)) {
		return;
	}

	const auto *rtpHeader = reinterpret_cast<const rtc::RtpHeader *>(packetData);
	const auto payloadView = parseRtpPayloadView(packetData, packetSize);
	if (!payloadView) {
		return;
	}

	const uint8_t *payload = packetData + payloadView->offset;
	size_t payloadSize = payloadView->size;
	std::vector<uint8_t> redPrimaryPayload;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		if (videoRedPayloadTypes_.count(payloadView->payloadType) != 0) {
			auto primaryPayload = extractRedPrimaryPayload(payload, payloadSize);
			if (!primaryPayload || primaryPayload->empty()) {
				return;
			}
			redPrimaryPayload = std::move(*primaryPayload);
			payload = redPrimaryPayload.data();
			payloadSize = redPrimaryPayload.size();
		}
	}

	const auto appendSeparator = [](std::vector<uint8_t> &accessUnit) {
		static const uint8_t kLongStartCode[] = {0x00, 0x00, 0x00, 0x01};
		accessUnit.insert(accessUnit.end(), std::begin(kLongStartCode), std::end(kLongStartCode));
	};

	const auto appendPayload = [&](std::vector<uint8_t> &accessUnit) -> bool {
		const uint8_t nalType = payload[0] & 0x1F;
		if (nalType > 0 && nalType < 24) {
			appendSeparator(accessUnit);
			accessUnit.insert(accessUnit.end(), payload, payload + payloadSize);
			return true;
		}

		if (nalType == 24) {
			size_t offset = 1;
			while (offset + sizeof(uint16_t) <= payloadSize) {
				const size_t naluSize = (static_cast<size_t>(payload[offset]) << 8) |
				                        static_cast<size_t>(payload[offset + 1]);
				offset += sizeof(uint16_t);
				if (offset + naluSize > payloadSize) {
					return false;
				}
				appendSeparator(accessUnit);
				accessUnit.insert(accessUnit.end(), payload + offset, payload + offset + naluSize);
				offset += naluSize;
			}
			return true;
		}

		if (nalType == 28) {
			if (payloadSize < 2) {
				return false;
			}
			const uint8_t fuIndicator = payload[0];
			const uint8_t fuHeader = payload[1];
			const bool start = (fuHeader & 0x80) != 0;
			const uint8_t reconstructedHeader = static_cast<uint8_t>((fuIndicator & 0xE0) | (fuHeader & 0x1F));
			if (start || accessUnit.empty()) {
				appendSeparator(accessUnit);
				accessUnit.push_back(reconstructedHeader);
			}
			accessUnit.insert(accessUnit.end(), payload + 2, payload + payloadSize);
			return true;
		}

		return false;
	};

	std::vector<std::pair<std::vector<uint8_t>, uint32_t>> completedFrames;
	completedFrames.reserve(2);

	{
		std::lock_guard<std::mutex> lock(videoAssemblyMutex_);
		if (!videoAssemblyActive_) {
			videoAssemblyActive_ = true;
			videoAssemblyTimestamp_ = rtpHeader->timestamp();
		} else if (videoAssemblyTimestamp_ != rtpHeader->timestamp()) {
			if (!videoAssemblyBuffer_.empty()) {
				completedFrames.emplace_back(std::move(videoAssemblyBuffer_), videoAssemblyTimestamp_);
				videoAssemblyBuffer_.clear();
			}
			videoAssemblyTimestamp_ = rtpHeader->timestamp();
		}

		if (!appendPayload(videoAssemblyBuffer_)) {
			return;
		}

		if (rtpHeader->marker() && !videoAssemblyBuffer_.empty()) {
			completedFrames.emplace_back(std::move(videoAssemblyBuffer_), videoAssemblyTimestamp_);
			videoAssemblyBuffer_.clear();
			videoAssemblyActive_ = false;
		}
	}

	for (const auto &frame : completedFrames) {
		if (!frame.first.empty()) {
			processVideoData(frame.first.data(), frame.first.size(), frame.second);
		}
	}
}

void VDONinjaSource::processAudioRtpPacket(const uint8_t *packetData, size_t packetSize)
{
	if (!nativeRunning_.load() || !packetData || packetSize < sizeof(rtc::RtpHeader)) {
		return;
	}

	const auto payloadView = parseRtpPayloadView(packetData, packetSize);
	if (!payloadView || payloadView->size == 0) {
		return;
	}

	const auto *rtpHeader = reinterpret_cast<const rtc::RtpHeader *>(packetData);
	processAudioData(packetData + payloadView->offset, payloadView->size, rtpHeader->timestamp());
}

void VDONinjaSource::processAudioData(const uint8_t *data, size_t size, uint32_t rtpTimestamp)
{
	if (!nativeRunning_.load() || !data || size == 0) {
		return;
	}

	if (!loggedFirstAudioPacket_.exchange(true)) {
		logInfo("Native receiver got first depacketized audio payload (%zu bytes, rtp ts=%u)", size, rtpTimestamp);
	}

	std::lock_guard<std::mutex> lock(audioDecodeMutex_);
	if (!initializeAudioDecoder(audioSampleRate_, audioChannels_)) {
		return;
	}

	av_packet_unref(audioPacket_);
	const int allocResult = av_new_packet(audioPacket_, static_cast<int>(size));
	if (allocResult < 0) {
		logError("Failed to allocate Opus packet buffer: %s", ffmpegErrorString(allocResult).c_str());
		return;
	}

	std::memcpy(audioPacket_->data, data, size);
	const int sendResult = avcodec_send_packet(audioDecoder_, audioPacket_);
	if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
		logWarning("Failed to submit Opus packet: %s", ffmpegErrorString(sendResult).c_str());
		return;
	}

	while (true) {
		const int receiveResult = avcodec_receive_frame(audioDecoder_, audioFrame_);
		if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
			break;
		}
		if (receiveResult < 0) {
			logWarning("Failed to decode Opus frame: %s", ffmpegErrorString(receiveResult).c_str());
			break;
		}

		outputDecodedAudioFrame(audioFrame_, mapAudioTimestamp(rtpTimestamp));
		av_frame_unref(audioFrame_);
	}

	lastAudioTime_ = currentTimeMs();
}

void VDONinjaSource::videoTick(float seconds)
{
	UNUSED_PARAMETER(seconds);

	if (!isInternalNativeSource()) {
		return;
	}

	std::shared_ptr<rtc::Track> videoTrack;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		videoTrack = videoTrack_;
	}

	if (!usingNativeReceiver() || !active_.load() || !connected_.load() || !videoTrack) {
		return;
	}

	const int64_t now = currentTimeMs();
	if (lastVideoTime_ != 0 && now - lastVideoTime_ < 1500) {
		return;
	}
	if (lastKeyframeRequestTime_ != 0 && now - lastKeyframeRequestTime_ < 1000) {
		return;
	}

	if (safeRequestKeyframe(videoTrack, "video-tick")) {
		lastKeyframeRequestTime_ = now;
		logInfo("Requested video keyframe for native receiver");
	}
}

void VDONinjaSource::videoRender(gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	if (isInternalNativeSource()) {
		return;
	}

	obs_source_t *child = getActiveChildSource();
	if (!child) {
		updateWrapperChildSource();
		child = getActiveChildSource();
	}
	if (child) {
		syncChildLifecycleState(child);
		obs_source_video_render(child);
	}
}

uint32_t VDONinjaSource::getWidth() const
{
	if (!isInternalNativeSource()) {
		obs_source_t *child = getActiveChildSource();
		const uint32_t childWidth = child ? obs_source_get_width(child) : 0;
		if (childWidth != 0) {
			return childWidth;
		}
	}
	return width_;
}

uint32_t VDONinjaSource::getHeight() const
{
	if (!isInternalNativeSource()) {
		obs_source_t *child = getActiveChildSource();
		const uint32_t childHeight = child ? obs_source_get_height(child) : 0;
		if (childHeight != 0) {
			return childHeight;
		}
	}
	return height_;
}

bool VDONinjaSource::isConnected() const
{
	if (isInternalNativeSource()) {
		return connected_.load();
	}

	return getActiveChildSource() != nullptr && !settings_.streamId.empty();
}

std::string VDONinjaSource::getStreamId() const
{
	return settings_.streamId;
}

obs_source_t *VDONinjaSource::obsSourceHandle() const
{
	return source_;
}

bool VDONinjaSource::initializeVideoDecoder()
{
	if (videoDecoder_) {
		return true;
	}

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		logError("FFmpeg H.264 decoder is unavailable");
		return false;
	}

	videoDecoder_ = avcodec_alloc_context3(codec);
	videoFrame_ = av_frame_alloc();
	videoTransferFrame_ = av_frame_alloc();
	videoPacket_ = av_packet_alloc();
	if (!videoDecoder_ || !videoFrame_ || !videoTransferFrame_ || !videoPacket_) {
		logError("Failed to allocate native H.264 decoder state");
		resetVideoDecoder();
		return false;
	}

	videoHwDecodeConfigured_ = false;
	videoHwStatusLogged_ = false;
	videoHwPixelFormat_ = AV_PIX_FMT_NONE;
	videoHwDeviceName_.clear();
	if (!videoHwDecodeDisabled_) {
		videoHwDecodeConfigured_ =
		    configureVideoHardwareDecoder(videoDecoder_, codec, videoHwPixelFormat_, videoHwDeviceName_);
	}
	if (!videoHwDecodeConfigured_) {
		videoDecoder_->thread_count = 0;
		videoDecoder_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	}

	const int openResult = avcodec_open2(videoDecoder_, codec, nullptr);
	if (openResult < 0) {
		if (videoHwDecodeConfigured_) {
			logWarning("Failed to open H.264 decoder with %s hardware acceleration: %s; falling back to software decode",
			           videoHwDeviceName_.c_str(), ffmpegErrorString(openResult).c_str());
			videoHwDecodeDisabled_ = true;
			resetVideoDecoder();
			return initializeVideoDecoder();
		}
		logError("Failed to open H.264 decoder: %s", ffmpegErrorString(openResult).c_str());
		resetVideoDecoder();
		return false;
	}

	if (videoHwDecodeConfigured_) {
		logInfo("Initialized native H.264 decoder with hardware acceleration backend %s", videoHwDeviceName_.c_str());
	} else {
		logInfo("Initialized native H.264 decoder in software mode");
	}

	return true;
}

bool VDONinjaSource::initializeAudioDecoder(int sampleRate, int channels)
{
	if (audioDecoder_ && audioSampleRate_ == sampleRate && audioChannels_ == channels) {
		return true;
	}

	resetAudioDecoder();

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!codec) {
		logError("FFmpeg Opus decoder is unavailable");
		return false;
	}

	audioDecoder_ = avcodec_alloc_context3(codec);
	audioFrame_ = av_frame_alloc();
	audioPacket_ = av_packet_alloc();
	if (!audioDecoder_ || !audioFrame_ || !audioPacket_) {
		logError("Failed to allocate native Opus decoder state");
		resetAudioDecoder();
		return false;
	}

	audioSampleRate_ = sampleRate > 0 ? sampleRate : 48000;
	audioChannels_ = channels > 0 ? channels : 2;
	audioDecoder_->sample_rate = audioSampleRate_;
	av_channel_layout_default(&audioDecoder_->ch_layout, audioChannels_);

	const int openResult = avcodec_open2(audioDecoder_, codec, nullptr);
	if (openResult < 0) {
		logError("Failed to open Opus decoder: %s", ffmpegErrorString(openResult).c_str());
		resetAudioDecoder();
		return false;
	}

	return true;
}

void VDONinjaSource::resetVideoDecoder()
{
	if (videoPacket_) {
		av_packet_free(&videoPacket_);
	}
	if (videoTransferFrame_) {
		av_frame_free(&videoTransferFrame_);
	}
	if (videoFrame_) {
		av_frame_free(&videoFrame_);
	}
	if (videoDecoder_) {
		avcodec_free_context(&videoDecoder_);
	}
	if (videoScaleContext_) {
		sws_freeContext(videoScaleContext_);
		videoScaleContext_ = nullptr;
	}

	videoTimingInitialized_ = false;
	videoBaseRtpTimestamp_ = 0;
	videoBaseTimestampNs_ = 0;
	lastVideoTimestampNs_ = 0;
	lastDecodedVideoWidth_ = 0;
	lastDecodedVideoHeight_ = 0;
	videoHwDecodeConfigured_ = false;
	videoHwStatusLogged_ = false;
	videoHwPixelFormat_ = AV_PIX_FMT_NONE;
	videoHwDeviceName_.clear();
}

void VDONinjaSource::resetAudioDecoder()
{
	if (audioPacket_) {
		av_packet_free(&audioPacket_);
	}
	if (audioFrame_) {
		av_frame_free(&audioFrame_);
	}
	if (audioDecoder_) {
		avcodec_free_context(&audioDecoder_);
	}
	if (audioResampleContext_) {
		swr_free(&audioResampleContext_);
	}

	audioResampleInputFormat_ = -1;
	audioResampleInputRate_ = 0;
	audioResampleInputChannels_ = 0;
	audioTimingInitialized_ = false;
	audioBaseRtpTimestamp_ = 0;
	audioBaseTimestampNs_ = 0;
	lastAudioTimestampNs_ = 0;
}

void VDONinjaSource::resetNativeState()
{
	std::scoped_lock nativeLock(nativeStateMutex_, videoAssemblyMutex_, videoDecodeMutex_, audioDecodeMutex_);
	clearTrackCallbacks(videoTrack_);
	clearTrackCallbacks(audioTrack_);
	loggedFirstVideoRtpPacket_ = false;
	loggedFirstVideoPacket_ = false;
	loggedFirstDecodedVideoFrame_ = false;
	loggedFirstAudioPacket_ = false;
	loggedFirstDecodedAudioFrame_ = false;
	videoTrack_.reset();
	audioTrack_.reset();
	videoTrackPeerUuid_.clear();
	audioTrackPeerUuid_.clear();
	videoRedPayloadTypes_.clear();
	videoAssemblyBuffer_.clear();
	videoAssemblyTimestamp_ = 0;
	videoAssemblyActive_ = false;
	videoHwDecodeDisabled_ = false;
	resetVideoDecoder();
	resetAudioDecoder();
	if (source_) {
		obs_source_set_audio_active(source_, false);
		obs_source_output_video(source_, nullptr);
	}
}

void VDONinjaSource::handlePeerDisconnected(const std::string &uuid)
{
	bool videoRemoved = false;
	bool audioRemoved = false;

	{
		std::scoped_lock nativeLock(nativeStateMutex_, videoAssemblyMutex_, videoDecodeMutex_, audioDecodeMutex_);

		if (!uuid.empty() && uuid == videoTrackPeerUuid_) {
			clearTrackCallbacks(videoTrack_);
			videoTrack_.reset();
			videoTrackPeerUuid_.clear();
			videoRedPayloadTypes_.clear();
			videoAssemblyBuffer_.clear();
			videoAssemblyTimestamp_ = 0;
			videoAssemblyActive_ = false;
			resetVideoDecoder();
			videoRemoved = true;
		}

		if (!uuid.empty() && uuid == audioTrackPeerUuid_) {
			clearTrackCallbacks(audioTrack_);
			audioTrack_.reset();
			audioTrackPeerUuid_.clear();
			resetAudioDecoder();
			audioRemoved = true;
		}

		connected_ = !videoTrackPeerUuid_.empty() || !audioTrackPeerUuid_.empty();
	}

	if (audioRemoved && source_) {
		obs_source_set_audio_active(source_, false);
	}

	if (videoRemoved && source_) {
		obs_source_output_video(source_, nullptr);
	}

	if (!connected_.load() && settings_.autoReconnect) {
		int retryCount = 0;
		{
			std::lock_guard<std::mutex> lock(retryStateMutex_);
			retryCount = viewRetryCount_;
		}
		scheduleViewRetry("peer-disconnected", computeViewerPeerRecoveryDelayMs(retryCount), false);
	}
}

void VDONinjaSource::outputDecodedVideoFrame(const AVFrame *frame, uint64_t timestampNs)
{
	if (!frame || !source_) {
		return;
	}

	if (!loggedFirstDecodedVideoFrame_.exchange(true)) {
		logInfo("Native receiver decoded first video frame (%dx%d, format=%d)", frame->width, frame->height,
		        frame->format);
	}
	if (frame->width != lastDecodedVideoWidth_ || frame->height != lastDecodedVideoHeight_) {
		logInfo("Native receiver video resolution changed to %dx%d", frame->width, frame->height);
		lastDecodedVideoWidth_ = frame->width;
		lastDecodedVideoHeight_ = frame->height;
	}

	const AVPixelFormat inputFormat = static_cast<AVPixelFormat>(frame->format);
	const AspectFitLayout layout =
	    computeAspectFitLayout(static_cast<uint32_t>(frame->width), static_cast<uint32_t>(frame->height), width_, height_);
	videoScaleContext_ = sws_getCachedContext(videoScaleContext_, frame->width, frame->height, inputFormat,
	                                          static_cast<int>(layout.contentWidth), static_cast<int>(layout.contentHeight),
	                                          AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!videoScaleContext_) {
		logError("Failed to create video conversion context");
		return;
	}

	const int outputStride = static_cast<int>(layout.outputWidth) * 4;
	std::vector<uint8_t> output(static_cast<size_t>(outputStride) * static_cast<size_t>(layout.outputHeight), 0);
	uint8_t *dstData[4] = {output.data() + (static_cast<size_t>(layout.offsetY) * static_cast<size_t>(outputStride)) +
	                                      (static_cast<size_t>(layout.offsetX) * 4),
	                      nullptr, nullptr, nullptr};
	int dstLinesize[4] = {outputStride, 0, 0, 0};

	const int scaledHeight = sws_scale(videoScaleContext_, frame->data, frame->linesize, 0, frame->height, dstData,
	                                   dstLinesize);
	if (scaledHeight <= 0 || static_cast<uint32_t>(scaledHeight) != layout.contentHeight) {
		logWarning("Failed to convert decoded video frame");
		return;
	}

	obs_source_frame obsFrame = {};
	obsFrame.width = layout.outputWidth;
	obsFrame.height = layout.outputHeight;
	obsFrame.format = VIDEO_FORMAT_BGRA;
	obsFrame.timestamp = timestampNs;
	obsFrame.full_range = true;
	obsFrame.data[0] = output.data();
	obsFrame.linesize[0] = static_cast<uint32_t>(outputStride);
	obs_source_output_video(source_, &obsFrame);
}

void VDONinjaSource::outputDecodedAudioFrame(const AVFrame *frame, uint64_t timestampNs)
{
	if (!frame || !source_) {
		return;
	}

	if (!loggedFirstDecodedAudioFrame_.exchange(true)) {
		logInfo("Native receiver decoded first audio frame (%d samples, format=%d, rate=%d)", frame->nb_samples,
		        frame->format, frame->sample_rate);
	}

	const int inputChannels = frame->ch_layout.nb_channels > 0 ? static_cast<int>(frame->ch_layout.nb_channels)
	                                                           : audioChannels_;
	const int outputChannels = inputChannels <= 1 ? 1 : 2;
	AVChannelLayout outputLayout;
	av_channel_layout_default(&outputLayout, outputChannels);

	const int inputFormat = frame->format;
	if (!audioResampleContext_ || audioResampleInputFormat_ != inputFormat ||
	    audioResampleInputRate_ != frame->sample_rate || audioResampleInputChannels_ != inputChannels) {
		if (audioResampleContext_) {
			swr_free(&audioResampleContext_);
		}

		AVChannelLayout inputLayout = frame->ch_layout;
		if (inputLayout.nb_channels == 0) {
			av_channel_layout_default(&inputLayout, inputChannels);
		}

		const int initResult =
		    swr_alloc_set_opts2(&audioResampleContext_, &outputLayout, AV_SAMPLE_FMT_FLTP, frame->sample_rate,
		                        &inputLayout, static_cast<AVSampleFormat>(inputFormat), frame->sample_rate, 0, nullptr);
		if (initResult < 0 || !audioResampleContext_) {
			logError("Failed to configure audio converter: %s", ffmpegErrorString(initResult).c_str());
			av_channel_layout_uninit(&outputLayout);
			return;
		}

		const int openResult = swr_init(audioResampleContext_);
		if (openResult < 0) {
			logError("Failed to initialize audio converter: %s", ffmpegErrorString(openResult).c_str());
			swr_free(&audioResampleContext_);
			av_channel_layout_uninit(&outputLayout);
			return;
		}

		audioResampleInputFormat_ = inputFormat;
		audioResampleInputRate_ = frame->sample_rate;
		audioResampleInputChannels_ = inputChannels;
	}

	const int outputSamples = swr_get_out_samples(audioResampleContext_, frame->nb_samples);
	if (outputSamples <= 0) {
		av_channel_layout_uninit(&outputLayout);
		return;
	}

	uint8_t *dstData[MAX_AV_PLANES] = {};
	int dstLinesize[MAX_AV_PLANES] = {};
	const int bufferSize = av_samples_alloc(dstData, dstLinesize, outputChannels, outputSamples, AV_SAMPLE_FMT_FLTP, 0);
	if (bufferSize < 0) {
		logError("Failed to allocate converted audio buffer: %s", ffmpegErrorString(bufferSize).c_str());
		av_channel_layout_uninit(&outputLayout);
		return;
	}

	const int convertedSamples =
	    swr_convert(audioResampleContext_, dstData, outputSamples,
	                const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
	if (convertedSamples < 0) {
		logError("Failed to convert decoded audio frame: %s", ffmpegErrorString(convertedSamples).c_str());
		av_freep(&dstData[0]);
		av_channel_layout_uninit(&outputLayout);
		return;
	}

	obs_source_audio audio = {};
	audio.frames = static_cast<uint32_t>(convertedSamples);
	audio.speakers = speakerLayoutForChannels(outputChannels);
	audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audio.samples_per_sec = static_cast<uint32_t>(frame->sample_rate);
	audio.timestamp = timestampNs;
	for (int i = 0; i < outputChannels && i < MAX_AV_PLANES; ++i) {
		audio.data[i] = dstData[i];
	}

	obs_source_set_audio_active(source_, true);
	obs_source_output_audio(source_, &audio);

	av_freep(&dstData[0]);
	av_channel_layout_uninit(&outputLayout);
}

uint64_t VDONinjaSource::mapVideoTimestamp(uint32_t rtpTimestamp)
{
	const uint64_t now = os_gettime_ns();
	if (!videoTimingInitialized_) {
		videoTimingInitialized_ = true;
		videoBaseRtpTimestamp_ = rtpTimestamp;
		videoBaseTimestampNs_ = now;
		lastVideoTimestampNs_ = now;
		return now;
	}

	const uint64_t delta = static_cast<uint32_t>(rtpTimestamp - videoBaseRtpTimestamp_);
	uint64_t mapped = videoBaseTimestampNs_ + (delta * 1000000000ULL) / 90000ULL;
	if (mapped <= lastVideoTimestampNs_) {
		mapped = lastVideoTimestampNs_ + 1;
	}
	lastVideoTimestampNs_ = mapped;
	return mapped;
}

uint64_t VDONinjaSource::mapAudioTimestamp(uint32_t rtpTimestamp)
{
	const uint64_t now = os_gettime_ns();
	const uint32_t clockRate = audioSampleRate_ > 0 ? static_cast<uint32_t>(audioSampleRate_) : 48000U;
	if (!audioTimingInitialized_) {
		audioTimingInitialized_ = true;
		audioBaseRtpTimestamp_ = rtpTimestamp;
		audioBaseTimestampNs_ = now;
		lastAudioTimestampNs_ = now;
		return now;
	}

	const uint64_t delta = static_cast<uint32_t>(rtpTimestamp - audioBaseRtpTimestamp_);
	uint64_t mapped = audioBaseTimestampNs_ + (delta * 1000000000ULL) / clockRate;
	if (mapped <= lastAudioTimestampNs_) {
		mapped = lastAudioTimestampNs_ + 1;
	}
	lastAudioTimestampNs_ = mapped;
	return mapped;
}

void VDONinjaSource::ensureNativeReceiverSource()
{
	if (isInternalNativeSource() || !usingNativeReceiver() || nativeReceiverSource_ || settings_.streamId.empty()) {
		return;
	}

	obs_data_t *nativeSettings = createNativeReceiverSourceSettings(settings_, width_, height_);
	nativeReceiverSource_ = obs_source_create_private(kInternalNativeSourceId, nativeReceiverSourceName_.c_str(),
	                                                  nativeSettings);
	obs_data_release(nativeSettings);

	if (!nativeReceiverSource_) {
		logError("Failed to create internal native receiver source for VDO.Ninja Source");
		return;
	}

	obs_source_add_audio_capture_callback(nativeReceiverSource_, vdoninja_source_child_audio_capture, callbackState_.get());
	signal_handler_t *sh = obs_source_get_signal_handler(nativeReceiverSource_);
	signal_handler_connect(sh, "audio_activate", vdoninja_source_child_audio_activate, callbackState_.get());
	signal_handler_connect(sh, "audio_deactivate", vdoninja_source_child_audio_deactivate, callbackState_.get());
	obs_source_set_audio_active(source_, obs_source_audio_active(nativeReceiverSource_));
	syncChildLifecycleState(nativeReceiverSource_);

	logInfo("Created internal native receiver source for VDO.Ninja Source");
}

void VDONinjaSource::releaseNativeReceiverSource()
{
	if (!nativeReceiverSource_) {
		return;
	}

	signal_handler_t *sh = obs_source_get_signal_handler(nativeReceiverSource_);
	signal_handler_disconnect(sh, "audio_activate", vdoninja_source_child_audio_activate, callbackState_.get());
	signal_handler_disconnect(sh, "audio_deactivate", vdoninja_source_child_audio_deactivate, callbackState_.get());
	obs_source_remove_audio_capture_callback(nativeReceiverSource_, vdoninja_source_child_audio_capture,
	                                         callbackState_.get());
	detachChildLifecycleState(nativeReceiverSource_);

	obs_source_set_audio_active(source_, false);
	obs_source_release(nativeReceiverSource_);
	nativeReceiverSource_ = nullptr;
}

void VDONinjaSource::updateNativeReceiverSource()
{
	if (isInternalNativeSource() || !usingNativeReceiver()) {
		releaseNativeReceiverSource();
		return;
	}

	if (settings_.streamId.empty()) {
		releaseNativeReceiverSource();
		return;
	}

	ensureNativeReceiverSource();
	if (!nativeReceiverSource_) {
		return;
	}

	obs_data_t *nativeSettings = createNativeReceiverSourceSettings(settings_, width_, height_);
	obs_source_update(nativeReceiverSource_, nativeSettings);
	obs_data_release(nativeSettings);
	syncChildLifecycleState(nativeReceiverSource_);
}

void VDONinjaSource::ensureBrowserSource()
{
	if (isInternalNativeSource() || usingNativeReceiver() || browserSource_ || settings_.streamId.empty()) {
		return;
	}

	const std::string url = buildViewerUrl();
	if (url.empty()) {
		return;
	}

	obs_data_t *browserSettings = createBrowserSourceSettings(url, width_, height_);
	browserSource_ = obs_source_create_private("browser_source", browserSourceName_.c_str(), browserSettings);
	obs_data_release(browserSettings);

	if (!browserSource_) {
		logError("Failed to create internal browser source for VDO.Ninja Source");
		return;
	}

	obs_source_add_audio_capture_callback(browserSource_, vdoninja_source_child_audio_capture, callbackState_.get());
	signal_handler_t *sh = obs_source_get_signal_handler(browserSource_);
	signal_handler_connect(sh, "audio_activate", vdoninja_source_child_audio_activate, callbackState_.get());
	signal_handler_connect(sh, "audio_deactivate", vdoninja_source_child_audio_deactivate, callbackState_.get());
	obs_source_set_audio_active(source_, obs_source_audio_active(browserSource_));
	syncChildLifecycleState(browserSource_);

	logInfo("Created internal Browser Source for VDO.Ninja Source");
}

void VDONinjaSource::releaseBrowserSource()
{
	if (!browserSource_) {
		return;
	}

	signal_handler_t *sh = obs_source_get_signal_handler(browserSource_);
	signal_handler_disconnect(sh, "audio_activate", vdoninja_source_child_audio_activate, callbackState_.get());
	signal_handler_disconnect(sh, "audio_deactivate", vdoninja_source_child_audio_deactivate, callbackState_.get());
	obs_source_remove_audio_capture_callback(browserSource_, vdoninja_source_child_audio_capture, callbackState_.get());
	detachChildLifecycleState(browserSource_);

	obs_source_set_audio_active(source_, false);
	obs_source_release(browserSource_);
	browserSource_ = nullptr;
}

void VDONinjaSource::updateBrowserSource()
{
	if (isInternalNativeSource() || usingNativeReceiver()) {
		releaseBrowserSource();
		return;
	}

	const std::string url = buildViewerUrl();
	if (url.empty()) {
		releaseBrowserSource();
		return;
	}

	ensureBrowserSource();
	if (!browserSource_) {
		return;
	}

	obs_data_t *browserSettings = createBrowserSourceSettings(url, width_, height_);
	obs_source_update(browserSource_, browserSettings);
	obs_data_release(browserSettings);
	syncChildLifecycleState(browserSource_);
}

void VDONinjaSource::releaseChildSources()
{
	releaseBrowserSource();
	releaseNativeReceiverSource();
}

void VDONinjaSource::updateWrapperChildSource()
{
	if (isInternalNativeSource()) {
		return;
	}

	if (usingNativeReceiver()) {
		releaseBrowserSource();
		updateNativeReceiverSource();
	} else {
		releaseNativeReceiverSource();
		updateBrowserSource();
	}
}

void VDONinjaSource::syncChildLifecycleState(obs_source_t *child)
{
	if (!child) {
		return;
	}

	const bool shouldShow = showing_.load() || (source_ && obs_source_showing(source_));
	if (shouldShow && !childShowing_) {
		obs_source_inc_showing(child);
		childShowing_ = true;
	} else if (!shouldShow && childShowing_) {
		obs_source_dec_showing(child);
		childShowing_ = false;
	}

	const bool shouldBeActive = active_.load() || (source_ && obs_source_active(source_));
	if (shouldBeActive && !childActive_) {
		obs_source_inc_active(child);
		childActive_ = true;
	} else if (!shouldBeActive && childActive_) {
		obs_source_dec_active(child);
		childActive_ = false;
	}
}

void VDONinjaSource::detachChildLifecycleState(obs_source_t *child)
{
	if (!child) {
		childShowing_ = false;
		childActive_ = false;
		return;
	}

	if (childShowing_) {
		obs_source_dec_showing(child);
		childShowing_ = false;
	}
	if (childActive_) {
		obs_source_dec_active(child);
		childActive_ = false;
	}
}

obs_source_t *VDONinjaSource::getActiveChildSource() const
{
	if (isInternalNativeSource()) {
		return nullptr;
	}

	return usingNativeReceiver() ? nativeReceiverSource_ : browserSource_;
}

std::string VDONinjaSource::buildViewerUrl() const
{
	return buildViewerPageUrl("https://vdo.ninja", settings_.streamId, settings_.password, settings_.roomId,
	                          settings_.salt, settings_.wssHost);
}

void VDONinjaSource::onChildAudioCaptured(const struct audio_data *audioData, bool muted)
{
	if (!audioData || muted || !source_) {
		return;
	}

	audio_t *audio = obs_get_audio();
	if (!audio) {
		return;
	}

	const struct audio_output_info *audioInfo = audio_output_get_info(audio);
	if (!audioInfo) {
		return;
	}

	obs_source_audio forwarded = {};
	forwarded.format = audioInfo->format;
	forwarded.samples_per_sec = audioInfo->samples_per_sec;
	forwarded.speakers = audioInfo->speakers;
	forwarded.frames = audioData->frames;
	forwarded.timestamp = audioData->timestamp;
	for (size_t i = 0; i < MAX_AV_PLANES; ++i) {
		forwarded.data[i] = audioData->data[i];
	}

	obs_source_output_audio(source_, &forwarded);
}

void VDONinjaSource::onChildAudioActivated()
{
	if (source_) {
		obs_source_set_audio_active(source_, true);
	}
}

void VDONinjaSource::onChildAudioDeactivated()
{
	if (source_) {
		obs_source_set_audio_active(source_, false);
	}
}

void VDONinjaSource::drainAsyncCallbacks()
{
	if (!callbackState_) {
		return;
	}

	AsyncCallbackGuard<VDONinjaSource>::detach(callbackState_.get());
	if (!AsyncCallbackGuard<VDONinjaSource>::waitForIdle(callbackState_.get())) {
		logWarning("Timed out waiting for VDO.Ninja source callbacks to drain during teardown");
	}
	callbackState_.reset();
}

} // namespace vdoninja
