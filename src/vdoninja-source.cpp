/*
 * OBS VDO.Ninja Plugin
 * Source module implementation
 */

#include "vdoninja-source.h"

#include <rtc/rtcpreceivingsession.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>

#include <util/platform.h>
#include <util/threading.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "plugin-main.h"
#include "vdoninja-rtp-utils.h"
#include "vdoninja-utils.h"

namespace vdoninja
{

namespace
{

constexpr const char *kInternalNativeSourceId = "vdoninja_native_source_internal";
constexpr const char *kInternalNativeSourceSetting = "internal_native_receiver_source";
constexpr int kViewRequestTimeoutMs = 15000;
constexpr int kMinViewRequestGapMs = 1500;
constexpr int64_t kNativeVideoStallBlankMs = 4000;
constexpr uint32_t kMinSourceWidth = 320;
constexpr uint32_t kMaxSourceWidth = 4096;
constexpr uint32_t kDefaultSourceWidth = 1920;
constexpr uint32_t kMinSourceHeight = 240;
constexpr uint32_t kMaxSourceHeight = 2160;
constexpr uint32_t kDefaultSourceHeight = 1080;

std::string buildNativeViewerInfoJson(obs_source_t *source)
{
	JsonBuilder info;
	const char *sourceName = source ? obs_source_get_name(source) : nullptr;
	info.add("label", (sourceName && *sourceName) ? sourceName : "OBS VDO.Ninja Viewer");
	info.add("version", PLUGIN_VERSION);
	info.add("platform", "OBS");
	info.add("Browser", "OBS VDO.Ninja Native Receiver");
	info.add("alpha_receive", "vp9-dualtrack-v1");
	return info.build();
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

const char *pixelFormatName(AVPixelFormat format)
{
	const char *name = av_get_pix_fmt_name(format);
	return name ? name : "unknown";
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

void runAudioActiveTask(obs_weak_source_t *weakSource, bool active)
{
	if (!weakSource) {
		return;
	}

	obs_source_t *source = obs_weak_source_get_source(weakSource);
	if (source) {
		obs_source_set_audio_active(source, active);
		obs_source_release(source);
	}
}

struct AudioActiveTask {
	obs_weak_source_t *weakSource;
	bool active;
};

void runQueuedAudioActiveTask(void *param)
{
	auto *task = static_cast<AudioActiveTask *>(param);
	if (!task) {
		return;
	}

	runAudioActiveTask(task->weakSource, task->active);
	obs_weak_source_release(task->weakSource);
	delete task;
}

void setObsWeakSourceAudioActiveSafe(obs_weak_source_t *weakSource, bool active)
{
	if (!weakSource) {
		return;
	}

	obs_weak_source_addref(weakSource);

	if (obs_in_task_thread(OBS_TASK_UI)) {
		runAudioActiveTask(weakSource, active);
		obs_weak_source_release(weakSource);
		return;
	}

	auto *task = new AudioActiveTask{weakSource, active};
	obs_queue_task(OBS_TASK_UI, runQueuedAudioActiveTask, task, false);
}

bool sourceSettingsEqualForChild(const SourceSettings &left, const SourceSettings &right)
{
	return left.streamId == right.streamId && left.roomId == right.roomId && left.password == right.password &&
	       left.wssHost == right.wssHost && left.salt == right.salt &&
	       left.customIceServersText == right.customIceServersText &&
	       left.useNativeReceiver == right.useNativeReceiver && left.enableDataChannel == right.enableDataChannel &&
	       left.autoReconnect == right.autoReconnect && left.forceTurn == right.forceTurn;
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
                                   std::string &deviceName, bool isVP9)
{
#if defined(_WIN32)
	if (isVP9) {
		return false;
	}

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
	UNUSED_PARAMETER(isVP9);
#endif

	return false;
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
	if (!track) {
		return false;
	}

	try {
		if (!track->isOpen()) {
			logDebug("Skipping video keyframe request (%s): track is not open", reasonTag);
			return false;
		}
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
	if (!track || bitrateBps == 0) {
		return false;
	}

	try {
		if (!track->isOpen()) {
			logDebug("Skipping video bitrate request (%s): track is not open", reasonTag);
			return false;
		}
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
		track->onMessage(std::function<void(rtc::message_variant)>{});
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

class InspectingReceivingSession : public rtc::RtcpReceivingSession
{
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

class RtxRepairMediaHandler : public rtc::MediaHandler
{
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
						rtxPayloadTypes_[static_cast<uint8_t>(payloadType)] = static_cast<uint8_t>(originalPayloadType);
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

			const auto normalizedSize =
			    normalizeRtxPacket(reinterpret_cast<uint8_t *>(message->data()), message->size(), rtxIt->second);
			if (!normalizedSize) {
				message.reset();
				continue;
			}
			message->resize(*normalizedSize);
		}
		messages.erase(std::remove(messages.begin(), messages.end(), nullptr), messages.end());
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
	return runNoexceptCallbackValue<uint32_t>("vdoninja_source_get_width", 0,
	                                          [data]() { return static_cast<VDONinjaSource *>(data)->getWidth(); });
}

static uint32_t vdoninja_source_get_height(void *data)
{
	return runNoexceptCallbackValue<uint32_t>("vdoninja_source_get_height", 0,
	                                          [data]() { return static_cast<VDONinjaSource *>(data)->getHeight(); });
}

static void vdoninja_source_enum_active_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	runNoexceptCallback("vdoninja_source_enum_active_sources", [data, cb, param]() {
		VDONinjaSource *source = static_cast<VDONinjaSource *>(data);
		obs_source_t *child = source ? source->acquireActiveChildSource() : nullptr;
		if (child) {
			cb(source->obsSourceHandle(), child, param);
			obs_source_release(child);
		}
	});
}

static obs_properties_t *vdoninja_source_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *note = obs_properties_add_text(
	    props, "experimental_note",
	    tr("VDONinjaSource.ModeNote",
	       "Default mode uses an internal Browser Source. Native Receiver (Experimental) uses the native "
	       "VP9/H.264/Opus WebRTC receive path. Compatible dual-track VP9 senders can preserve transparency "
	       "here; browser viewers stay standard color video."),
	    OBS_TEXT_INFO);
	obs_property_text_set_info_type(note, OBS_TEXT_INFO_NORMAL);
	obs_property_text_set_info_word_wrap(note, true);

	obs_property_t *useNative = obs_properties_add_bool(
	    props, "use_native_receiver", tr("VDONinjaSource.UseNativeReceiver", "Use Native Receiver (Experimental)"));
	obs_property_set_long_description(
	    useNative, tr("VDONinjaSource.UseNativeReceiver.Description",
	                  "Unchecked uses the simple browser-backed viewer path. Checked enables the experimental native "
	                  "VP9/H.264/Opus receiver path with slower retry/backoff after failures. Dual-track VP9 alpha "
	                  "transparency requires this mode and a compatible sender."));
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
	obs_properties_add_group(props, "advanced", tr("AdvancedSettings", "Advanced Settings"), OBS_GROUP_NORMAL,
	                         advanced);
	setNativeOnlyPropertiesVisible(props, false);

	return props;
}

static void vdoninja_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(
	    settings, "experimental_note",
	    "Default mode uses an internal Browser Source. Native Receiver (Experimental) uses the native "
	    "VP9/H.264/Opus WebRTC receive path. Compatible dual-track VP9 senders can preserve transparency "
	    "here; browser viewers stay standard color video.");
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
	sourceWeak_ = obs_source_get_weak_source(source_);
	browserSourceName_ = "VDO.Ninja Source Child " + generateSessionId();
	nativeReceiverSourceName_ = "VDO.Ninja Native Child " + generateSessionId();
	callbackState_ = std::make_shared<AsyncCallbackState<VDONinjaSource>>();
	callbackState_->owner.store(this, std::memory_order_release);
	loadSettings(settings);
	if (isInternalNativeSource()) {
		signaling_ = std::make_unique<VDONinjaSignaling>();
		peerManager_ = std::make_unique<VDONinjaPeerManager>();
		logWarning("VDO.Ninja Source native receiver mode is experimental (VP9/H.264 video + Opus audio)");
	} else {
		updateWrapperChildSource();
		if (usingNativeReceiver()) {
			logWarning("VDO.Ninja Source wrapper created with experimental native receiver child enabled");
		} else {
			logInfo("VDO.Ninja Source created in browser-backed mode");
		}
	}
}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
VDONinjaSource::VDONinjaSource(NativeMediaTestTag)
{
	callbackState_ = std::make_shared<AsyncCallbackState<VDONinjaSource>>();
	callbackState_->owner.store(this, std::memory_order_release);
	internalNativeSource_ = true;
	settings_.useNativeReceiver = true;
	nativeRunning_.store(true, std::memory_order_release);
	active_.store(true, std::memory_order_release);
	nativeVideoCodec_ = NativeVideoCodec::VP9;
	videoHwDecodeDisabled_ = true;
	publishOutputDimensions(16, 16);
	outputMediaEpoch_.store(mediaEpochGate_.capture(), std::memory_order_release);
}

void VDONinjaSource::setNativeMediaTestStageHook(NativeMediaTestStageHook hook)
{
	nativeMediaTestStageHook_ = std::move(hook);
}

void VDONinjaSource::setNativeMediaTestOutputHook(NativeMediaTestOutputHook hook)
{
	nativeMediaTestOutputHook_ = std::move(hook);
}

void VDONinjaSource::setNativeMediaTestAudioOutputHook(NativeMediaTestAudioOutputHook hook)
{
	nativeMediaTestAudioOutputHook_ = std::move(hook);
}

void VDONinjaSource::setNativeMediaTestClearOutputHook(NativeMediaTestClearOutputHook hook)
{
	nativeMediaTestClearOutputHook_ = std::move(hook);
}

void VDONinjaSource::setNativeMediaTestVideoDecoderHooks(NativeMediaTestSendPacketHook sendHook,
                                                         NativeMediaTestReceiveFrameHook receiveHook)
{
	nativeMediaTestSendPacketHook_ = std::move(sendHook);
	nativeMediaTestReceiveFrameHook_ = std::move(receiveHook);
}

void VDONinjaSource::setNativeMediaTestAlphaDecoderHooks(NativeMediaTestSendPacketHook sendHook,
                                                         NativeMediaTestReceiveFrameHook receiveHook)
{
	nativeMediaTestAlphaSendPacketHook_ = std::move(sendHook);
	nativeMediaTestAlphaReceiveFrameHook_ = std::move(receiveHook);
}

void VDONinjaSource::runNativeMediaTestStage(NativeMediaTestStage stage, bool alpha, uint32_t rtpTimestamp,
                                             uint64_t mediaEpoch)
{
	if (nativeMediaTestStageHook_) {
		nativeMediaTestStageHook_(stage, alpha, rtpTimestamp, mediaEpoch);
	}
}

void VDONinjaSource::feedNativeMediaTestVp9AccessUnit(bool alpha, const std::vector<uint8_t> &accessUnit,
                                                      uint32_t rtpTimestamp)
{
	feedNativeMediaTestVp9Packet(alpha, accessUnit, rtpTimestamp, true, true);
}

void VDONinjaSource::feedNativeMediaTestVp9Packet(bool alpha, const std::vector<uint8_t> &payload,
                                                  uint32_t rtpTimestamp, bool startOfFrame, bool endOfFrame)
{
	if (payload.empty()) {
		return;
	}

	const uint64_t mediaEpoch = mediaEpochGate_.capture();
	runNativeMediaTestStage(NativeMediaTestStage::Identity, alpha, rtpTimestamp, mediaEpoch);
	if (!mediaEpochGate_.isCurrent(mediaEpoch)) {
		return;
	}

	std::vector<uint8_t> packet(13 + payload.size(), 0);
	packet[0] = 0x80;
	packet[1] = static_cast<uint8_t>((endOfFrame ? 0x80 : 0x00) | 98);
	packet[2] = 0;
	packet[3] = 1;
	packet[4] = static_cast<uint8_t>(rtpTimestamp >> 24);
	packet[5] = static_cast<uint8_t>(rtpTimestamp >> 16);
	packet[6] = static_cast<uint8_t>(rtpTimestamp >> 8);
	packet[7] = static_cast<uint8_t>(rtpTimestamp);
	packet[8] = 0x01;
	packet[9] = 0x02;
	packet[10] = 0x03;
	packet[11] = alpha ? 0x05 : 0x04;
	packet[12] = static_cast<uint8_t>((startOfFrame ? 0x08 : 0x00) | (endOfFrame ? 0x04 : 0x00));
	std::copy(payload.begin(), payload.end(), packet.begin() + 13);

	if (alpha) {
		processAlphaRtpPacket(packet.data(), packet.size(), mediaEpoch);
	} else {
		processVideoRtpPacket(packet.data(), packet.size(), mediaEpoch);
	}
}

void VDONinjaSource::transitionNativeMediaTestPipeline(bool alphaActive, bool enableOutput)
{
	{
		std::unique_lock<std::mutex> stateLock(nativeStateMutex_);
		std::unique_lock<std::mutex> videoAssemblyLock(videoAssemblyMutex_);
		std::unique_lock<std::mutex> videoDecodeLock(videoDecodeMutex_);
		std::unique_lock<std::mutex> alphaAssemblyLock(alphaAssemblyMutex_);
		std::unique_lock<std::mutex> alphaDecodeLock(alphaDecodeMutex_);
		std::unique_lock<std::mutex> outputLock(videoOutputMutex_);
		std::unique_lock<std::mutex> pairingLock(alphaPairingMutex_);
		std::unique_lock<std::mutex> audioDecodeLock(audioDecodeMutex_);
		resetMediaPipelineStateLocked();
		alphaTrackActive_.store(alphaActive, std::memory_order_release);
		preferSoftwareVp9DecodeForAlpha_.store(alphaActive, std::memory_order_release);
	}
	completeMediaPipelineTransition("native-media-linked-gate", enableOutput);
}

void VDONinjaSource::applyNativeMediaTestVideoSuppression(bool suppressed)
{
	ReceiverVideoSuppressionUpdate update;
	update.hasMediaVideoMuted = true;
	update.mediaVideoMuted = suppressed;
	handleReceiverVideoSuppressionState("native-media-linked-gate", update);
}

void VDONinjaSource::applyNativeMediaTestVideoSuppressionUpdate(const ReceiverVideoSuppressionUpdate &update)
{
	handleReceiverVideoSuppressionState("native-media-linked-gate", update);
}

void VDONinjaSource::resetNativeMediaTestState()
{
	resetNativeState();
}

void VDONinjaSource::applyNativeMediaTestPeerCleanup(const PeerEventIdentity &identity)
{
	handlePeerCleanupSignal(identity);
}

void VDONinjaSource::applyNativeMediaTestSignalingCleanup(VDONinjaPeerManager &manager, const std::string &uuid,
                                                          const std::string &session)
{
	handleSignalingPeerCleanup(manager, uuid, session);
}

void VDONinjaSource::applyNativeMediaTestSignalingLifecycleEvent(VDONinjaPeerManager &manager,
                                                                 const SignalingLifecycleEvent &event)
{
	if (manager.processSignalingLifecycleEvent(event) == SignalingLifecycleDisposition::AmbiguousSessionless) {
		nativeMediaTestAmbiguousSessionlessCleanups_.fetch_add(1, std::memory_order_acq_rel);
	}
}

void VDONinjaSource::applyNativeMediaTestLegacyStreamRemoved(const std::string &streamId, const std::string &uuid)
{
	handleStreamRemovedSignal(streamId, uuid);
}

void VDONinjaSource::setNativeMediaTestSignalingLifecycleHook(NativeMediaTestSignalingLifecycleHook hook)
{
	if (nativeMediaTestPeerManager_) {
		nativeMediaTestPeerManager_->setNativeMediaTestSignalingLifecycleHook(std::move(hook));
	}
}

void VDONinjaSource::setNativeMediaTestStreamId(const std::string &streamId)
{
	settings_.streamId = streamId;
}

void VDONinjaSource::bindNativeMediaTestSignaling(VDONinjaSignaling &signaling, VDONinjaPeerManager &manager)
{
	configureSignalingLifecycleCallbacks(signaling, manager);
}

void VDONinjaSource::setNativeMediaTestSignaling(std::unique_ptr<VDONinjaSignaling> signaling)
{
	signaling_ = std::move(signaling);
}

bool VDONinjaSource::advanceNativeMediaTestPeerIdentity(const PeerEventIdentity &identity)
{
	std::lock_guard<std::mutex> applyLock(trackEventApplyMutex_);
	return acceptPeerEventIdentityLocked(identity);
}

void VDONinjaSource::emitNativeMediaTestAudioFrame(uint64_t timestampNs)
{
	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		return;
	}
	outputDecodedAudioFrame(frame, timestampNs);
	av_frame_free(&frame);
}

void VDONinjaSource::ageNativeMediaTestVideoOutput(int64_t ageMs)
{
	lastVideoTime_.store(currentTimeMs() - std::max<int64_t>(0, ageMs), std::memory_order_relaxed);
}

void VDONinjaSource::updateNativeMediaTestDimensions(uint32_t width, uint32_t height)
{
	publishOutputDimensions(width, height);
}

NativeMediaTestSnapshot VDONinjaSource::nativeMediaTestSnapshot()
{
	NativeMediaTestSnapshot snapshot;
	std::unique_lock<std::mutex> stateLock(nativeStateMutex_);
	std::unique_lock<std::mutex> videoAssemblyLock(videoAssemblyMutex_);
	std::unique_lock<std::mutex> videoDecodeLock(videoDecodeMutex_);
	std::unique_lock<std::mutex> alphaAssemblyLock(alphaAssemblyMutex_);
	std::unique_lock<std::mutex> alphaDecodeLock(alphaDecodeMutex_);
	std::unique_lock<std::mutex> outputLock(videoOutputMutex_);
	std::unique_lock<std::mutex> commitStateLock(videoCommitStateMutex_);
	std::unique_lock<std::mutex> pairingLock(alphaPairingMutex_);
	snapshot.primaryAssemblyActive = videoAssemblyActive_;
	snapshot.alphaAssemblyActive = alphaAssemblyActive_;
	snapshot.primaryDecoderAllocated = videoDecoder_ != nullptr;
	snapshot.alphaDecoderAllocated = alphaDecoder_ != nullptr;
	snapshot.primaryAssemblyBytes = videoAssemblyBuffer_.size();
	snapshot.alphaAssemblyBytes = alphaAssemblyBuffer_.size();
	snapshot.pendingPrimaryFrames = alphaFrameSynchronizer_.pendingPrimaryCount();
	snapshot.pendingAlphaFrames = alphaFrameSynchronizer_.pendingAlphaCount();
	snapshot.retainedVideoFrames = nativeMediaTestRetainedVideoFrames_->load(std::memory_order_acquire);
	snapshot.primaryRequestedThreadCount = nativeMediaTestPrimaryRequestedThreadCount_;
	snapshot.primaryRequestedThreadType = nativeMediaTestPrimaryRequestedThreadType_;
	snapshot.primaryActiveThreadType = videoDecoder_ ? videoDecoder_->active_thread_type : 0;
	snapshot.alphaRequestedThreadCount = nativeMediaTestAlphaRequestedThreadCount_;
	snapshot.alphaRequestedThreadType = nativeMediaTestAlphaRequestedThreadType_;
	snapshot.alphaActiveThreadType = alphaDecoder_ ? alphaDecoder_->active_thread_type : 0;
	snapshot.videoOutputActive = videoOutputActive_.load(std::memory_order_relaxed);
	snapshot.lastVideoTimeMs = lastVideoTime_.load(std::memory_order_relaxed);
	const auto dimensions = outputDimensions();
	snapshot.outputWidth = dimensions.width;
	snapshot.outputHeight = dimensions.height;
	snapshot.videoSuppressed = remoteVideoMuted_.load(std::memory_order_acquire);
	snapshot.audioMuted = remoteAudioMuted_.load(std::memory_order_relaxed);
	snapshot.sourceAudioActive = sourceAudioActive_.load(std::memory_order_relaxed);
	snapshot.mediaVideoMuted = remoteMediaVideoMuted_.load(std::memory_order_relaxed);
	snapshot.directorVideoMuted = remoteDirectorVideoMuted_.load(std::memory_order_relaxed);
	snapshot.virtualHangup = remoteVirtualHangup_.load(std::memory_order_relaxed);
	snapshot.acceptedPeerCleanups = nativeMediaTestAcceptedPeerCleanups_.load(std::memory_order_acquire);
	snapshot.peerRetirements = nativeMediaTestPeerRetirements_.load(std::memory_order_acquire);
	snapshot.peerRetrySchedules = nativeMediaTestPeerRetrySchedules_.load(std::memory_order_acquire);
	snapshot.dataChannelOpenActions = nativeMediaTestDataChannelOpenActions_.load(std::memory_order_acquire);
	snapshot.ambiguousSessionlessCleanups =
	    nativeMediaTestAmbiguousSessionlessCleanups_.load(std::memory_order_acquire);
	snapshot.targetedPeerByes = nativeMediaTestTargetedPeerByes_.load(std::memory_order_acquire);
	snapshot.legacyStreamRemovalActions = nativeMediaTestLegacyStreamRemovalActions_.load(std::memory_order_acquire);
	return snapshot;
}

NativeMediaTestTrackSnapshot VDONinjaSource::nativeMediaTestTrackSnapshot()
{
	std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
	return {videoTrack_.get(), alphaVideoTrack_.get(), audioTrack_.get()};
}

void VDONinjaSource::bindNativeMediaTestPeerManager(VDONinjaPeerManager &manager)
{
	nativeMediaTestPeerManager_ = &manager;
	const auto callbackState = callbackState_;
	manager.setOnTrack([callbackState](const TrackSlotEvent &event) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		guard.owner()->handleTrackSlotEvent(event);
	});
	manager.setOnPeerDisconnected([callbackState](const PeerEventIdentity &identity) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		guard.owner()->handlePeerDisconnected(identity);
	});
	manager.setOnDataChannel(
	    [callbackState](const PeerEventIdentity &identity, const std::shared_ptr<rtc::DataChannel> &dc) {
		    AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		    if (!guard) {
			    return;
		    }
		    guard.owner()->handlePeerDataChannelOpen(identity, dc);
	    });
	manager.setOnDataChannelMessage([callbackState](const PeerEventIdentity &identity, const std::string &message) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		guard.owner()->handlePeerDataChannelMessage(identity, message);
	});
	manager.setOnAcceptedSignalingLifecycleEvent([callbackState](const AcceptedSignalingLifecycleEvent &event) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		guard.owner()->handleAcceptedSignalingLifecycleEvent(event);
	});
}

bool VDONinjaSource::nativeMediaTestCanAcquireVideoCommitState()
{
	if (!videoCommitStateMutex_.try_lock()) {
		return false;
	}
	videoCommitStateMutex_.unlock();
	return true;
}

int VDONinjaSource::nativeMediaTestRejectedTrackEventCount() const
{
	return nativeMediaTestRejectedTrackEvents_.load(std::memory_order_acquire);
}

uint64_t VDONinjaSource::nativeMediaTestEpoch() const
{
	return mediaEpochGate_.capture();
}
#endif

VDONinjaSource::~VDONinjaSource()
{
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	// Test callbacks often reference collectors declared after this source.
	// They are observation seams only and must not participate in teardown.
	nativeMediaTestStageHook_ = nullptr;
	nativeMediaTestOutputHook_ = nullptr;
	nativeMediaTestAudioOutputHook_ = nullptr;
	nativeMediaTestClearOutputHook_ = nullptr;
	nativeMediaTestSendPacketHook_ = nullptr;
	nativeMediaTestReceiveFrameHook_ = nullptr;
	nativeMediaTestAlphaSendPacketHook_ = nullptr;
	nativeMediaTestAlphaReceiveFrameHook_ = nullptr;
#endif
	if (isInternalNativeSource()) {
		disconnect();
		resetNativeState();
	} else {
		releaseChildSources();
	}
	drainAsyncCallbacks();
	if (sourceWeak_) {
		obs_weak_source_release(sourceWeak_);
		sourceWeak_ = nullptr;
	}

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

	const int64_t rawWidth = obs_data_get_int(settings, "width");
	const int64_t rawHeight = obs_data_get_int(settings, "height");
	const uint32_t width = normalizeSourceDimension(rawWidth, kDefaultSourceWidth, kMinSourceWidth, kMaxSourceWidth);
	const uint32_t height =
	    normalizeSourceDimension(rawHeight, kDefaultSourceHeight, kMinSourceHeight, kMaxSourceHeight);
	publishOutputDimensions(width, height);
	if (rawWidth != static_cast<int64_t>(width)) {
		logWarning("Clamped VDO.Ninja source width setting from %lld to %u", static_cast<long long>(rawWidth), width);
	}
	if (rawHeight != static_cast<int64_t>(height)) {
		logWarning("Clamped VDO.Ninja source height setting from %lld to %u", static_cast<long long>(rawHeight),
		           height);
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
		const SourceSettings previousSettings = settings_;
		const auto previousDimensions = outputDimensions();

		loadSettings(settings);

		logWarning("VDO.Ninja Source native receiver mode is experimental (VP9/H.264 video + Opus audio)");

		const bool connectionSettingsChanged = !sourceSettingsEqualForChild(previousSettings, settings_);
		const auto dimensions = outputDimensions();
		const bool dimensionsChanged =
		    previousDimensions.width != dimensions.width || previousDimensions.height != dimensions.height;
		if (!active_.load()) {
			if (nativeRunning_.load() && connectionSettingsChanged) {
				disconnect();
			}
			return;
		}
		if (connectionSettingsChanged) {
			disconnect();
			connect();
			return;
		}
		if (!nativeRunning_.load()) {
			connect();
			return;
		}
		if (dimensionsChanged) {
			logInfo("Updated native receiver dimensions to %ux%u without reconnecting", dimensions.width,
			        dimensions.height);
			if (peerManager_) {
				peerManager_->sendDataToAll(buildViewerRequestMessage(dimensions.width, dimensions.height,
				                                                      !settings_.roomId.empty(),
				                                                      buildNativeViewerInfoJson(source_)));
			}
			requestNativeTargetBitrate("source-dimension-update");
		}
	} else {
		loadSettings(settings);
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
		obs_source_t *child = acquireActiveChildSource();
		if (child) {
			syncChildLifecycleState(child);
			obs_source_release(child);
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
		obs_source_t *child = acquireActiveChildSource();
		if (child) {
			syncChildLifecycleState(child);
			obs_source_release(child);
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
		obs_source_t *child = acquireActiveChildSource();
		if (child) {
			syncChildLifecycleState(child);
			obs_source_release(child);
		}
	}
}

void VDONinjaSource::hide()
{
	if (!showing_.exchange(false)) {
		return;
	}

	if (!isInternalNativeSource()) {
		obs_source_t *child = acquireActiveChildSource();
		if (child) {
			syncChildLifecycleState(child);
			obs_source_release(child);
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
	lastVideoTime_.store(0, std::memory_order_relaxed);
	lastAudioTime_.store(0, std::memory_order_relaxed);
	lastKeyframeRequestTime_.store(0, std::memory_order_relaxed);
	logWarning("Use Native Receiver (Experimental) is enabled");
	connectionThread_ = std::thread(&VDONinjaSource::connectionThread, this);
}

void VDONinjaSource::disconnect()
{
	nativeRunning_ = false;
	connected_ = false;
	setObsSourceAudioActive(false);
	resetViewRetryState();

	// Tell the publisher to retire this viewer while its data channel is still
	// open. Closing signaling alone leaves the publisher's peer alive until ICE
	// times out, which prevents an OBS source toggle from reconnecting promptly.
	if (peerManager_) {
		peerManager_->sendDataToAll(R"({"bye":true})");
	}

	if (signaling_) {
		if (!settings_.streamId.empty()) {
			signaling_->stopViewing(settings_.streamId);
		}
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
		signaling_->setOnStreamRemoved(nullptr);
		signaling_->setOnPeerCleanup(nullptr);
		signaling_->setOnLifecycleEvent(nullptr);
	}

	if (peerManager_) {
		peerManager_->setOnPeerConnected(nullptr);
		peerManager_->setOnPeerDisconnected(nullptr);
		peerManager_->setOnTrack(nullptr);
		peerManager_->setOnDataChannel(nullptr);
		peerManager_->setOnDataChannelMessage(nullptr);
		peerManager_->setOnAcceptedSignalingLifecycleEvent(nullptr);
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

		peerManager_->setOnTrack([callbackState](const TrackSlotEvent &event) {
			AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			if (!guard) {
				return;
			}
			guard.owner()->handleTrackSlotEvent(event);
		});

		peerManager_->setOnPeerConnected([callbackState](const PeerEventIdentity &identity) {
			AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaSource *self = guard.owner();
			logInfo("Connected to publisher: %s", identity.uuid.c_str());
			self->markNativePeerConnectedIfReady(identity, "peer-connected");
		});

		peerManager_->setOnPeerDisconnected([callbackState](const PeerEventIdentity &identity) {
			AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			if (!guard) {
				return;
			}
			logInfo("Disconnected from publisher: %s", identity.uuid.c_str());
			guard.owner()->handlePeerDisconnected(identity);
		});

		peerManager_->setOnDataChannel(
		    [callbackState](const PeerEventIdentity &identity, std::shared_ptr<rtc::DataChannel> dc) {
			    AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			    if (!guard) {
				    return;
			    }
			    guard.owner()->handlePeerDataChannelOpen(identity, dc);
		    });

		peerManager_->setOnDataChannelMessage(
		    [callbackState](const PeerEventIdentity &identity, const std::string &message) {
			    AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			    if (!guard) {
				    return;
			    }
			    guard.owner()->handlePeerDataChannelMessage(identity, message);
		    });

		peerManager_->setOnAcceptedSignalingLifecycleEvent(
		    [callbackState](const AcceptedSignalingLifecycleEvent &event) {
			    AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			    if (!guard) {
				    return;
			    }
			    guard.owner()->handleAcceptedSignalingLifecycleEvent(event);
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
			logInfo("Disconnected from signaling server; existing native media remains active during reconnect");
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
			if (self->matchesTargetStreamId(streamId)) {
				logInfo("Target stream appeared in room, connecting...");
				self->requestViewStream("stream-added", false);
			}
		});
		configureSignalingLifecycleCallbacks(*signaling_, *peerManager_);

		signaling_->setAutoReconnect(settings_.autoReconnect, DEFAULT_RECONNECT_ATTEMPTS);

		if (!signaling_->connect(settings_.wssHost)) {
			logError("Failed to connect to signaling server");
			nativeRunning_ = false;
			return;
		}

		while (nativeRunning_.load()) {
			serviceViewRetry();
			// Release peers retired from RTC callbacks; without this an idle
			// viewer keeps disconnected PeerConnections (sockets, threads)
			// alive indefinitely.
			peerManager_->runDeferredCleanup();
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

VDONinjaPeerManager *VDONinjaSource::activePeerManager() const
{
	VDONinjaPeerManager *manager = peerManager_.get();
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (!manager) {
		manager = nativeMediaTestPeerManager_;
	}
#endif
	return manager;
}

void VDONinjaSource::sendViewerPreferencesToPeer(const std::string &uuid, const char *reason)
{
	VDONinjaPeerManager *manager = activePeerManager();
	if (uuid.empty() || !manager) {
		return;
	}

	const auto dimensions = outputDimensions();
	const std::string preferences = buildViewerRequestMessage(
	    dimensions.width, dimensions.height, !settings_.roomId.empty(), buildNativeViewerInfoJson(source_));
	manager->sendDataToPeer(uuid, preferences);
	logInfo("Sent viewer preferences to %s (%s): %s", uuid.c_str(), reason ? reason : "unknown", preferences.c_str());
}

void VDONinjaSource::sendViewerPreferencesToPeer(const PeerEventIdentity &identity, const char *reason)
{
	VDONinjaPeerManager *manager = activePeerManager();
	if (identity.uuid.empty() || identity.generation == 0 || !manager) {
		return;
	}

	const auto dimensions = outputDimensions();
	const std::string preferences = buildViewerRequestMessage(
	    dimensions.width, dimensions.height, !settings_.roomId.empty(), buildNativeViewerInfoJson(source_));
	manager->sendDataToPeer(identity, preferences);
	logInfo("Sent viewer preferences to %s generation %llu (%s): %s", identity.uuid.c_str(),
	        static_cast<unsigned long long>(identity.generation), reason ? reason : "unknown", preferences.c_str());
}

void VDONinjaSource::handlePeerDataChannelOpen(const PeerEventIdentity &identity,
                                               const std::shared_ptr<rtc::DataChannel> &dc)
{
	if (!dc || identity.uuid.empty()) {
		return;
	}
	{
		std::lock_guard<std::mutex> applyLock(trackEventApplyMutex_);
		if (!acceptPeerEventIdentityLocked(identity, false, PeerEventLane::DataChannelOpen)) {
			return;
		}
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	nativeMediaTestDataChannelOpenActions_.fetch_add(1, std::memory_order_acq_rel);
#endif
	sendViewerPreferencesToPeer(identity, "datachannel-open");
}

void VDONinjaSource::requestNativeTargetBitrate(const char *reason)
{
	std::shared_ptr<rtc::Track> currentVideoTrack;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		currentVideoTrack = videoTrack_;
	}

	const auto dimensions = outputDimensions();
	const unsigned int targetBitrateBps =
	    static_cast<unsigned int>(chooseViewerTargetBitrateKbps(dimensions.width, dimensions.height)) * 1000U;
	if (safeRequestBitrate(currentVideoTrack, targetBitrateBps, reason)) {
		logInfo("Requested native video REMB target of %u bps (%s)", targetBitrateBps, reason ? reason : "unknown");
	}
}

bool VDONinjaSource::acceptPeerEventIdentityLocked(const PeerEventIdentity &identity, bool terminalEvent,
                                                   PeerEventLane lane)
{
	if (identity.uuid.empty() || identity.generation == 0) {
		return false;
	}

	uint64_t retiredGeneration = 0;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		auto &state = peerEventStates_[identity.uuid];
		if (identity.generation < state.generation) {
			return false;
		}
		if (identity.generation > state.generation) {
			retiredGeneration = state.generation;
			state.generation = identity.generation;
			state.highestSequence = 0;
			state.lastDataMessageSequence = 0;
			state.lastDataChannelOpenSequence = 0;
			state.terminal = false;
			++state.seenGenerations;
		} else if (state.terminal) {
			return false;
		}
		if (identity.sequence != 0) {
			uint64_t *laneSequence = nullptr;
			if (lane == PeerEventLane::DataMessage) {
				laneSequence = &state.lastDataMessageSequence;
			} else if (lane == PeerEventLane::DataChannelOpen) {
				laneSequence = &state.lastDataChannelOpenSequence;
			}
			if (laneSequence && *laneSequence != 0 && identity.sequence <= *laneSequence) {
				return false;
			}
			if (laneSequence) {
				*laneSequence = identity.sequence;
			}
			state.highestSequence = std::max(state.highestSequence, identity.sequence);
		}
		if (terminalEvent) {
			state.terminal = true;
		}
	}

	// The first event from a replacement generation atomically retires all
	// source-owned media from the previous generation before the new event is
	// applied. trackEventApplyMutex_ serializes every peer-scoped source event,
	// so primary/alpha/audio can never straddle generations for one UUID.
	if (retiredGeneration != 0) {
		handlePeerDisconnectedAccepted({identity.uuid, "", retiredGeneration}, false);
		resetPeerGenerationSuppressionStateLocked(identity);
	}
	return true;
}

void VDONinjaSource::resetPeerGenerationSuppressionStateLocked(const PeerEventIdentity &identity)
{
	{
		std::lock_guard<std::mutex> commitStateLock(videoCommitStateMutex_);
		peerControlStates_[identity.uuid][identity.generation] = {};
	}
	publishActivePeerControlStateLocked("peer-generation-start");
}

void VDONinjaSource::publishActivePeerControlStateLocked(const char *reason)
{
	PeerEventIdentity owner;
	bool ownerHasAudio = false;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		if (!videoTrackPeerUuid_.empty()) {
			owner = {videoTrackPeerUuid_, "", videoTrackPeerGeneration_};
		} else if (!alphaVideoTrackPeerUuid_.empty()) {
			owner = {alphaVideoTrackPeerUuid_, "", alphaVideoTrackPeerGeneration_};
		} else if (!audioTrackPeerUuid_.empty()) {
			owner = {audioTrackPeerUuid_, "", audioTrackPeerGeneration_};
		}
		ownerHasAudio =
		    audioTrack_ && audioTrackPeerUuid_ == owner.uuid && audioTrackPeerGeneration_ == owner.generation;
	}

	PeerControlState stored;
	bool previousSuppressed = false;
	bool suppressed = false;
	{
		std::lock_guard<std::mutex> commitStateLock(videoCommitStateMutex_);
		if (!owner.uuid.empty() && owner.generation != 0) {
			const auto peerStates = peerControlStates_.find(owner.uuid);
			if (peerStates != peerControlStates_.end()) {
				const auto generationState = peerStates->second.find(owner.generation);
				if (generationState != peerStates->second.end()) {
					stored = generationState->second;
				}
			}
		}
		previousSuppressed = remoteVideoSuppressedState_;
		suppressed = stored.mediaVideoMuted || stored.directorVideoMuted || stored.virtualHangup;
		remoteAudioMuted_.store(stored.audioMuted, std::memory_order_relaxed);
		remoteMediaVideoMuted_.store(stored.mediaVideoMuted, std::memory_order_relaxed);
		remoteDirectorVideoMuted_.store(stored.directorVideoMuted, std::memory_order_relaxed);
		remoteVirtualHangup_.store(stored.virtualHangup, std::memory_order_relaxed);
		remoteVideoSuppressedState_ = suppressed;
		remoteVideoMuted_.store(suppressed, std::memory_order_release);
	}

	// Host/output callbacks are intentionally outside videoCommitStateMutex_.
	setObsSourceAudioActive(ownerHasAudio && !stored.audioMuted);
	if (previousSuppressed != suppressed) {
		logInfo("Native receiver owner state from %s: %s (%s)",
		        owner.uuid.empty() ? "no active peer" : owner.uuid.c_str(), suppressed ? "suppressed" : "restored",
		        reason ? reason : "peer-owner-transition");
	}
	if (suppressed) {
		clearNativeVideoOutput(reason ? reason : "peer-owner-transition");
	}
}

void VDONinjaSource::handlePeerDataChannelMessage(const PeerEventIdentity &identity, const std::string &message)
{
	constexpr size_t kMaxPreviewChars = 256;
	std::string preview = message;
	if (preview.size() > kMaxPreviewChars) {
		preview = preview.substr(0, kMaxPreviewChars) + "...(truncated)";
	}
	logInfo("Received source datachannel message from %s [generation %llu]: %s", identity.uuid.c_str(),
	        static_cast<unsigned long long>(identity.generation), preview.c_str());

	const DataMessage parsed = dataChannel_.parseMessage(message);
	VDONinjaPeerManager *manager = activePeerManager();
	std::string targetUuid;
	std::string targetSession;
	try {
		JsonParser raw(message);
		targetUuid = raw.getString("UUID");
		if (targetUuid.empty()) {
			targetUuid = raw.getString("uuid");
		}
		targetSession = raw.getString("session");
	} catch (const std::exception &e) {
		logWarning("Ignoring malformed source datachannel message from %s: %s", identity.uuid.c_str(), e.what());
		return;
	}
	if (parsed.type == DataMessageType::PeerBye && targetUuid.empty()) {
		handlePeerCleanupSignal(identity);
		return;
	}

	std::unique_lock<std::mutex> applyLock(trackEventApplyMutex_);
	if (!acceptPeerEventIdentityLocked(identity, false, PeerEventLane::DataMessage)) {
		logDebug("Ignoring stale datachannel message from %s generation %llu", identity.uuid.c_str(),
		         static_cast<unsigned long long>(identity.generation));
		return;
	}

	if (parsed.type == DataMessageType::Ping) {
		applyLock.unlock();
		if (manager) {
			manager->sendDataToPeer(identity.uuid, dataChannel_.createPongMessage(parsed.data));
		}
		return;
	}
	if (parsed.type == DataMessageType::Pong) {
		return;
	}

	std::optional<PeerEventIdentity> stateIdentity = identity;
	if (!targetUuid.empty()) {
		if (manager) {
			manager->bindViewerSignalingDataChannel(identity.uuid, targetUuid, targetSession);
			stateIdentity = manager->claimPeerEventIdentity(targetUuid);
		} else {
			stateIdentity = std::nullopt;
		}
		// A browser can use its established data channel to offer a separate
		// media peer. That peer does not exist until signaling applies the offer.
		// Controls still require an existing peer; existing peers retain their
		// session/generation checks, and the transport was validated above.
		if ((!stateIdentity && parsed.type != DataMessageType::Signaling) ||
		    (stateIdentity && ((!targetSession.empty() && stateIdentity->session != targetSession) ||
		                       !acceptPeerEventIdentityLocked(*stateIdentity, false, PeerEventLane::DataMessage)))) {
			return;
		}
	}

	if (parsed.type == DataMessageType::Mute) {
		const MuteStateUpdate muteUpdate = dataChannel_.parseMuteState(message);
		const ReceiverVideoSuppressionUpdate videoUpdate = dataChannel_.parseReceiverVideoSuppression(message);
		handlePeerControlState(*stateIdentity, &muteUpdate, &videoUpdate);
		return;
	}

	if (parsed.type == DataMessageType::DirectorVideoState) {
		const ReceiverVideoSuppressionUpdate videoUpdate = dataChannel_.parseReceiverVideoSuppression(message);
		handlePeerControlState(*stateIdentity, nullptr, &videoUpdate);
		return;
	}

	if (parsed.type == DataMessageType::Signaling) {
		applyLock.unlock();
		if (signaling_) {
			signaling_->processIncomingMessage(dataChannel_.prepareSignalingMessage(message, identity.uuid));
		}
		if (manager && !targetUuid.empty()) {
			// Processing the offer can create or replace the target peer. Never
			// send preferences using an identity captured before that transition.
			const auto resolvedIdentity = manager->claimPeerEventIdentity(targetUuid, targetSession);
			if (resolvedIdentity) {
				sendViewerPreferencesToPeer(*resolvedIdentity, "resolved-media-peer");
			}
		}
		return;
	}

	if (parsed.type == DataMessageType::PeerBye) {
		applyLock.unlock();
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		nativeMediaTestTargetedPeerByes_.fetch_add(1, std::memory_order_acq_rel);
#endif
		if (signaling_) {
			signaling_->processIncomingMessage(message);
		}
	}
}

void VDONinjaSource::handlePeerControlState(const PeerEventIdentity &identity, const MuteStateUpdate *muteUpdate,
                                            const ReceiverVideoSuppressionUpdate *videoUpdate)
{
	if (identity.uuid.empty() || identity.generation == 0 || (!muteUpdate && !videoUpdate)) {
		return;
	}
	const bool directorApplies = !videoUpdate || !videoUpdate->hasDirectorVideoMuted ||
	                             dataChannel_.receiverDirectorVideoAppliesToPeer(*videoUpdate, identity.uuid);
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (videoUpdate) {
		runNativeMediaTestStage(NativeMediaTestStage::SuppressionRequest, false, 0, mediaEpochGate_.capture());
	}
#endif
	bool activeOwner = false;
	bool hasActiveOwner = false;
	bool activeAudioOwner = false;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		hasActiveOwner =
		    !videoTrackPeerUuid_.empty() || !alphaVideoTrackPeerUuid_.empty() || !audioTrackPeerUuid_.empty();
		activeOwner =
		    (videoTrackPeerUuid_ == identity.uuid && videoTrackPeerGeneration_ == identity.generation) ||
		    (alphaVideoTrackPeerUuid_ == identity.uuid && alphaVideoTrackPeerGeneration_ == identity.generation) ||
		    (audioTrackPeerUuid_ == identity.uuid && audioTrackPeerGeneration_ == identity.generation);
		activeAudioOwner =
		    audioTrack_ && audioTrackPeerUuid_ == identity.uuid && audioTrackPeerGeneration_ == identity.generation;
	}
	// A state event may be the first accepted event for a replacement
	// generation, before that generation's track commit reaches the source. In
	// that ownerless interval it is authoritative immediately. When another
	// peer still owns media, however, only store this peer's state for a later
	// adoption; publishing it would let a deferred peer suppress the owner.
	const bool publishState = activeOwner || !hasActiveOwner;

	bool audioChanged = false;
	bool audioMuted = false;
	bool previousSuppressed = false;
	bool suppressed = false;
	bool videoPublished = false;
	const char *reason = "remote-video-state";
	{
		std::lock_guard<std::mutex> commitStateLock(videoCommitStateMutex_);
		auto &stored = peerControlStates_[identity.uuid][identity.generation];
		if (muteUpdate && muteUpdate->hasAudioMuted) {
			stored.audioMuted = muteUpdate->audioMuted;
		}
		if (videoUpdate) {
			if (videoUpdate->hasMediaVideoMuted) {
				stored.mediaVideoMuted = videoUpdate->mediaVideoMuted;
				reason = videoUpdate->mediaVideoMuted ? "remote-video-muted" : "remote-video-unmuted";
			}
			if (videoUpdate->hasDirectorVideoMuted && directorApplies) {
				stored.directorVideoMuted = videoUpdate->directorVideoMuted;
				reason = videoUpdate->directorVideoMuted ? "director-video-muted" : "director-video-unmuted";
			}
			if (videoUpdate->hasVirtualHangup) {
				stored.virtualHangup = videoUpdate->virtualHangup;
				reason = videoUpdate->virtualHangup ? "remote-virtual-hangup" : "remote-virtual-hangup-cleared";
			}
		}

		if (publishState) {
			audioMuted = stored.audioMuted;
			audioChanged = remoteAudioMuted_.load(std::memory_order_relaxed) != stored.audioMuted;
			previousSuppressed = remoteVideoSuppressedState_;
			suppressed = stored.mediaVideoMuted || stored.directorVideoMuted || stored.virtualHangup;
			remoteAudioMuted_.store(stored.audioMuted, std::memory_order_relaxed);
			remoteMediaVideoMuted_.store(stored.mediaVideoMuted, std::memory_order_relaxed);
			remoteDirectorVideoMuted_.store(stored.directorVideoMuted, std::memory_order_relaxed);
			remoteVirtualHangup_.store(stored.virtualHangup, std::memory_order_relaxed);
			remoteVideoSuppressedState_ = suppressed;
			remoteVideoMuted_.store(suppressed, std::memory_order_release);
			videoPublished = videoUpdate != nullptr;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
			if (videoUpdate) {
				runNativeMediaTestStage(NativeMediaTestStage::SuppressionAttempt, false, 0, mediaEpochGate_.capture());
			}
#endif
		}
	}

	if (videoUpdate && videoUpdate->hasDirectorVideoMuted && !directorApplies) {
		logDebug("Ignoring director video mute for non-local target %s from %s",
		         videoUpdate->directorVideoTarget.empty() ? "unknown" : videoUpdate->directorVideoTarget.c_str(),
		         identity.uuid.c_str());
	}
	if (!publishState) {
		return;
	}
	if (audioChanged) {
		logInfo("Native receiver remote audio mute from %s: %s", identity.uuid.c_str(),
		        audioMuted ? "muted" : "unmuted");
	}
	if (muteUpdate && muteUpdate->hasAudioMuted) {
		setObsSourceAudioActive(activeAudioOwner && !audioMuted);
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (videoPublished) {
		runNativeMediaTestStage(NativeMediaTestStage::MutePublished, false, 0, mediaEpochGate_.capture());
	}
#endif
	if (videoPublished && previousSuppressed != suppressed) {
		logInfo("Native receiver remote video output from %s: %s (%s)", identity.uuid.c_str(),
		        suppressed ? "suppressed" : "restored", reason);
	}
	if (videoPublished && suppressed) {
		clearNativeVideoOutput(reason);
	}
}

void VDONinjaSource::handleReceiverVideoSuppressionState(const std::string &uuid,
                                                         const ReceiverVideoSuppressionUpdate &update)
{
	const bool directorApplies =
	    !update.hasDirectorVideoMuted || dataChannel_.receiverDirectorVideoAppliesToPeer(update, uuid);
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::SuppressionRequest, false, 0, mediaEpochGate_.capture());
#endif
	bool changed = false;
	const char *reason = "remote-video-state";
	bool previousSuppressed = false;
	bool suppressed = false;
	{
		std::lock_guard<std::mutex> commitStateLock(videoCommitStateMutex_);
		if (update.hasMediaVideoMuted) {
			const bool previous = remoteMediaVideoMuted_.exchange(update.mediaVideoMuted, std::memory_order_relaxed);
			changed = changed || previous != update.mediaVideoMuted;
			reason = update.mediaVideoMuted ? "remote-video-muted" : "remote-video-unmuted";
		}
		if (update.hasDirectorVideoMuted && directorApplies) {
			const bool previous =
			    remoteDirectorVideoMuted_.exchange(update.directorVideoMuted, std::memory_order_relaxed);
			changed = changed || previous != update.directorVideoMuted;
			reason = update.directorVideoMuted ? "director-video-muted" : "director-video-unmuted";
		}
		if (update.hasVirtualHangup) {
			const bool previous = remoteVirtualHangup_.exchange(update.virtualHangup, std::memory_order_relaxed);
			changed = changed || previous != update.virtualHangup;
			reason = update.virtualHangup ? "remote-virtual-hangup" : "remote-virtual-hangup-cleared";
		}
		if (!changed) {
			return;
		}

		suppressed = remoteMediaVideoMuted_.load(std::memory_order_relaxed) ||
		             remoteDirectorVideoMuted_.load(std::memory_order_relaxed) ||
		             remoteVirtualHangup_.load(std::memory_order_relaxed);
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		runNativeMediaTestStage(NativeMediaTestStage::SuppressionAttempt, false, 0, mediaEpochGate_.capture());
#endif
		previousSuppressed = remoteVideoSuppressedState_;
		remoteVideoSuppressedState_ = suppressed;
		remoteVideoMuted_.store(suppressed, std::memory_order_release);
	}
	if (update.hasDirectorVideoMuted && !directorApplies) {
		logDebug("Ignoring director video mute for non-local target %s from %s",
		         update.directorVideoTarget.empty() ? "unknown" : update.directorVideoTarget.c_str(),
		         uuid.empty() ? "unknown peer" : uuid.c_str());
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::MutePublished, false, 0, mediaEpochGate_.capture());
#endif
	if (previousSuppressed != suppressed) {
		logInfo("Native receiver remote video output from %s: %s (%s)", uuid.empty() ? "unknown peer" : uuid.c_str(),
		        suppressed ? "suppressed" : "restored", reason ? reason : "remote-video-state");
	}
	if (suppressed) {
		clearNativeVideoOutput(reason ? reason : "remote-video-suppressed");
	}
}

void VDONinjaSource::requestViewStream(const char *reason, bool resetRetryCount)
{
	if (!nativeRunning_.load() || !signaling_ || !peerManager_ || settings_.streamId.empty() ||
	    !signaling_->isConnected()) {
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
			logWarning("Native receiver did not get a peer within %d ms; backing off before retry",
			           kViewRequestTimeoutMs);
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

void VDONinjaSource::configureSignalingLifecycleCallbacks(VDONinjaSignaling &signaling, VDONinjaPeerManager &manager)
{
	// The lifecycle envelope and legacy callbacks are additive at the signaling
	// layer. A receiver must install exactly one cleanup path or a single server
	// message can compete with itself after identity has been discarded.
	signaling.setOnStreamRemoved(nullptr);
	signaling.setOnPeerCleanup(nullptr);
	const auto callbackState = callbackState_;
	signaling.setOnLifecycleEvent([callbackState, &manager](const SignalingLifecycleEvent &event) {
		AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
		if (!guard) {
			return;
		}
		const auto disposition = manager.processSignalingLifecycleEvent(event);
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		if (disposition == SignalingLifecycleDisposition::AmbiguousSessionless) {
			guard.owner()->nativeMediaTestAmbiguousSessionlessCleanups_.fetch_add(1, std::memory_order_acq_rel);
		}
#else
		(void)disposition;
#endif
	});
}

void VDONinjaSource::handleAcceptedSignalingLifecycleEvent(const AcceptedSignalingLifecycleEvent &event)
{
	const char *reason = event.kind == SignalingLifecycleEventKind::StreamRemoved ? "stream-removed" : "peer-cleanup";
	if (event.kind == SignalingLifecycleEventKind::StreamRemoved) {
		logInfo("Native receiver accepted stream removal for %s generation %llu (stream=%s)",
		        event.identity.uuid.c_str(), static_cast<unsigned long long>(event.identity.generation),
		        event.streamId.empty() ? "unknown" : event.streamId.c_str());
	}
	(void)applyPeerCleanupSignal(event.identity, reason);
}

void VDONinjaSource::handleSignalingPeerCleanup(VDONinjaPeerManager &manager, const std::string &uuid,
                                                const std::string &session)
{
	bool ambiguousReuse = false;
	const auto claimedIdentity = manager.claimSignalingPeerCleanupIdentity(uuid, session, &ambiguousReuse);
	if (ambiguousReuse) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		nativeMediaTestAmbiguousSessionlessCleanups_.fetch_add(1, std::memory_order_acq_rel);
#endif
		logWarning("Ignoring ambiguous sessionless cleanup for peer %s with manager-observed UUID reuse", uuid.c_str());
		return;
	}
	if (claimedIdentity) {
		handlePeerCleanupSignal(*claimedIdentity);
	}
}

bool VDONinjaSource::applyPeerCleanupSignal(const PeerEventIdentity &identity, const char *reason)
{
	if (identity.uuid.empty()) {
		return false;
	}
	{
		std::lock_guard<std::mutex> applyLock(trackEventApplyMutex_);
		if (!acceptPeerEventIdentityLocked(identity, true)) {
			return false;
		}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		nativeMediaTestAcceptedPeerCleanups_.fetch_add(1, std::memory_order_acq_rel);
#endif

		logInfo("Native receiver got %s for peer %s generation %llu", reason ? reason : "cleanup/bye",
		        identity.uuid.c_str(), static_cast<unsigned long long>(identity.generation));
		handlePeerDisconnectedAccepted(identity, true);
	}
	return true;
}

void VDONinjaSource::handlePeerCleanupSignal(const PeerEventIdentity &identity)
{
	if (!applyPeerCleanupSignal(identity, "cleanup/bye")) {
		return;
	}
	if (peerManager_) {
		peerManager_->disconnectPeer(identity);
	}
}

bool VDONinjaSource::matchesTargetStreamId(const std::string &streamId) const
{
	if (streamId.empty() || settings_.streamId.empty()) {
		return false;
	}

	return streamId == settings_.streamId ||
	       hashStreamId(settings_.streamId, settings_.password, settings_.salt) == streamId ||
	       hashStreamId(settings_.streamId, DEFAULT_PASSWORD, settings_.salt) == streamId;
}

void VDONinjaSource::clearNativeVideoOutput(const char *reason)
{
	std::lock_guard<std::mutex> outputLock(videoOutputMutex_);
	clearNativeVideoOutputLocked(reason);
}

void VDONinjaSource::clearNativeVideoOutputLocked(const char *reason)
{
	const bool hadVideo = videoOutputActive_.exchange(false, std::memory_order_relaxed);
	if (hadVideo && reason && *reason) {
		logInfo("Clearing native video output (%s)", reason);
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (nativeMediaTestClearOutputHook_) {
		nativeMediaTestClearOutputHook_(hadVideo, reason ? reason : "");
		return;
	}
#endif
	if (source_) {
		obs_source_output_video(source_, nullptr);
	}
}

void VDONinjaSource::markNativePeerConnectedIfReady(const PeerEventIdentity &identity, const char *reason)
{
	if (identity.uuid.empty() || !peerManager_) {
		return;
	}
	std::lock_guard<std::mutex> applyLock(trackEventApplyMutex_);
	if (!acceptPeerEventIdentityLocked(identity)) {
		return;
	}
	markNativePeerConnectedIfReadyAccepted(identity, reason);
}

void VDONinjaSource::markNativePeerConnectedIfReadyAccepted(const PeerEventIdentity &identity, const char *reason)
{
	if (identity.uuid.empty() || !peerManager_) {
		return;
	}
	const std::string &uuid = identity.uuid;

	std::shared_ptr<rtc::Track> videoTrack;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		if (uuid != videoTrackPeerUuid_ && uuid != alphaVideoTrackPeerUuid_ && uuid != audioTrackPeerUuid_) {
			logInfo("Publisher %s connected without accepted native media yet (%s)", uuid.c_str(),
			        reason ? reason : "unknown");
			return;
		}
		if (uuid == videoTrackPeerUuid_) {
			videoTrack = videoTrack_;
		}
	}

	const auto currentIdentity = peerManager_->getPeerIdentity(uuid);
	if (!currentIdentity || currentIdentity->generation != identity.generation ||
	    peerManager_->getPeerState(uuid) != ConnectionState::Connected) {
		return;
	}

	connected_ = true;
	cancelViewRetry();
	{
		std::lock_guard<std::mutex> lock(retryStateMutex_);
		viewRetryCount_ = 0;
		awaitingPeerConnection_ = false;
		suppressViewerRetry_ = false;
	}
	sendViewerPreferencesToPeer(uuid, reason ? reason : "peer-connected");
	requestNativeTargetBitrate(reason ? reason : "peer-connected");
	if (safeRequestKeyframe(videoTrack, reason ? reason : "peer-connected")) {
		lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
		logInfo("Requested initial video keyframe for native receiver");
	}
}

void VDONinjaSource::handleStreamRemovedSignal(const std::string &streamId, const std::string &uuid)
{
	bool activePeerMatches = false;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		activePeerMatches = (!uuid.empty() && (uuid == videoTrackPeerUuid_ || uuid == alphaVideoTrackPeerUuid_ ||
		                                       uuid == audioTrackPeerUuid_));
	}

	if (!matchesTargetStreamId(streamId) && !activePeerMatches) {
		return;
	}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	nativeMediaTestLegacyStreamRemovalActions_.fetch_add(1, std::memory_order_acq_rel);
#endif

	logInfo("Native receiver got stream-removed for target stream (%s) from %s; clearing active receiver state",
	        streamId.empty() ? settings_.streamId.c_str() : streamId.c_str(),
	        uuid.empty() ? "unknown peer" : uuid.c_str());
	if (peerManager_) {
		peerManager_->stopViewing(settings_.streamId);
	}
	connected_ = false;
	resetNativeState();

	if (settings_.autoReconnect && nativeRunning_.load()) {
		int retryCount = 0;
		{
			std::lock_guard<std::mutex> lock(retryStateMutex_);
			retryCount = viewRetryCount_;
		}
		scheduleViewRetry("stream-removed", computeViewerPeerRecoveryDelayMs(retryCount), false);
	}
}

void VDONinjaSource::deferPeerTrackLocked(const PeerEventIdentity &identity, TrackType type,
                                          const std::shared_ptr<rtc::Track> &track)
{
	if (identity.uuid.empty() || identity.generation == 0 || !track) {
		return;
	}
	auto &bundle = pendingPeerTrackBundles_[identity.uuid][identity.generation];
	if (bundle.order == 0) {
		bundle.identity = identity;
		bundle.order = nextPendingPeerTrackOrder_++;
	}
	if (type == TrackType::Audio) {
		bundle.audio = track;
	} else if (type == TrackType::AlphaVideo) {
		bundle.alpha = track;
	} else {
		bundle.video = track;
	}
}

void VDONinjaSource::removePendingPeerTrackLocked(const TrackSlotEvent &event)
{
	const auto uuidIt = pendingPeerTrackBundles_.find(event.uuid);
	if (uuidIt == pendingPeerTrackBundles_.end()) {
		return;
	}
	const auto generationIt = uuidIt->second.find(event.generation);
	if (generationIt == uuidIt->second.end()) {
		return;
	}
	auto &bundle = generationIt->second;
	std::shared_ptr<rtc::Track> *slot = &bundle.video;
	if (event.type == TrackType::Audio) {
		slot = &bundle.audio;
	} else if (event.type == TrackType::AlphaVideo) {
		slot = &bundle.alpha;
	}
	if (!event.retiredTrack || *slot == event.retiredTrack) {
		slot->reset();
	}
	if (!bundle.video && !bundle.alpha && !bundle.audio) {
		uuidIt->second.erase(generationIt);
		if (uuidIt->second.empty()) {
			pendingPeerTrackBundles_.erase(uuidIt);
		}
	}
}

std::optional<PeerEventIdentity> VDONinjaSource::adoptNextPendingPeerBundleIfOwnerless(const char *reason)
{
	PendingPeerTrackBundle selected;
	bool found = false;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		if (videoTrack_ || alphaVideoTrack_ || audioTrack_) {
			return std::nullopt;
		}
		std::string selectedUuid;
		uint64_t selectedGeneration = 0;
		uint64_t selectedOrder = std::numeric_limits<uint64_t>::max();
		for (const auto &[uuid, generations] : pendingPeerTrackBundles_) {
			for (const auto &[generation, bundle] : generations) {
				if ((bundle.video || bundle.alpha || bundle.audio) && bundle.order < selectedOrder) {
					selectedUuid = uuid;
					selectedGeneration = generation;
					selectedOrder = bundle.order;
				}
			}
		}
		if (!selectedUuid.empty()) {
			auto uuidIt = pendingPeerTrackBundles_.find(selectedUuid);
			auto generationIt = uuidIt->second.find(selectedGeneration);
			selected = std::move(generationIt->second);
			uuidIt->second.erase(generationIt);
			if (uuidIt->second.empty()) {
				pendingPeerTrackBundles_.erase(uuidIt);
			}
			found = true;
		}
	}
	if (!found) {
		return std::nullopt;
	}

	peerTrackBundleAdoptionInProgress_.store(true, std::memory_order_release);
	logInfo("Adopting deferred native media bundle from %s generation %llu (%s)", selected.identity.uuid.c_str(),
	        static_cast<unsigned long long>(selected.identity.generation), reason ? reason : "owner-retired");
	try {
		if (selected.video) {
			onVideoTrack(selected.identity, selected.video);
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
			runNativeMediaTestStage(NativeMediaTestStage::PendingBundlePrimaryAttached, false, 0,
			                        mediaEpochGate_.capture());
#endif
		}
		if (selected.alpha) {
			onAlphaVideoTrack(selected.identity, selected.alpha);
		}
		if (selected.audio) {
			onAudioTrack(selected.identity, selected.audio);
		}
	} catch (...) {
		peerTrackBundleAdoptionInProgress_.store(false, std::memory_order_release);
		throw;
	}
	return selected.identity;
}

void VDONinjaSource::handleTrackSlotEvent(const TrackSlotEvent &event)
{
	std::lock_guard<std::mutex> applyLock(trackEventApplyMutex_);
	const PeerEventIdentity identity{event.uuid, event.session, event.generation, event.sequence};
	auto *positions = &videoTrackEventPositions_;
	if (event.type == TrackType::AlphaVideo) {
		positions = &alphaTrackEventPositions_;
	} else if (event.type == TrackType::Audio) {
		positions = &audioTrackEventPositions_;
	}
	auto &position = (*positions)[event.uuid];
	if (event.generation < position.generation ||
	    (event.generation == position.generation && event.revision <= position.revision)) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		nativeMediaTestRejectedTrackEvents_.fetch_add(1, std::memory_order_acq_rel);
#endif
		return;
	}
	if (!acceptPeerEventIdentityLocked({event.uuid, event.session, event.generation, event.sequence})) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		nativeMediaTestRejectedTrackEvents_.fetch_add(1, std::memory_order_acq_rel);
#endif
		return;
	}
	position = {event.generation, event.revision};

	if (event.track) {
		if (event.type == TrackType::Video) {
			onVideoTrack(identity, event.track);
		} else if (event.type == TrackType::AlphaVideo) {
			onAlphaVideoTrack(identity, event.track, event.retiredTrack);
		} else {
			onAudioTrack(identity, event.track);
		}
	} else if (event.type == TrackType::Video || event.type == TrackType::AlphaVideo) {
		handleVideoTrackClosed(identity, event.retiredTrack, event.type == TrackType::AlphaVideo,
		                       event.type == TrackType::AlphaVideo ? "manager-alpha-slot-retired"
		                                                           : "manager-video-slot-retired");
		{
			std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
			removePendingPeerTrackLocked(event);
		}
	} else {
		handleAudioTrackRetired(event);
	}
	std::optional<PeerEventIdentity> adoptedIdentity;
	if (!event.track) {
		adoptedIdentity = adoptNextPendingPeerBundleIfOwnerless("track-slot-retired");
	}

	// The source owns callback retirement after an accepted slot transition.
	// Never clear a handle which has already been re-added as any current or
	// deferred slot; stale/rejected events have no side effects above.
	bool retiredTrackIsCurrent = false;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		retiredTrackIsCurrent =
		    event.retiredTrack && (videoTrack_ == event.retiredTrack || alphaVideoTrack_ == event.retiredTrack ||
		                           audioTrack_ == event.retiredTrack);
		if (!retiredTrackIsCurrent && event.retiredTrack) {
			for (const auto &[uuid, generations] : pendingPeerTrackBundles_) {
				for (const auto &[generation, bundle] : generations) {
					if (bundle.video == event.retiredTrack || bundle.alpha == event.retiredTrack ||
					    bundle.audio == event.retiredTrack) {
						retiredTrackIsCurrent = true;
						break;
					}
				}
				if (retiredTrackIsCurrent) {
					break;
				}
			}
		}
	}
	if (!retiredTrackIsCurrent) {
		clearTrackCallbacks(event.retiredTrack);
	}
	publishActivePeerControlStateLocked("track-owner-transition");
	if (adoptedIdentity) {
		peerTrackBundleAdoptionInProgress_.store(false, std::memory_order_release);
		markNativePeerConnectedIfReadyAccepted(*adoptedIdentity, "deferred-peer-bundle-adopted");
	}
}

void VDONinjaSource::handleAudioTrackRetired(const TrackSlotEvent &event)
{
	{
		std::unique_lock<std::mutex> stateLock(nativeStateMutex_);
		std::unique_lock<std::mutex> audioDecodeLock(audioDecodeMutex_);
		if (audioTrackPeerUuid_ == event.uuid && audioTrackPeerGeneration_ == event.generation &&
		    (!event.retiredTrack || audioTrack_ == event.retiredTrack)) {
			audioTrack_.reset();
			audioTrackPeerUuid_.clear();
			audioTrackPeerGeneration_ = 0;
			resetAudioDecoder();
			loggedFirstAudioPacket_.store(false, std::memory_order_relaxed);
			loggedFirstDecodedAudioFrame_.store(false, std::memory_order_relaxed);
			lastAudioTime_.store(0, std::memory_order_relaxed);
		}
		removePendingPeerTrackLocked(event);
		connected_ = !videoTrackPeerUuid_.empty() || !alphaVideoTrackPeerUuid_.empty() || !audioTrackPeerUuid_.empty();
	}
}

void VDONinjaSource::onVideoTrack(const PeerEventIdentity &identity, std::shared_ptr<rtc::Track> track)
{
	const std::string &uuid = identity.uuid;
	logInfo("Received video track from %s", uuid.c_str());

	if (!track) {
		return;
	}

	const rtc::Description::Media description = track->description();
	const bool hasH264 = mediaDescriptionHasCodec(description, "h264");
	const bool hasVP9 = mediaDescriptionHasCodec(description, "vp9");
	if (!hasH264 && !hasVP9) {
		logError("Native receiver requires H.264 or VP9 video; offered codecs: %s",
		         describeMediaCodecs(description).c_str());
		return;
	}
	const NativeVideoCodec negotiatedCodec = hasVP9 ? NativeVideoCodec::VP9 : NativeVideoCodec::H264;

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
	std::shared_ptr<rtc::Track> replacedTrack;
	{
		// Canonical media-transition lock order. Packet callbacks never acquire
		// these locks in reverse order, and RTC/OBS calls happen after release.
		std::unique_lock<std::mutex> stateLock(nativeStateMutex_);
		std::unique_lock<std::mutex> videoAssemblyLock(videoAssemblyMutex_);
		std::unique_lock<std::mutex> videoDecodeLock(videoDecodeMutex_);
		std::unique_lock<std::mutex> alphaAssemblyLock(alphaAssemblyMutex_);
		std::unique_lock<std::mutex> alphaDecodeLock(alphaDecodeMutex_);
		std::unique_lock<std::mutex> outputLock(videoOutputMutex_);
		std::unique_lock<std::mutex> commitStateLock(videoCommitStateMutex_);
		std::unique_lock<std::mutex> pairingLock(alphaPairingMutex_);
		if (videoTrack_ == track) {
			return;
		}
		const std::string ownerUuid =
		    !videoTrackPeerUuid_.empty()
		        ? videoTrackPeerUuid_
		        : (!alphaVideoTrackPeerUuid_.empty() ? alphaVideoTrackPeerUuid_ : audioTrackPeerUuid_);
		const uint64_t ownerGeneration =
		    !videoTrackPeerUuid_.empty()
		        ? videoTrackPeerGeneration_
		        : (!alphaVideoTrackPeerUuid_.empty() ? alphaVideoTrackPeerGeneration_ : audioTrackPeerGeneration_);
		if (!ownerUuid.empty() && (ownerUuid != uuid || ownerGeneration != identity.generation)) {
			logWarning("Deferring native video track from %s while media peer %s remains active", uuid.c_str(),
			           ownerUuid.c_str());
			deferPeerTrackLocked(identity, TrackType::Video, track);
			return;
		}
		replacedExistingTrack = (videoTrack_ != nullptr);
		replacedTrack = videoTrack_;
		videoTrack_ = track;
		videoTrackPeerUuid_ = uuid;
		videoTrackPeerGeneration_ = identity.generation;
		videoRedPayloadTypes_ = redPayloadTypes;
		nativeVideoCodec_ = negotiatedCodec;
		resetMediaPipelineStateLocked();
		alphaTrackActive_.store(alphaVideoTrack_ != nullptr, std::memory_order_release);
		preferSoftwareVp9DecodeForAlpha_.store(alphaVideoTrack_ != nullptr, std::memory_order_release);
		if (replacedExistingTrack) {
			logInfo("Replacing native video track for peer %s; reset both media tracks to epoch %llu", uuid.c_str(),
			        static_cast<unsigned long long>(mediaEpochGate_.capture()));
			loggedFirstVideoRtpPacket_ = false;
			loggedFirstVideoPacket_ = false;
			loggedFirstDecodedVideoFrame_ = false;
			lastVideoTime_.store(0, std::memory_order_relaxed);
			lastKeyframeRequestTime_.store(0, std::memory_order_relaxed);
		}
	}
	clearTrackCallbacks(replacedTrack);
	completeMediaPipelineTransition(replacedExistingTrack ? "video-track-replaced" : "video-track-attached", true);
	auto rtxFilter = std::make_shared<RtxRepairMediaHandler>();
	const auto callbackState = callbackState_;
	auto receivingSession =
	    std::make_shared<InspectingReceivingSession>([callbackState](const rtc::message_ptr &message) {
		    runNoexceptCallback("native_video_receiving_session", [&]() {
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
			            static_cast<unsigned>(rtpHeader->payloadType()), payloadSize,
			            static_cast<unsigned>(rtpHeader->marker()), rtpHeader->timestamp());
		    });
	    });
	track->setMediaHandler(rtxFilter);
	track->chainMediaHandler(receivingSession);
	track->onMessage([callbackState, weakTrack = std::weak_ptr<rtc::Track>(track)](rtc::message_variant message) {
		runNoexceptCallback("native_video_track_onMessage", [&]() {
			AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaSource *self = guard.owner();
			if (self->peerTrackBundleAdoptionInProgress_.load(std::memory_order_acquire)) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
				self->runNativeMediaTestStage(NativeMediaTestStage::PendingBundlePacketRejected, false, 0,
				                              self->mediaEpochGate_.capture());
#endif
				return;
			}
			const auto strongTrack = weakTrack.lock();
			if (!strongTrack) {
				return;
			}
			uint64_t mediaEpoch = 0;
			{
				std::lock_guard<std::mutex> stateLock(self->nativeStateMutex_);
				if (self->videoTrack_ != strongTrack) {
					return;
				}
				mediaEpoch = self->mediaEpochGate_.capture();
			}

			if (!std::holds_alternative<rtc::binary>(message)) {
				return;
			}

			const auto &packet = std::get<rtc::binary>(message);
			self->processVideoRtpPacket(reinterpret_cast<const uint8_t *>(packet.data()), packet.size(), mediaEpoch);
		});
	});
	if (replacedExistingTrack && safeRequestKeyframe(track, "video-track-replaced")) {
		lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
		logInfo("Requested video keyframe after replacing native video track");
	}
	requestNativeTargetBitrate("video-track-attached");
	if (!peerTrackBundleAdoptionInProgress_.load(std::memory_order_acquire)) {
		markNativePeerConnectedIfReadyAccepted(identity, "video-track-attached");
	}
}

void VDONinjaSource::onAlphaVideoTrack(const PeerEventIdentity &identity, std::shared_ptr<rtc::Track> track,
                                       std::shared_ptr<rtc::Track> retiredTrack)
{
	const std::string &uuid = identity.uuid;
	logInfo("Received VP9 alpha video track from %s", uuid.c_str());

	if (!track) {
		handleVideoTrackClosed(identity, retiredTrack, true, "alpha-capability-removed");
		return;
	}

	std::shared_ptr<rtc::Track> currentVideoTrack;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		currentVideoTrack = videoTrack_;
	}
	const std::string currentVideoMid = currentVideoTrack ? currentVideoTrack->mid() : "";
	if (!currentVideoMid.empty() && !track->mid().empty() && track->mid() == currentVideoMid) {
		logInfo("Alpha track for %s reused primary video mid=%s; reattaching it as the native video track",
		        uuid.c_str(), track->mid().c_str());
		onVideoTrack(identity, track);
		return;
	}

	bool replacedExistingTrack = false;
	std::shared_ptr<rtc::Track> replacedTrack;
	{
		std::unique_lock<std::mutex> stateLock(nativeStateMutex_);
		std::unique_lock<std::mutex> videoAssemblyLock(videoAssemblyMutex_);
		std::unique_lock<std::mutex> videoDecodeLock(videoDecodeMutex_);
		std::unique_lock<std::mutex> alphaAssemblyLock(alphaAssemblyMutex_);
		std::unique_lock<std::mutex> alphaDecodeLock(alphaDecodeMutex_);
		std::unique_lock<std::mutex> outputLock(videoOutputMutex_);
		std::unique_lock<std::mutex> pairingLock(alphaPairingMutex_);
		if (alphaVideoTrack_ == track) {
			return;
		}
		const std::string ownerUuid =
		    !videoTrackPeerUuid_.empty()
		        ? videoTrackPeerUuid_
		        : (!alphaVideoTrackPeerUuid_.empty() ? alphaVideoTrackPeerUuid_ : audioTrackPeerUuid_);
		const uint64_t ownerGeneration =
		    !videoTrackPeerUuid_.empty()
		        ? videoTrackPeerGeneration_
		        : (!alphaVideoTrackPeerUuid_.empty() ? alphaVideoTrackPeerGeneration_ : audioTrackPeerGeneration_);
		if (!ownerUuid.empty() && (ownerUuid != uuid || ownerGeneration != identity.generation)) {
			logWarning("Deferring alpha video track from %s while peer %s remains active", uuid.c_str(),
			           ownerUuid.c_str());
			deferPeerTrackLocked(identity, TrackType::AlphaVideo, track);
			return;
		}
		replacedExistingTrack = (alphaVideoTrack_ != nullptr);
		replacedTrack = alphaVideoTrack_;
		alphaVideoTrack_ = track;
		alphaVideoTrackPeerUuid_ = uuid;
		alphaVideoTrackPeerGeneration_ = identity.generation;
		resetMediaPipelineStateLocked();
		alphaTrackActive_.store(true, std::memory_order_release);
		preferSoftwareVp9DecodeForAlpha_.store(true, std::memory_order_release);
		loggedAlphaSoftwareDecodeMode_.store(false, std::memory_order_relaxed);
		loggedAlphaCompositionActive_.store(false, std::memory_order_relaxed);
		loggedAlphaTimestampSyncWait_.store(false, std::memory_order_relaxed);
		loggedAlphaTimestampMiss_.store(false, std::memory_order_relaxed);
		loggedAlphaPixelFormatMismatch_.store(false, std::memory_order_relaxed);
		loggedAlphaDimensionMismatch_.store(false, std::memory_order_relaxed);
		loggedFirstAlphaRtpPacket_ = false;
	}
	clearTrackCallbacks(replacedTrack);
	completeMediaPipelineTransition(replacedExistingTrack ? "alpha-track-replaced" : "alpha-track-attached", true);
	if (replacedExistingTrack) {
		logInfo("Replacing VP9 alpha track for peer %s; reset both media tracks to epoch %llu", uuid.c_str(),
		        static_cast<unsigned long long>(mediaEpochGate_.capture()));
	} else {
		logInfo("Reset primary decoder so alpha composition uses software frames");
	}

	logInfo("Attaching native VP9 alpha video receive callbacks (mid=%s)", track->mid().c_str());

	const auto callbackState = callbackState_;
	track->onMessage([callbackState, weakTrack = std::weak_ptr<rtc::Track>(track)](rtc::message_variant message) {
		runNoexceptCallback("native_alpha_track_onMessage", [&]() {
			AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaSource *self = guard.owner();
			if (self->peerTrackBundleAdoptionInProgress_.load(std::memory_order_acquire)) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
				self->runNativeMediaTestStage(NativeMediaTestStage::PendingBundlePacketRejected, true, 0,
				                              self->mediaEpochGate_.capture());
#endif
				return;
			}
			const auto strongTrack = weakTrack.lock();
			if (!strongTrack) {
				return;
			}
			uint64_t mediaEpoch = 0;
			{
				std::lock_guard<std::mutex> stateLock(self->nativeStateMutex_);
				if (self->alphaVideoTrack_ != strongTrack) {
					return;
				}
				mediaEpoch = self->mediaEpochGate_.capture();
			}
			if (!std::holds_alternative<rtc::binary>(message)) {
				return;
			}
			const auto &packet = std::get<rtc::binary>(message);
			self->processAlphaRtpPacket(reinterpret_cast<const uint8_t *>(packet.data()), packet.size(), mediaEpoch);
		});
	});
	if (!peerTrackBundleAdoptionInProgress_.load(std::memory_order_acquire)) {
		markNativePeerConnectedIfReadyAccepted(identity, "alpha-track-attached");
	}
}

void VDONinjaSource::handleVideoTrackClosed(const PeerEventIdentity &identity, const std::shared_ptr<rtc::Track> &track,
                                            bool alphaTrack, const char *reason)
{
	const std::string &uuid = identity.uuid;
	std::shared_ptr<rtc::Track> removedTrack;
	std::shared_ptr<rtc::Track> remainingVideoTrack;
	bool removed = false;
	bool removedAlphaTrack = alphaTrack;

	{
		std::unique_lock<std::mutex> stateLock(nativeStateMutex_);
		std::unique_lock<std::mutex> videoAssemblyLock(videoAssemblyMutex_);
		std::unique_lock<std::mutex> videoDecodeLock(videoDecodeMutex_);
		std::unique_lock<std::mutex> alphaAssemblyLock(alphaAssemblyMutex_);
		std::unique_lock<std::mutex> alphaDecodeLock(alphaDecodeMutex_);
		std::unique_lock<std::mutex> outputLock(videoOutputMutex_);
		std::unique_lock<std::mutex> pairingLock(alphaPairingMutex_);

		if (alphaTrack && alphaVideoTrackPeerUuid_ == uuid && alphaVideoTrackPeerGeneration_ == identity.generation &&
		    clearSharedSlotIfMatches(alphaVideoTrack_, track)) {
			removedTrack = track;
			alphaVideoTrackPeerUuid_.clear();
			alphaVideoTrackPeerGeneration_ = 0;
			removed = static_cast<bool>(removedTrack);
		} else if (alphaTrack && videoTrackPeerUuid_ == uuid && videoTrackPeerGeneration_ == identity.generation &&
		           clearSharedSlotIfMatches(videoTrack_, track)) {
			// Some libdatachannel renegotiations report the alpha manager slot
			// using the primary mid. The source intentionally treats that handle
			// as primary, so its exact retirement must follow the handle too.
			removedTrack = track;
			videoTrackPeerUuid_.clear();
			videoTrackPeerGeneration_ = 0;
			videoRedPayloadTypes_.clear();
			removed = static_cast<bool>(removedTrack);
			removedAlphaTrack = false;
		} else {
			if (alphaTrack || videoTrackPeerUuid_ != uuid || videoTrackPeerGeneration_ != identity.generation ||
			    !clearSharedSlotIfMatches(videoTrack_, track)) {
				return;
			}
			removedTrack = track;
			videoTrackPeerUuid_.clear();
			videoTrackPeerGeneration_ = 0;
			videoRedPayloadTypes_.clear();
			removed = static_cast<bool>(removedTrack);
		}
		if (!removed) {
			return;
		}

		resetMediaPipelineStateLocked();
		alphaTrackActive_.store(alphaVideoTrack_ != nullptr, std::memory_order_release);
		preferSoftwareVp9DecodeForAlpha_.store(alphaVideoTrack_ != nullptr, std::memory_order_release);
		remainingVideoTrack = videoTrack_;

		connected_ = !videoTrackPeerUuid_.empty() || !alphaVideoTrackPeerUuid_.empty() || !audioTrackPeerUuid_.empty();
	}

	// A close callback may currently be executing on removedTrack, so callback
	// cleanup stays with the peer manager after this state transition returns.
	completeMediaPipelineTransition(reason ? reason : "video-track-transition", static_cast<bool>(remainingVideoTrack));
	logInfo("Native %s track for %s ended (%s); media epoch is now %llu", removedAlphaTrack ? "alpha" : "video",
	        uuid.c_str(), reason ? reason : "closed", static_cast<unsigned long long>(mediaEpochGate_.capture()));

	if (remainingVideoTrack && safeRequestKeyframe(remainingVideoTrack, reason ? reason : "track-transition")) {
		lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
	}
}

void VDONinjaSource::onAudioTrack(const PeerEventIdentity &identity, std::shared_ptr<rtc::Track> track)
{
	const std::string &uuid = identity.uuid;
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

	const int normalizedSampleRate = normalizeOpusSampleRate(sampleRate);
	const int normalizedChannels = normalizeOpusChannelCount(channels);
	if (sampleRate > 0 && sampleRate != normalizedSampleRate) {
		logWarning("Normalized native Opus sample rate from %d to %d", sampleRate, normalizedSampleRate);
	}
	if (channels > 0 && channels != normalizedChannels) {
		logWarning("Normalized native Opus channel count from %d to %d", channels, normalizedChannels);
	}

	bool replacedExistingTrack = false;
	std::shared_ptr<rtc::Track> replacedTrack;
	{
		std::unique_lock<std::mutex> stateLock(nativeStateMutex_);
		std::unique_lock<std::mutex> audioDecodeLock(audioDecodeMutex_);
		if (audioTrack_ == track) {
			return;
		}
		const std::string ownerUuid =
		    !videoTrackPeerUuid_.empty()
		        ? videoTrackPeerUuid_
		        : (!alphaVideoTrackPeerUuid_.empty() ? alphaVideoTrackPeerUuid_ : audioTrackPeerUuid_);
		const uint64_t ownerGeneration =
		    !videoTrackPeerUuid_.empty()
		        ? videoTrackPeerGeneration_
		        : (!alphaVideoTrackPeerUuid_.empty() ? alphaVideoTrackPeerGeneration_ : audioTrackPeerGeneration_);
		if (!ownerUuid.empty() && (ownerUuid != uuid || ownerGeneration != identity.generation)) {
			logWarning("Deferring native audio track from %s while media peer %s remains active", uuid.c_str(),
			           ownerUuid.c_str());
			deferPeerTrackLocked(identity, TrackType::Audio, track);
			return;
		}
		replacedExistingTrack = (audioTrack_ != nullptr);
		replacedTrack = audioTrack_;
		audioTrack_ = track;
		audioTrackPeerUuid_ = uuid;
		audioTrackPeerGeneration_ = identity.generation;
		if (replacedExistingTrack) {
			logInfo("Replacing native audio track for peer %s; resetting decoder state", uuid.c_str());
			resetAudioDecoder();
			loggedFirstAudioPacket_ = false;
			loggedFirstDecodedAudioFrame_ = false;
			loggedAudioDecodeSubmitFailure_ = false;
			lastAudioTime_.store(0, std::memory_order_relaxed);
		}
	}
	clearTrackCallbacks(replacedTrack);
	audioSampleRate_ = normalizedSampleRate;
	audioChannels_ = normalizedChannels;
	const auto callbackState = callbackState_;
	track->onMessage([callbackState, weakTrack = std::weak_ptr<rtc::Track>(track)](rtc::message_variant message) {
		runNoexceptCallback("native_audio_track_onMessage", [&]() {
			AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			if (!guard) {
				return;
			}
			VDONinjaSource *self = guard.owner();
			if (self->peerTrackBundleAdoptionInProgress_.load(std::memory_order_acquire)) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
				self->runNativeMediaTestStage(NativeMediaTestStage::PendingBundlePacketRejected, false, 0,
				                              self->mediaEpochGate_.capture());
#endif
				return;
			}
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
	});
	if (!peerTrackBundleAdoptionInProgress_.load(std::memory_order_acquire)) {
		markNativePeerConnectedIfReadyAccepted(identity, "audio-track-attached");
	}
}

std::shared_ptr<AVFrame> VDONinjaSource::retainVideoFrame(const AVFrame *frame)
{
	AVFrame *clonedFrame = frame ? av_frame_clone(frame) : nullptr;
	if (!clonedFrame) {
		return nullptr;
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	const auto retainedCount = nativeMediaTestRetainedVideoFrames_;
	retainedCount->fetch_add(1, std::memory_order_acq_rel);
	return std::shared_ptr<AVFrame>(clonedFrame, [retainedCount](AVFrame *ownedFrame) {
		av_frame_free(&ownedFrame);
		retainedCount->fetch_sub(1, std::memory_order_acq_rel);
	});
#else
	return std::shared_ptr<AVFrame>(clonedFrame, [](AVFrame *ownedFrame) { av_frame_free(&ownedFrame); });
#endif
}

int VDONinjaSource::sendVideoPacket(AVCodecContext *decoder, const AVPacket *packet)
{
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (nativeMediaTestSendPacketHook_) {
		return nativeMediaTestSendPacketHook_(decoder, packet);
	}
#endif
	return avcodec_send_packet(decoder, packet);
}

int VDONinjaSource::receiveVideoFrame(AVCodecContext *decoder, AVFrame *frame)
{
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (nativeMediaTestReceiveFrameHook_) {
		return nativeMediaTestReceiveFrameHook_(decoder, frame);
	}
#endif
	return avcodec_receive_frame(decoder, frame);
}

int VDONinjaSource::sendAlphaPacket(AVCodecContext *decoder, const AVPacket *packet)
{
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (nativeMediaTestAlphaSendPacketHook_) {
		return nativeMediaTestAlphaSendPacketHook_(decoder, packet);
	}
#endif
	return avcodec_send_packet(decoder, packet);
}

int VDONinjaSource::receiveAlphaFrame(AVCodecContext *decoder, AVFrame *frame)
{
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (nativeMediaTestAlphaReceiveFrameHook_) {
		return nativeMediaTestAlphaReceiveFrameHook_(decoder, frame);
	}
#endif
	return avcodec_receive_frame(decoder, frame);
}

bool VDONinjaSource::hasNativeVideoOutputTarget() const
{
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	return source_ != nullptr || static_cast<bool>(nativeMediaTestOutputHook_);
#else
	return source_ != nullptr;
#endif
}

VDONinjaSource::OutputDimensions VDONinjaSource::outputDimensions() const
{
	const uint64_t packed = outputDimensionsPacked_.load(std::memory_order_acquire);
	return {static_cast<uint32_t>(packed >> 32), static_cast<uint32_t>(packed)};
}

void VDONinjaSource::publishOutputDimensions(uint32_t width, uint32_t height)
{
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::DimensionUpdateMidpoint, false, 0, mediaEpochGate_.capture());
#endif
	const uint64_t packed = (static_cast<uint64_t>(width) << 32) | static_cast<uint64_t>(height);
	outputDimensionsPacked_.store(packed, std::memory_order_release);
}

void VDONinjaSource::processVideoData(const uint8_t *data, size_t size, uint32_t rtpTimestamp, uint64_t mediaEpoch)
{
	if (!nativeRunning_.load() || !data || size == 0 || !mediaEpochGate_.isCurrent(mediaEpoch)) {
		return;
	}

	if (!loggedFirstVideoPacket_.exchange(true)) {
		logInfo("Native receiver got first depacketized video payload (%zu bytes, rtp ts=%u)", size, rtpTimestamp);
	}

	std::shared_ptr<rtc::Track> currentVideoTrack;
	NativeVideoCodec codec = NativeVideoCodec::H264;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		if (!mediaEpochGate_.isCurrent(mediaEpoch)) {
			return;
		}
		currentVideoTrack = videoTrack_;
		codec = nativeVideoCodec_;
	}
	const char *codecName = codec == NativeVideoCodec::VP9 ? "VP9" : "H.264";

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::PreDecode, false, rtpTimestamp, mediaEpoch);
#endif

	std::vector<std::pair<std::shared_ptr<AVFrame>, uint32_t>> decodedFrames;
	{
		std::lock_guard<std::mutex> lock(videoDecodeMutex_);
		if (!mediaEpochGate_.isCurrent(mediaEpoch)) {
			return;
		}
		if (!initializeVideoDecoder()) {
			return;
		}

		av_packet_unref(videoPacket_);
		const int allocResult = av_new_packet(videoPacket_, static_cast<int>(size));
		if (allocResult < 0) {
			logError("Failed to allocate %s packet buffer: %s", codecName, ffmpegErrorString(allocResult).c_str());
			return;
		}

		std::memcpy(videoPacket_->data, data, size);
		videoPacket_->pts = static_cast<int64_t>(rtpTimestamp);
		videoPacket_->dts = static_cast<int64_t>(rtpTimestamp);
		const auto drainDecodedFrames = [&]() -> bool {
			while (true) {
				const int receiveResult = receiveVideoFrame(videoDecoder_, videoFrame_);
				if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
					return true;
				}
				if (receiveResult < 0) {
					logWarning("Failed to decode %s frame: %s", codecName, ffmpegErrorString(receiveResult).c_str());
					if (safeRequestKeyframe(currentVideoTrack, "decode-failure")) {
						lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
					}
					return true;
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
				if (videoHwDecodeConfigured_ && videoHwPixelFormat_ != AV_PIX_FMT_NONE &&
				    videoFrame_->format == videoHwPixelFormat_) {
					av_frame_unref(videoTransferFrame_);
					const int transferResult = av_hwframe_transfer_data(videoTransferFrame_, videoFrame_, 0);
					if (transferResult < 0) {
						logWarning("Failed to transfer hardware-decoded %s frame from %s: %s; disabling hardware "
						           "decode for this session",
						           codecName, videoHwDeviceName_.c_str(), ffmpegErrorString(transferResult).c_str());
						videoHwDecodeDisabled_ = true;
						resetVideoDecoder();
						if (safeRequestKeyframe(currentVideoTrack, "hw-transfer-failure")) {
							lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
						}
						return false;
					}
					av_frame_copy_props(videoTransferFrame_, videoFrame_);
					frameToOutput = videoTransferFrame_;
				}

				const auto decodedTimestamp =
				    resolveDecodedRtpTimestamp(frameToOutput->pts, frameToOutput->best_effort_timestamp);
				if (!decodedTimestamp && alphaTrackActive_.load(std::memory_order_relaxed)) {
					logWarning(
					    "Dropping primary video frame without a decoder-preserved RTP timestamp while alpha is active");
				} else {
					const uint32_t decodedRtpTimestamp = decodedTimestamp.value_or(rtpTimestamp);
					auto retainedFrame = retainVideoFrame(frameToOutput);
					if (retainedFrame) {
						decodedFrames.emplace_back(std::move(retainedFrame), decodedRtpTimestamp);
					} else {
						logWarning("Failed to retain decoded %s frame for serialized output", codecName);
					}
				}
				av_frame_unref(videoFrame_);
				if (frameToOutput == videoTransferFrame_) {
					av_frame_unref(videoTransferFrame_);
				}
			}
		};

		int sendResult = sendVideoPacket(videoDecoder_, videoPacket_);
		bool canContinue = true;
		if (sendResult == AVERROR(EAGAIN)) {
			canContinue = drainDecodedFrames();
			if (canContinue) {
				sendResult = sendVideoPacket(videoDecoder_, videoPacket_);
			}
		}
		if (canContinue && sendResult < 0) {
			if (!loggedVideoDecodeSubmitFailure_.exchange(true, std::memory_order_relaxed)) {
				logWarning("Failed to submit %s packet before a decodable keyframe arrived: %s", codecName,
				           ffmpegErrorString(sendResult).c_str());
			}
			const int64_t now = currentTimeMs();
			const int64_t lastKeyframeRequestTime = lastKeyframeRequestTime_.load(std::memory_order_relaxed);
			if ((lastKeyframeRequestTime == 0 || now - lastKeyframeRequestTime >= 1000) &&
			    safeRequestKeyframe(currentVideoTrack, "send-packet-failure")) {
				lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
			}
			canContinue = false;
		}
		if (canContinue) {
			drainDecodedFrames();
		}
	}

	for (const auto &decodedFrame : decodedFrames) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		runNativeMediaTestStage(NativeMediaTestStage::PrePair, false, decodedFrame.second, mediaEpoch);
#endif
		if (mediaEpochGate_.isCurrent(mediaEpoch)) {
			outputDecodedVideoFrame(decodedFrame.first.get(), decodedFrame.second, mediaEpoch);
		}
	}
}

void VDONinjaSource::processVideoRtpPacket(const uint8_t *packetData, size_t packetSize, uint64_t mediaEpoch)
{
	if (!nativeRunning_.load() || !packetData || packetSize < sizeof(rtc::RtpHeader) ||
	    !mediaEpochGate_.isCurrent(mediaEpoch)) {
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
	NativeVideoCodec codec;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		if (!mediaEpochGate_.isCurrent(mediaEpoch)) {
			return;
		}
		codec = nativeVideoCodec_;
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

	if (codec == NativeVideoCodec::VP9) {
		processVP9RtpPacket(payload, payloadSize, rtpHeader->timestamp(), mediaEpoch);
		return;
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
				const size_t naluSize =
				    (static_cast<size_t>(payload[offset]) << 8) | static_cast<size_t>(payload[offset + 1]);
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
		if (!mediaEpochGate_.isCurrent(mediaEpoch)) {
			return;
		}
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
			processVideoData(frame.first.data(), frame.first.size(), frame.second, mediaEpoch);
		}
	}
}

void VDONinjaSource::processVP9RtpPacket(const uint8_t *payload, size_t payloadSize, uint32_t rtpTimestamp,
                                         uint64_t mediaEpoch)
{
	if (payloadSize == 0 || !mediaEpochGate_.isCurrent(mediaEpoch)) {
		return;
	}

	const auto desc = parseVP9PayloadDescriptor(payload, payloadSize);
	if (!desc.valid) {
		return;
	}
	if (desc.payloadOffset >= payloadSize) {
		logWarning("VP9 RTP payload descriptor consumed entire payload (payload=%zu, offset=%zu, ts=%u)", payloadSize,
		           desc.payloadOffset, rtpTimestamp);
		return;
	}

	const uint8_t *vpData = payload + desc.payloadOffset;
	const size_t vpSize = payloadSize - desc.payloadOffset;

	std::vector<std::pair<std::vector<uint8_t>, uint32_t>> completedFrames;

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::PreAssembly, false, rtpTimestamp, mediaEpoch);
#endif

	{
		std::lock_guard<std::mutex> lock(videoAssemblyMutex_);
		if (!mediaEpochGate_.isCurrent(mediaEpoch)) {
			return;
		}

		// Start of frame: discard any incomplete prior frame and begin fresh.
		if (desc.startOfFrame) {
			if (videoAssemblyActive_ && !videoAssemblyBuffer_.empty()) {
				logWarning("VP9 B=1 received before E=1 for previous frame; discarding %zu bytes",
				           videoAssemblyBuffer_.size());
			}
			videoAssemblyBuffer_.clear();
			videoAssemblyActive_ = true;
			videoAssemblyTimestamp_ = rtpTimestamp;
		}

		if (!videoAssemblyActive_) {
			// Mid-frame packet arrived before we saw a B=1 — skip until next keyframe.
			return;
		}

		videoAssemblyBuffer_.insert(videoAssemblyBuffer_.end(), vpData, vpData + vpSize);

		if (desc.endOfFrame && !videoAssemblyBuffer_.empty()) {
			completedFrames.emplace_back(std::move(videoAssemblyBuffer_), videoAssemblyTimestamp_);
			videoAssemblyBuffer_.clear();
			videoAssemblyActive_ = false;
		}
	}

	for (const auto &frame : completedFrames) {
		if (!frame.first.empty()) {
			processVideoData(frame.first.data(), frame.first.size(), frame.second, mediaEpoch);
		}
	}
}

void VDONinjaSource::processAlphaRtpPacket(const uint8_t *packetData, size_t packetSize, uint64_t mediaEpoch)
{
	if (!nativeRunning_.load() || !packetData || packetSize < sizeof(rtc::RtpHeader) ||
	    !mediaEpochGate_.isCurrent(mediaEpoch)) {
		return;
	}

	const auto *rtpHeader = reinterpret_cast<const rtc::RtpHeader *>(packetData);
	const auto payloadView = parseRtpPayloadView(packetData, packetSize);
	if (!payloadView || payloadView->size == 0) {
		return;
	}

	if (!loggedFirstAlphaRtpPacket_.exchange(true)) {
		logInfo("Native receiver got first alpha video RTP packet (pt=%u, bytes=%zu, rtp ts=%u)",
		        static_cast<unsigned>(rtpHeader->payloadType()), payloadView->size, rtpHeader->timestamp());
	}

	processAlphaVP9RtpPacket(packetData + payloadView->offset, payloadView->size, rtpHeader->timestamp(), mediaEpoch);
}

void VDONinjaSource::processAlphaVP9RtpPacket(const uint8_t *payload, size_t payloadSize, uint32_t rtpTimestamp,
                                              uint64_t mediaEpoch)
{
	if (payloadSize == 0 || !mediaEpochGate_.isCurrent(mediaEpoch)) {
		return;
	}

	const auto desc = parseVP9PayloadDescriptor(payload, payloadSize);
	if (!desc.valid) {
		return;
	}
	if (desc.payloadOffset >= payloadSize) {
		logWarning("VP9 alpha RTP payload descriptor consumed entire payload (payload=%zu, offset=%zu, ts=%u)",
		           payloadSize, desc.payloadOffset, rtpTimestamp);
		return;
	}

	const uint8_t *vpData = payload + desc.payloadOffset;
	const size_t vpSize = payloadSize - desc.payloadOffset;

	std::vector<std::pair<std::vector<uint8_t>, uint32_t>> completedFrames;

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::PreAssembly, true, rtpTimestamp, mediaEpoch);
#endif

	{
		std::lock_guard<std::mutex> lock(alphaAssemblyMutex_);
		if (!mediaEpochGate_.isCurrent(mediaEpoch)) {
			return;
		}

		if (desc.startOfFrame) {
			if (alphaAssemblyActive_ && !alphaAssemblyBuffer_.empty()) {
				logWarning("VP9 alpha B=1 received before E=1 for previous frame; discarding %zu bytes",
				           alphaAssemblyBuffer_.size());
			}
			alphaAssemblyBuffer_.clear();
			alphaAssemblyActive_ = true;
			alphaAssemblyTimestamp_ = rtpTimestamp;
		}

		if (!alphaAssemblyActive_) {
			return;
		}

		alphaAssemblyBuffer_.insert(alphaAssemblyBuffer_.end(), vpData, vpData + vpSize);

		if (desc.endOfFrame && !alphaAssemblyBuffer_.empty()) {
			completedFrames.emplace_back(std::move(alphaAssemblyBuffer_), alphaAssemblyTimestamp_);
			alphaAssemblyBuffer_.clear();
			alphaAssemblyActive_ = false;
		}
	}

	for (const auto &frame : completedFrames) {
		if (!frame.first.empty()) {
			processAlphaVideoData(frame.first.data(), frame.first.size(), frame.second, mediaEpoch);
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
		if (!loggedAudioDecodeSubmitFailure_.exchange(true, std::memory_order_relaxed)) {
			logWarning("Failed to submit Opus packet: %s", ffmpegErrorString(sendResult).c_str());
		}
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

	lastAudioTime_.store(currentTimeMs(), std::memory_order_relaxed);
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

	if (!usingNativeReceiver() || !active_.load()) {
		return;
	}

	const int64_t now = currentTimeMs();
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	bool staleCandidate = false;
	{
		std::lock_guard<std::mutex> outputLock(videoOutputMutex_);
		const int64_t candidateTime = lastVideoTime_.load(std::memory_order_relaxed);
		staleCandidate = videoOutputActive_.load(std::memory_order_relaxed) && candidateTime != 0 &&
		                 now - candidateTime >= kNativeVideoStallBlankMs;
	}
	if (staleCandidate) {
		runNativeMediaTestStage(NativeMediaTestStage::PreStallClear, false, 0, mediaEpochGate_.capture());
	}
#endif
	int64_t lastVideoTime = 0;
	{
		std::lock_guard<std::mutex> outputLock(videoOutputMutex_);
		lastVideoTime = lastVideoTime_.load(std::memory_order_relaxed);
		if (videoOutputActive_.load(std::memory_order_relaxed) && lastVideoTime != 0 &&
		    now - lastVideoTime >= kNativeVideoStallBlankMs) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
			// The observation latch runs before this lock is reacquired so a fresh
			// commit can win; the age is deliberately re-evaluated here.
#endif
			if (!loggedVideoStallClear_.exchange(true, std::memory_order_relaxed)) {
				logWarning("No native video packets for %lld ms; clearing stale frame",
				           static_cast<long long>(now - lastVideoTime));
			}
			clearNativeVideoOutputLocked("stale-video-timeout");
		}
	}

	if (!connected_.load() || !videoTrack) {
		return;
	}

	if (lastVideoTime != 0 && now - lastVideoTime < 1500) {
		return;
	}
	const int64_t lastKeyframeRequestTime = lastKeyframeRequestTime_.load(std::memory_order_relaxed);
	if (lastKeyframeRequestTime != 0 && now - lastKeyframeRequestTime < 1000) {
		return;
	}

	if (safeRequestKeyframe(videoTrack, "video-tick")) {
		lastKeyframeRequestTime_.store(now, std::memory_order_relaxed);
		logInfo("Requested video keyframe for native receiver");
	}
}

void VDONinjaSource::videoRender(gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	if (isInternalNativeSource()) {
		return;
	}

	obs_source_t *child = acquireActiveChildSource();
	if (child) {
		obs_source_video_render(child);
		obs_source_release(child);
	}
}

uint32_t VDONinjaSource::getWidth() const
{
	return outputDimensions().width;
}

uint32_t VDONinjaSource::getHeight() const
{
	return outputDimensions().height;
}

bool VDONinjaSource::isConnected() const
{
	if (isInternalNativeSource()) {
		return connected_.load();
	}

	obs_source_t *child = acquireActiveChildSource();
	const bool connected = child != nullptr && !settings_.streamId.empty();
	if (child) {
		obs_source_release(child);
	}
	return connected;
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

	const bool isVP9 = (nativeVideoCodec_ == NativeVideoCodec::VP9);
	const AVCodecID codecId = isVP9 ? AV_CODEC_ID_VP9 : AV_CODEC_ID_H264;
	const char *codecName = isVP9 ? "VP9" : "H.264";
	const bool preferSoftwareForAlpha = preferSoftwareVp9DecodeForAlpha_.load(std::memory_order_relaxed);

	const AVCodec *codec = avcodec_find_decoder(codecId);
	if (!codec) {
		logError("FFmpeg %s decoder is unavailable", codecName);
		return false;
	}

	videoDecoder_ = avcodec_alloc_context3(codec);
	videoFrame_ = av_frame_alloc();
	videoTransferFrame_ = av_frame_alloc();
	videoPacket_ = av_packet_alloc();
	if (!videoDecoder_ || !videoFrame_ || !videoTransferFrame_ || !videoPacket_) {
		logError("Failed to allocate native %s decoder state", codecName);
		resetVideoDecoder();
		return false;
	}
	videoDecoder_->pkt_timebase = AVRational{1, 90000};

	videoHwDecodeConfigured_ = false;
	videoHwStatusLogged_ = false;
	videoHwPixelFormat_ = AV_PIX_FMT_NONE;
	videoHwDeviceName_.clear();
	if (preferSoftwareForAlpha) {
		if (!loggedAlphaSoftwareDecodeMode_.exchange(true, std::memory_order_relaxed)) {
			logInfo("VP9 alpha track active; using software decode for compositable primary frames");
		}
	} else if (!videoHwDecodeDisabled_) {
		videoHwDecodeConfigured_ =
		    configureVideoHardwareDecoder(videoDecoder_, codec, videoHwPixelFormat_, videoHwDeviceName_, isVP9);
	}
	if (!videoHwDecodeConfigured_) {
		videoDecoder_->thread_count = 0;
		videoDecoder_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	nativeMediaTestPrimaryRequestedThreadCount_ = videoDecoder_->thread_count;
	nativeMediaTestPrimaryRequestedThreadType_ = videoDecoder_->thread_type;
#endif

	const int openResult = avcodec_open2(videoDecoder_, codec, nullptr);
	if (openResult < 0) {
		if (videoHwDecodeConfigured_) {
			logWarning("Failed to open %s decoder with %s hardware acceleration: %s; falling back to software decode",
			           codecName, videoHwDeviceName_.c_str(), ffmpegErrorString(openResult).c_str());
			videoHwDecodeDisabled_ = true;
			resetVideoDecoder();
			return initializeVideoDecoder();
		}
		logError("Failed to open %s decoder: %s", codecName, ffmpegErrorString(openResult).c_str());
		resetVideoDecoder();
		return false;
	}

	if (videoHwDecodeConfigured_) {
		logInfo("Initialized native %s decoder with hardware acceleration backend %s", codecName,
		        videoHwDeviceName_.c_str());
	} else {
		logInfo("Initialized native %s decoder in software mode", codecName);
	}

	return true;
}

bool VDONinjaSource::initializeAudioDecoder(int sampleRate, int channels)
{
	sampleRate = normalizeOpusSampleRate(sampleRate);
	channels = normalizeOpusChannelCount(channels);

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

	audioSampleRate_ = sampleRate;
	audioChannels_ = channels;
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
	std::unique_lock<std::mutex> outputLock(videoOutputMutex_);
	std::unique_lock<std::mutex> pairingLock(alphaPairingMutex_);
	alphaFrameSynchronizer_.reset();
	videoTimestampMapper_.reset();
	resetVideoDecoderStorageLocked();
}

void VDONinjaSource::resetVideoDecoderStorageLocked()
{
	if (videoScaleContext_) {
		sws_freeContext(videoScaleContext_);
		videoScaleContext_ = nullptr;
	}
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

	lastDecodedVideoWidth_ = 0;
	lastDecodedVideoHeight_ = 0;
	loggedVideoDecodeSubmitFailure_.store(false, std::memory_order_relaxed);
	videoHwDecodeConfigured_ = false;
	videoHwStatusLogged_ = false;
	videoHwPixelFormat_ = AV_PIX_FMT_NONE;
	videoHwDeviceName_.clear();
}

void VDONinjaSource::resetAlphaDecoder()
{
	std::unique_lock<std::mutex> outputLock(videoOutputMutex_);
	std::unique_lock<std::mutex> pairingLock(alphaPairingMutex_);
	alphaFrameSynchronizer_.reset();
	videoTimestampMapper_.reset();
	resetAlphaDecoderStorageLocked();
}

void VDONinjaSource::resetAlphaDecoderStorageLocked()
{
	if (alphaPacket_) {
		av_packet_free(&alphaPacket_);
	}
	if (alphaFrame_) {
		av_frame_free(&alphaFrame_);
	}
	if (alphaDecoder_) {
		avcodec_free_context(&alphaDecoder_);
	}
}

void VDONinjaSource::resetMediaPipelineStateLocked()
{
	outputMediaEpoch_.store(0, std::memory_order_release);
	mediaEpochGate_.advance();
	videoAssemblyBuffer_.clear();
	videoAssemblyTimestamp_ = 0;
	videoAssemblyActive_ = false;
	alphaAssemblyBuffer_.clear();
	alphaAssemblyTimestamp_ = 0;
	alphaAssemblyActive_ = false;
	resetVideoDecoderStorageLocked();
	resetAlphaDecoderStorageLocked();
	alphaFrameSynchronizer_.reset();
	videoTimestampMapper_.reset();
}

void VDONinjaSource::completeMediaPipelineTransition(const char *reason, bool enableOutput)
{
	std::lock_guard<std::mutex> outputLock(videoOutputMutex_);
	clearNativeVideoOutputLocked(reason ? reason : "media-pipeline-transition");
	outputMediaEpoch_.store(enableOutput ? mediaEpochGate_.capture() : 0, std::memory_order_release);
}

bool VDONinjaSource::initializeAlphaDecoder()
{
	if (alphaDecoder_) {
		return true;
	}

	// Alpha channel is always decoded in software: HW decoders output NV12 and
	// do not expose a separate Y plane suitable for alpha extraction.
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_VP9);
	if (!codec) {
		logError("FFmpeg VP9 decoder unavailable for alpha channel");
		return false;
	}

	alphaDecoder_ = avcodec_alloc_context3(codec);
	alphaFrame_ = av_frame_alloc();
	alphaPacket_ = av_packet_alloc();
	if (!alphaDecoder_ || !alphaFrame_ || !alphaPacket_) {
		logError("Failed to allocate VP9 alpha decoder state");
		resetAlphaDecoder();
		return false;
	}
	alphaDecoder_->pkt_timebase = AVRational{1, 90000};
	alphaDecoder_->thread_count = 0;
	alphaDecoder_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	nativeMediaTestAlphaRequestedThreadCount_ = alphaDecoder_->thread_count;
	nativeMediaTestAlphaRequestedThreadType_ = alphaDecoder_->thread_type;
#endif

	const int openResult = avcodec_open2(alphaDecoder_, codec, nullptr);
	if (openResult < 0) {
		logError("Failed to open VP9 alpha decoder: %s", ffmpegErrorString(openResult).c_str());
		resetAlphaDecoder();
		return false;
	}

	logInfo("VP9 alpha decoder initialized (software libvpx-vp9)");
	return true;
}

void VDONinjaSource::processAlphaVideoData(const uint8_t *data, size_t size, uint32_t rtpTimestamp, uint64_t mediaEpoch)
{
	if (!nativeRunning_.load() || !data || size == 0 || !mediaEpochGate_.isCurrent(mediaEpoch)) {
		return;
	}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::PreDecode, true, rtpTimestamp, mediaEpoch);
#endif

	std::vector<PendingAlphaFrame> decodedFrames;
	std::unique_lock<std::mutex> lock(alphaDecodeMutex_);
	if (!mediaEpochGate_.isCurrent(mediaEpoch)) {
		return;
	}
	if (!initializeAlphaDecoder()) {
		return;
	}

	av_packet_unref(alphaPacket_);
	const int allocResult = av_new_packet(alphaPacket_, static_cast<int>(size));
	if (allocResult < 0) {
		return;
	}

	std::memcpy(alphaPacket_->data, data, size);
	alphaPacket_->pts = static_cast<int64_t>(rtpTimestamp);
	alphaPacket_->dts = static_cast<int64_t>(rtpTimestamp);
	const auto drainDecodedFrames = [&]() {
		while (true) {
			const int receiveResult = receiveAlphaFrame(alphaDecoder_, alphaFrame_);
			if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
				return;
			}
			if (receiveResult < 0) {
				if (!loggedAlphaDecodeReceiveFailure_.exchange(true, std::memory_order_relaxed)) {
					logWarning("Failed to decode VP9 alpha frame: %s", ffmpegErrorString(receiveResult).c_str());
				}
				return;
			}

			// We only need the Y plane — it carries the alpha values.
			const int w = alphaFrame_->width;
			const int h = alphaFrame_->height;
			const int linesize = alphaFrame_->linesize[0];
			const auto decodedTimestamp =
			    resolveDecodedRtpTimestamp(alphaFrame_->pts, alphaFrame_->best_effort_timestamp);
			if (!decodedTimestamp) {
				logWarning("Dropping VP9 alpha frame without a decoder-preserved RTP timestamp");
				av_frame_unref(alphaFrame_);
				continue;
			}
			const uint32_t decodedRtpTimestamp = *decodedTimestamp;
			if (w > 0 && h > 0 && linesize > 0 && alphaFrame_->data[0]) {
				if (!loggedFirstDecodedAlphaFrame_.exchange(true, std::memory_order_relaxed)) {
					uint8_t minAlpha = 255;
					uint8_t maxAlpha = 0;
					for (int y = 0; y < h; ++y) {
						const uint8_t *row = alphaFrame_->data[0] + static_cast<ptrdiff_t>(y) * linesize;
						for (int x = 0; x < w; ++x) {
							const uint8_t alpha = row[x];
							minAlpha = std::min(minAlpha, alpha);
							maxAlpha = std::max(maxAlpha, alpha);
						}
					}
					logInfo(
					    "Native receiver decoded first alpha frame (%dx%d, format=%d, rtp ts=%u, alpha range=%u-%u)", w,
					    h, alphaFrame_->format, decodedRtpTimestamp, static_cast<unsigned>(minAlpha),
					    static_cast<unsigned>(maxAlpha));
				}
				loggedAlphaDecodeSubmitFailure_.store(false, std::memory_order_relaxed);
				loggedAlphaDecodeReceiveFailure_.store(false, std::memory_order_relaxed);
				PendingAlphaFrame pendingFrame;
				pendingFrame.width = w;
				pendingFrame.height = h;
				pendingFrame.yLinesize = linesize;
				pendingFrame.rtpTimestamp = decodedRtpTimestamp;
				pendingFrame.mediaEpoch = mediaEpoch;
				pendingFrame.yData.resize(static_cast<size_t>(linesize) * static_cast<size_t>(h));
				std::memcpy(pendingFrame.yData.data(), alphaFrame_->data[0],
				            static_cast<size_t>(linesize) * static_cast<size_t>(h));

				decodedFrames.push_back(std::move(pendingFrame));
			}
			av_frame_unref(alphaFrame_);
		}
	};
	int sendResult = sendAlphaPacket(alphaDecoder_, alphaPacket_);
	if (sendResult == AVERROR(EAGAIN)) {
		drainDecodedFrames();
		sendResult = sendAlphaPacket(alphaDecoder_, alphaPacket_);
	}
	if (sendResult < 0) {
		if (!loggedAlphaDecodeSubmitFailure_.exchange(true, std::memory_order_relaxed)) {
			logWarning("Failed to submit VP9 alpha packet: %s", ffmpegErrorString(sendResult).c_str());
		}
		return;
	}
	drainDecodedFrames();
	lock.unlock();

	for (auto &decodedFrame : decodedFrames) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		runNativeMediaTestStage(NativeMediaTestStage::PrePair, true, decodedFrame.rtpTimestamp, mediaEpoch);
#endif
		if (mediaEpochGate_.isCurrent(mediaEpoch)) {
			handleDecodedAlphaFrame(std::move(decodedFrame), mediaEpoch);
		}
	}
}

void VDONinjaSource::handleDecodedAlphaFrame(PendingAlphaFrame frame, uint64_t mediaEpoch)
{
	AlphaFrameSyncResult result;
	{
		std::lock_guard<std::mutex> pairingLock(alphaPairingMutex_);
		if (!mediaEpochGate_.isCurrent(mediaEpoch) || frame.mediaEpoch != mediaEpoch) {
			return;
		}
		result = alphaFrameSynchronizer_.pushAlpha(std::move(frame));
	}

	if ((result.rejectedIncomingFrame || result.droppedPrimaryFrames > 0 || result.droppedAlphaFrames > 0) &&
	    !loggedAlphaTimestampMiss_.exchange(true, std::memory_order_relaxed)) {
		logInfo("Dropped unmatched VP9 alpha pairing state (primary=%zu, alpha=%zu, rejected=%s)",
		        result.droppedPrimaryFrames, result.droppedAlphaFrames,
		        result.rejectedIncomingFrame ? "true" : "false");
	}
	if (result.pair) {
		outputPairedVideoFrame(std::move(*result.pair), true);
	}
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
	audioTimestampMapper_.reset();
	lastAudioTimestampNs_ = 0;
}

void VDONinjaSource::resetNativeState()
{
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::SuppressionResetRequest, false, 0, mediaEpochGate_.capture());
#endif
	std::lock_guard<std::mutex> applyLock(trackEventApplyMutex_);
	std::vector<std::shared_ptr<rtc::Track>> retiredTracks;
	{
		std::unique_lock<std::mutex> stateLock(nativeStateMutex_);
		std::unique_lock<std::mutex> videoAssemblyLock(videoAssemblyMutex_);
		std::unique_lock<std::mutex> videoDecodeLock(videoDecodeMutex_);
		std::unique_lock<std::mutex> alphaAssemblyLock(alphaAssemblyMutex_);
		std::unique_lock<std::mutex> alphaDecodeLock(alphaDecodeMutex_);
		std::unique_lock<std::mutex> outputLock(videoOutputMutex_);
		std::unique_lock<std::mutex> commitStateLock(videoCommitStateMutex_);
		std::unique_lock<std::mutex> pairingLock(alphaPairingMutex_);
		std::unique_lock<std::mutex> audioDecodeLock(audioDecodeMutex_);

		retiredTracks = {videoTrack_, alphaVideoTrack_, audioTrack_};
		for (const auto &[uuid, generations] : pendingPeerTrackBundles_) {
			for (const auto &[generation, bundle] : generations) {
				retiredTracks.push_back(bundle.video);
				retiredTracks.push_back(bundle.alpha);
				retiredTracks.push_back(bundle.audio);
			}
		}
		loggedFirstVideoRtpPacket_ = false;
		loggedFirstVideoPacket_ = false;
		loggedFirstDecodedVideoFrame_ = false;
		loggedFirstAlphaRtpPacket_ = false;
		loggedFirstAudioPacket_ = false;
		loggedFirstDecodedAudioFrame_ = false;
		loggedAudioDecodeSubmitFailure_ = false;
		remoteAudioMuted_.store(false, std::memory_order_relaxed);
		remoteVideoSuppressedState_ = false;
		remoteVideoMuted_.store(false, std::memory_order_release);
		remoteMediaVideoMuted_.store(false, std::memory_order_relaxed);
		remoteDirectorVideoMuted_.store(false, std::memory_order_relaxed);
		remoteVirtualHangup_.store(false, std::memory_order_relaxed);
		loggedFirstDecodedAlphaFrame_ = false;
		loggedAlphaDecodeSubmitFailure_ = false;
		loggedAlphaDecodeReceiveFailure_ = false;
		alphaTrackActive_.store(false, std::memory_order_release);
		preferSoftwareVp9DecodeForAlpha_.store(false, std::memory_order_release);
		loggedAlphaSoftwareDecodeMode_.store(false, std::memory_order_relaxed);
		loggedAlphaCompositionActive_.store(false, std::memory_order_relaxed);
		loggedAlphaTimestampSyncWait_.store(false, std::memory_order_relaxed);
		loggedAlphaTimestampMiss_.store(false, std::memory_order_relaxed);
		loggedAlphaPixelFormatMismatch_.store(false, std::memory_order_relaxed);
		loggedAlphaDimensionMismatch_.store(false, std::memory_order_relaxed);
		videoTrack_.reset();
		alphaVideoTrack_.reset();
		audioTrack_.reset();
		videoTrackPeerUuid_.clear();
		alphaVideoTrackPeerUuid_.clear();
		audioTrackPeerUuid_.clear();
		videoTrackPeerGeneration_ = 0;
		alphaVideoTrackPeerGeneration_ = 0;
		audioTrackPeerGeneration_ = 0;
		pendingPeerTrackBundles_.clear();
		nextPendingPeerTrackOrder_ = 1;
		peerTrackBundleAdoptionInProgress_.store(false, std::memory_order_release);
		peerEventStates_.clear();
		peerControlStates_.clear();
		videoTrackEventPositions_.clear();
		alphaTrackEventPositions_.clear();
		audioTrackEventPositions_.clear();
		videoRedPayloadTypes_.clear();
		videoHwDecodeDisabled_ = false;
		videoOutputActive_.store(false, std::memory_order_relaxed);
		loggedVideoStallClear_.store(false, std::memory_order_relaxed);
		resetMediaPipelineStateLocked();
		resetAudioDecoder();
	}
	for (const auto &track : retiredTracks) {
		clearTrackCallbacks(track);
	}
	if (source_) {
		setObsSourceAudioActive(false);
	}
	completeMediaPipelineTransition("reset-native-state", false);
}

void VDONinjaSource::handlePeerDisconnected(const PeerEventIdentity &identity)
{
	std::lock_guard<std::mutex> applyLock(trackEventApplyMutex_);
	if (!acceptPeerEventIdentityLocked(identity, true)) {
		return;
	}
	handlePeerDisconnectedAccepted(identity, true);
}

void VDONinjaSource::handlePeerDisconnectedAccepted(const PeerEventIdentity &identity, bool scheduleRetry)
{
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	nativeMediaTestPeerRetirements_.fetch_add(1, std::memory_order_acq_rel);
#endif
	const std::string &uuid = identity.uuid;
	bool mediaRemoved = false;
	bool audioRemoved = false;
	bool enableOutputAfterTransition = false;
	std::vector<std::shared_ptr<rtc::Track>> retiredTracks;

	{
		std::unique_lock<std::mutex> stateLock(nativeStateMutex_);
		std::unique_lock<std::mutex> videoAssemblyLock(videoAssemblyMutex_);
		std::unique_lock<std::mutex> videoDecodeLock(videoDecodeMutex_);
		std::unique_lock<std::mutex> alphaAssemblyLock(alphaAssemblyMutex_);
		std::unique_lock<std::mutex> alphaDecodeLock(alphaDecodeMutex_);
		std::unique_lock<std::mutex> outputLock(videoOutputMutex_);
		std::unique_lock<std::mutex> pairingLock(alphaPairingMutex_);
		std::unique_lock<std::mutex> audioDecodeLock(audioDecodeMutex_);

		const auto pendingUuid = pendingPeerTrackBundles_.find(uuid);
		if (pendingUuid != pendingPeerTrackBundles_.end()) {
			const auto pendingGeneration = pendingUuid->second.find(identity.generation);
			if (pendingGeneration != pendingUuid->second.end()) {
				retiredTracks.push_back(pendingGeneration->second.video);
				retiredTracks.push_back(pendingGeneration->second.alpha);
				retiredTracks.push_back(pendingGeneration->second.audio);
				pendingUuid->second.erase(pendingGeneration);
				if (pendingUuid->second.empty()) {
					pendingPeerTrackBundles_.erase(pendingUuid);
				}
			}
		}

		if (!uuid.empty() && uuid == videoTrackPeerUuid_ && identity.generation == videoTrackPeerGeneration_) {
			retiredTracks.push_back(videoTrack_);
			videoTrack_.reset();
			videoTrackPeerUuid_.clear();
			videoTrackPeerGeneration_ = 0;
			videoRedPayloadTypes_.clear();
			mediaRemoved = true;
		}

		if (!uuid.empty() && uuid == alphaVideoTrackPeerUuid_ &&
		    identity.generation == alphaVideoTrackPeerGeneration_) {
			retiredTracks.push_back(alphaVideoTrack_);
			alphaVideoTrack_.reset();
			alphaVideoTrackPeerUuid_.clear();
			alphaVideoTrackPeerGeneration_ = 0;
			mediaRemoved = true;
		}
		if (mediaRemoved) {
			resetMediaPipelineStateLocked();
			alphaTrackActive_.store(alphaVideoTrack_ != nullptr, std::memory_order_release);
			preferSoftwareVp9DecodeForAlpha_.store(alphaVideoTrack_ != nullptr, std::memory_order_release);
		}
		enableOutputAfterTransition = videoTrack_ != nullptr;

		if (!uuid.empty() && uuid == audioTrackPeerUuid_ && identity.generation == audioTrackPeerGeneration_) {
			retiredTracks.push_back(audioTrack_);
			audioTrack_.reset();
			audioTrackPeerUuid_.clear();
			audioTrackPeerGeneration_ = 0;
			resetAudioDecoder();
			loggedAudioDecodeSubmitFailure_ = false;
			audioRemoved = true;
		}

		connected_ = !videoTrackPeerUuid_.empty() || !alphaVideoTrackPeerUuid_.empty() || !audioTrackPeerUuid_.empty();
	}
	for (const auto &track : retiredTracks) {
		clearTrackCallbacks(track);
	}

	if (audioRemoved && source_) {
		setObsSourceAudioActive(false);
	}

	if (mediaRemoved) {
		completeMediaPipelineTransition("peer-disconnected", enableOutputAfterTransition);
	}
	const auto adoptedIdentity = adoptNextPendingPeerBundleIfOwnerless("peer-disconnected");
	{
		std::lock_guard<std::mutex> commitStateLock(videoCommitStateMutex_);
		const auto peerStates = peerControlStates_.find(identity.uuid);
		if (peerStates != peerControlStates_.end()) {
			peerStates->second.erase(identity.generation);
			if (peerStates->second.empty()) {
				peerControlStates_.erase(peerStates);
			}
		}
	}
	publishActivePeerControlStateLocked("peer-terminal-owner-transition");
	if (adoptedIdentity) {
		peerTrackBundleAdoptionInProgress_.store(false, std::memory_order_release);
		markNativePeerConnectedIfReadyAccepted(*adoptedIdentity, "deferred-peer-bundle-adopted");
	}

	if (scheduleRetry && !connected_.load() && settings_.autoReconnect) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		nativeMediaTestPeerRetrySchedules_.fetch_add(1, std::memory_order_acq_rel);
#endif
		int retryCount = 0;
		{
			std::lock_guard<std::mutex> lock(retryStateMutex_);
			retryCount = viewRetryCount_;
		}
		scheduleViewRetry("peer-disconnected", computeViewerPeerRecoveryDelayMs(retryCount), false);
	}
}

void VDONinjaSource::outputDecodedVideoFrame(const AVFrame *frame, uint32_t rtpTimestamp, uint64_t mediaEpoch)
{
	if (!frame || !hasNativeVideoOutputTarget() || !mediaEpochGate_.isCurrent(mediaEpoch) ||
	    outputMediaEpoch_.load(std::memory_order_acquire) != mediaEpoch) {
		return;
	}
	if (remoteVideoMuted_.load(std::memory_order_relaxed)) {
		return;
	}

	if (!alphaTrackActive_.load(std::memory_order_relaxed)) {
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		runNativeMediaTestStage(NativeMediaTestStage::PreOutput, false, rtpTimestamp, mediaEpoch);
#endif
		std::lock_guard<std::mutex> outputLock(videoOutputMutex_);
		if (!mediaEpochGate_.isCurrent(mediaEpoch) || outputMediaEpoch_.load(std::memory_order_acquire) != mediaEpoch ||
		    alphaTrackActive_.load(std::memory_order_acquire)) {
			return;
		}
		outputDecodedVideoFrameLocked(frame, rtpTimestamp, nullptr, mediaEpoch);
		return;
	}

	auto retainedFrame = retainVideoFrame(frame);
	if (!retainedFrame) {
		logWarning("Failed to retain primary video frame while waiting for exact VP9 alpha timestamp %u", rtpTimestamp);
		return;
	}
	PendingPrimaryFrame primaryFrame;
	primaryFrame.frame = std::move(retainedFrame);
	primaryFrame.width = frame->width;
	primaryFrame.height = frame->height;
	primaryFrame.rtpTimestamp = rtpTimestamp;
	primaryFrame.mediaEpoch = mediaEpoch;

	AlphaFrameSyncResult result;
	{
		std::lock_guard<std::mutex> pairingLock(alphaPairingMutex_);
		if (!mediaEpochGate_.isCurrent(mediaEpoch) || outputMediaEpoch_.load(std::memory_order_acquire) != mediaEpoch ||
		    !alphaTrackActive_.load(std::memory_order_acquire)) {
			return;
		}
		result = alphaFrameSynchronizer_.pushPrimary(std::move(primaryFrame));
	}
	if (result.queued && !loggedAlphaTimestampSyncWait_.exchange(true, std::memory_order_relaxed)) {
		logInfo("Buffering primary video until the exact VP9 alpha RTP timestamp arrives");
	}
	if ((result.rejectedIncomingFrame || result.droppedPrimaryFrames > 0 || result.droppedAlphaFrames > 0) &&
	    !loggedAlphaTimestampMiss_.exchange(true, std::memory_order_relaxed)) {
		logInfo("Dropped unmatched VP9 alpha pairing state (primary=%zu, alpha=%zu, rejected=%s)",
		        result.droppedPrimaryFrames, result.droppedAlphaFrames,
		        result.rejectedIncomingFrame ? "true" : "false");
	}
	if (result.pair) {
		outputPairedVideoFrame(std::move(*result.pair), false);
	}
}

void VDONinjaSource::outputPairedVideoFrame(AlphaFramePair pair, bool completedByAlpha)
{
	if (!pair.primary.frame || !hasNativeVideoOutputTarget()) {
		return;
	}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::PreOutput, completedByAlpha, pair.primary.rtpTimestamp,
	                        pair.mediaEpoch);
#else
	UNUSED_PARAMETER(completedByAlpha);
#endif

	std::lock_guard<std::mutex> outputLock(videoOutputMutex_);
	{
		std::lock_guard<std::mutex> pairingLock(alphaPairingMutex_);
		if (!alphaFrameSynchronizer_.isCurrentGeneration(pair.generation) ||
		    !mediaEpochGate_.isCurrent(pair.mediaEpoch)) {
			return;
		}
	}
	if (!nativeRunning_.load(std::memory_order_relaxed) || !alphaTrackActive_.load(std::memory_order_relaxed) ||
	    remoteVideoMuted_.load(std::memory_order_relaxed) || !mediaEpochGate_.isCurrent(pair.mediaEpoch) ||
	    outputMediaEpoch_.load(std::memory_order_acquire) != pair.mediaEpoch) {
		return;
	}

	outputDecodedVideoFrameLocked(pair.primary.frame.get(), pair.primary.rtpTimestamp, &pair.alpha, pair.mediaEpoch);
}

void VDONinjaSource::outputDecodedVideoFrameLocked(const AVFrame *frame, uint32_t rtpTimestamp,
                                                   const PendingAlphaFrame *alphaFrame, uint64_t mediaEpoch)
{
	if (!frame || !hasNativeVideoOutputTarget() || remoteVideoMuted_.load(std::memory_order_relaxed) ||
	    !mediaEpochGate_.isCurrent(mediaEpoch) || outputMediaEpoch_.load(std::memory_order_acquire) != mediaEpoch) {
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

	std::vector<uint8_t> alphaYCopy;
	std::vector<uint8_t> scaledAlphaY;
	int alphaYLinesize = 0;
	bool hasAlpha = false;
	int alphaWidth = 0;
	int alphaHeight = 0;
	if (alphaFrame) {
		if (alphaFrame->width == frame->width && alphaFrame->height == frame->height &&
		    alphaFrame->yLinesize >= alphaFrame->width &&
		    alphaFrame->yData.size() >=
		        static_cast<size_t>(alphaFrame->yLinesize) * static_cast<size_t>(alphaFrame->height)) {
			alphaYCopy = alphaFrame->yData;
			alphaYLinesize = alphaFrame->yLinesize;
			hasAlpha = true;
		} else if (scaleAlphaPlaneNearest(alphaFrame->yData, alphaFrame->width, alphaFrame->height,
		                                  alphaFrame->yLinesize, frame->width, frame->height, scaledAlphaY)) {
			alphaYCopy = std::move(scaledAlphaY);
			alphaYLinesize = frame->width;
			hasAlpha = true;
			alphaWidth = alphaFrame->width;
			alphaHeight = alphaFrame->height;
		}
		if (!hasAlpha) {
			if (!loggedAlphaPixelFormatMismatch_.exchange(true, std::memory_order_relaxed)) {
				logWarning("Dropping exactly paired primary/alpha frame with an invalid alpha plane (rtp ts=%u)",
				           rtpTimestamp);
			}
			return;
		}
	}

	if (hasAlpha && alphaWidth > 0 && alphaHeight > 0 &&
	    !loggedAlphaDimensionMismatch_.exchange(true, std::memory_order_relaxed)) {
		logInfo("Scaled VP9 alpha frame for RTP timestamp %u from %dx%d to primary video %dx%d", rtpTimestamp,
		        alphaWidth, alphaHeight, frame->width, frame->height);
	}
	const AVFrame *frameToScale = frame;
	if (hasAlpha) {
		if (!loggedAlphaCompositionActive_.exchange(true, std::memory_order_relaxed)) {
			logInfo("Native receiver alpha composition active");
		}
		loggedAlphaPixelFormatMismatch_.store(false, std::memory_order_relaxed);
	}

	const auto dimensions = outputDimensions();
	const AVPixelFormat inputFormat = static_cast<AVPixelFormat>(frameToScale->format);
	const AspectFitLayout layout =
	    computeAspectFitLayout(static_cast<uint32_t>(frameToScale->width), static_cast<uint32_t>(frameToScale->height),
	                           dimensions.width, dimensions.height);
	videoScaleContext_ =
	    sws_getCachedContext(videoScaleContext_, frameToScale->width, frameToScale->height, inputFormat,
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

	const int scaledHeight = sws_scale(videoScaleContext_, frameToScale->data, frameToScale->linesize, 0,
	                                   frameToScale->height, dstData, dstLinesize);
	if (scaledHeight <= 0 || static_cast<uint32_t>(scaledHeight) != layout.contentHeight) {
		logWarning("Failed to convert decoded video frame");
		return;
	}
	if (hasAlpha && alphaYLinesize > 0 &&
	    alphaYCopy.size() >= static_cast<size_t>(alphaYLinesize) * static_cast<size_t>(frame->height)) {
		uint8_t minAppliedAlpha = 255;
		uint8_t maxAppliedAlpha = 0;
		uint64_t nonZeroAppliedAlpha = 0;
		uint64_t appliedAlphaPixels = 0;
		for (uint32_t y = 0; y < layout.contentHeight; ++y) {
			const int srcY = std::min(
			    frame->height - 1, static_cast<int>((static_cast<uint64_t>(y) * static_cast<uint64_t>(frame->height)) /
			                                        std::max<uint32_t>(1, layout.contentHeight)));
			const uint8_t *alphaRow =
			    alphaYCopy.data() + static_cast<size_t>(srcY) * static_cast<size_t>(alphaYLinesize);
			uint8_t *dstRow = output.data() +
			                  (static_cast<size_t>(layout.offsetY + y) * static_cast<size_t>(outputStride)) +
			                  (static_cast<size_t>(layout.offsetX) * 4);
			for (uint32_t x = 0; x < layout.contentWidth; ++x) {
				const int srcX =
				    std::min(frame->width - 1,
				             static_cast<int>((static_cast<uint64_t>(x) * static_cast<uint64_t>(frame->width)) /
				                              std::max<uint32_t>(1, layout.contentWidth)));
				const uint8_t alpha = alphaRow[srcX];
				dstRow[static_cast<size_t>(x) * 4 + 3] = alpha;
				minAppliedAlpha = std::min(minAppliedAlpha, alpha);
				maxAppliedAlpha = std::max(maxAppliedAlpha, alpha);
				if (alpha > 0) {
					nonZeroAppliedAlpha++;
				}
				appliedAlphaPixels++;
			}
		}
		static std::atomic<bool> loggedAppliedAlphaOutputRange{false};
		if (!loggedAppliedAlphaOutputRange.exchange(true, std::memory_order_relaxed)) {
			logInfo("Applied VP9 alpha plane to BGRA output (rtp ts=%u, range=%u-%u, nonzero=%llu/%llu)", rtpTimestamp,
			        static_cast<unsigned>(minAppliedAlpha), static_cast<unsigned>(maxAppliedAlpha),
			        static_cast<unsigned long long>(nonZeroAppliedAlpha),
			        static_cast<unsigned long long>(appliedAlphaPixels));
		}
	} else if (!hasAlpha) {
		for (uint32_t y = 0; y < layout.outputHeight; ++y) {
			uint8_t *row = output.data() + static_cast<size_t>(y) * static_cast<size_t>(outputStride);
			for (uint32_t x = 0; x < layout.outputWidth; ++x) {
				row[static_cast<size_t>(x) * 4 + 3] = 255;
			}
		}
	}

	obs_source_frame obsFrame = {};
	obsFrame.width = layout.outputWidth;
	obsFrame.height = layout.outputHeight;
	obsFrame.format = VIDEO_FORMAT_BGRA;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::PreCommit, false, rtpTimestamp, mediaEpoch);
#endif
	std::unique_lock<std::mutex> commitStateLock(videoCommitStateMutex_);
	if (remoteVideoSuppressedState_ || !mediaEpochGate_.isCurrent(mediaEpoch) ||
	    outputMediaEpoch_.load(std::memory_order_acquire) != mediaEpoch) {
		return;
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	runNativeMediaTestStage(NativeMediaTestStage::CommitAuthorized, false, rtpTimestamp, mediaEpoch);
#endif
	const auto outputTimestamp = videoTimestampMapper_.map(rtpTimestamp, os_gettime_ns());
	if (!outputTimestamp) {
		return;
	}
	obsFrame.timestamp = *outputTimestamp;
	obsFrame.full_range = true;
	obsFrame.data[0] = output.data();
	obsFrame.linesize[0] = static_cast<uint32_t>(outputStride);
	videoOutputActive_.store(true, std::memory_order_relaxed);
	lastVideoTime_.store(currentTimeMs(), std::memory_order_relaxed);
	loggedVideoStallClear_.store(false, std::memory_order_relaxed);
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (nativeMediaTestOutputHook_) {
		NativeMediaTestOutput testOutput;
		testOutput.bgra = output;
		testOutput.width = layout.outputWidth;
		testOutput.height = layout.outputHeight;
		testOutput.rtpTimestamp = rtpTimestamp;
		testOutput.outputTimestampNs = *outputTimestamp;
		testOutput.hasAlpha = hasAlpha;
		nativeMediaTestOutputHook_(std::move(testOutput));
		return;
	}
#endif
	obs_source_output_video(source_, &obsFrame);
}

void VDONinjaSource::outputDecodedAudioFrame(const AVFrame *frame, uint64_t timestampNs)
{
	if (!frame) {
		return;
	}
	if (remoteAudioMuted_.load(std::memory_order_relaxed)) {
		return;
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (nativeMediaTestAudioOutputHook_) {
		nativeMediaTestAudioOutputHook_(timestampNs);
		return;
	}
#endif
	if (!source_) {
		return;
	}

	if (!loggedFirstDecodedAudioFrame_.exchange(true)) {
		logInfo("Native receiver decoded first audio frame (%d samples, format=%d, rate=%d)", frame->nb_samples,
		        frame->format, frame->sample_rate);
	}

	const int inputChannels =
	    frame->ch_layout.nb_channels > 0 ? static_cast<int>(frame->ch_layout.nb_channels) : audioChannels_;
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

	const int convertedSamples = swr_convert(audioResampleContext_, dstData, outputSamples,
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

	setObsSourceAudioActive(true);
	obs_source_output_audio(source_, &audio);

	av_freep(&dstData[0]);
	av_channel_layout_uninit(&outputLayout);
}

uint64_t VDONinjaSource::mapAudioTimestamp(uint32_t rtpTimestamp)
{
	const uint64_t now = os_gettime_ns();
	const uint32_t clockRate = audioSampleRate_ > 0 ? static_cast<uint32_t>(audioSampleRate_) : 48000U;
	// Extend successive RTP deltas so audio keeps advancing beyond a full
	// 32-bit clock cycle. Late/duplicate packets retain the existing monotonic clamp.
	uint64_t mapped = audioTimestampMapper_.map(rtpTimestamp, now, clockRate).value_or(lastAudioTimestampNs_ + 1);
	if (mapped <= lastAudioTimestampNs_) {
		mapped = lastAudioTimestampNs_ + 1;
	}
	lastAudioTimestampNs_ = mapped;
	return mapped;
}

void VDONinjaSource::ensureNativeReceiverSource()
{
	if (isInternalNativeSource() || !usingNativeReceiver() || settings_.streamId.empty()) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (nativeReceiverSource_) {
			return;
		}
	}

	const auto dimensions = outputDimensions();
	obs_data_t *nativeSettings = createNativeReceiverSourceSettings(settings_, dimensions.width, dimensions.height);
	obs_source_t *created =
	    obs_source_create_private(kInternalNativeSourceId, nativeReceiverSourceName_.c_str(), nativeSettings);
	obs_data_release(nativeSettings);

	if (!created) {
		logError("Failed to create internal native receiver source for VDO.Ninja Source");
		return;
	}

	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (nativeReceiverSource_) {
			obs_source_release(created);
			return;
		}
		nativeReceiverSource_ = created;
	}

	obs_source_add_audio_capture_callback(created, vdoninja_source_child_audio_capture, callbackState_.get());
	signal_handler_t *sh = obs_source_get_signal_handler(created);
	signal_handler_connect(sh, "audio_activate", vdoninja_source_child_audio_activate, callbackState_.get());
	signal_handler_connect(sh, "audio_deactivate", vdoninja_source_child_audio_deactivate, callbackState_.get());
	setObsSourceAudioActive(obs_source_audio_active(created));
	syncChildLifecycleState(created);
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		nativeReceiverSettings_ = settings_;
		nativeReceiverWidth_ = dimensions.width;
		nativeReceiverHeight_ = dimensions.height;
		nativeReceiverConfigApplied_ = true;
	}

	logInfo("Created internal native receiver source for VDO.Ninja Source");
}

void VDONinjaSource::releaseNativeReceiverSource()
{
	obs_source_t *child = nullptr;
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		child = nativeReceiverSource_;
		if (!child) {
			return;
		}
		nativeReceiverSource_ = nullptr;
		nativeReceiverConfigApplied_ = false;
		nativeReceiverWidth_ = 0;
		nativeReceiverHeight_ = 0;
		nativeReceiverSettings_ = SourceSettings{};
	}

	if (!child) {
		return;
	}

	signal_handler_t *sh = obs_source_get_signal_handler(child);
	signal_handler_disconnect(sh, "audio_activate", vdoninja_source_child_audio_activate, callbackState_.get());
	signal_handler_disconnect(sh, "audio_deactivate", vdoninja_source_child_audio_deactivate, callbackState_.get());
	obs_source_remove_audio_capture_callback(child, vdoninja_source_child_audio_capture, callbackState_.get());
	detachChildLifecycleState(child);

	setObsSourceAudioActive(false);
	obs_source_release(child);
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
	obs_source_t *child = nullptr;
	bool configApplied = false;
	uint32_t configuredWidth = 0;
	uint32_t configuredHeight = 0;
	SourceSettings configuredSettings;
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		child = nativeReceiverSource_;
		if (child) {
			child = obs_source_get_ref(child);
		}
		configApplied = nativeReceiverConfigApplied_;
		configuredWidth = nativeReceiverWidth_;
		configuredHeight = nativeReceiverHeight_;
		configuredSettings = nativeReceiverSettings_;
	}

	if (!child) {
		return;
	}

	const auto dimensions = outputDimensions();
	if (configApplied && configuredWidth == dimensions.width && configuredHeight == dimensions.height &&
	    sourceSettingsEqualForChild(configuredSettings, settings_)) {
		syncChildLifecycleState(child);
		obs_source_release(child);
		return;
	}

	obs_data_t *nativeSettings = createNativeReceiverSourceSettings(settings_, dimensions.width, dimensions.height);
	obs_source_update(child, nativeSettings);
	obs_data_release(nativeSettings);
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (nativeReceiverSource_ == child) {
			nativeReceiverSettings_ = settings_;
			nativeReceiverWidth_ = dimensions.width;
			nativeReceiverHeight_ = dimensions.height;
			nativeReceiverConfigApplied_ = true;
		}
	}
	syncChildLifecycleState(child);
	obs_source_release(child);
}

void VDONinjaSource::ensureBrowserSource()
{
	if (isInternalNativeSource() || usingNativeReceiver() || settings_.streamId.empty()) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (browserSource_) {
			return;
		}
	}

	const std::string url = buildViewerUrl();
	if (url.empty()) {
		return;
	}

	const auto dimensions = outputDimensions();
	obs_data_t *browserSettings = createBrowserSourceSettings(url, dimensions.width, dimensions.height);
	obs_source_t *created = obs_source_create_private("browser_source", browserSourceName_.c_str(), browserSettings);
	obs_data_release(browserSettings);

	if (!created) {
		logError("Failed to create internal browser source for VDO.Ninja Source");
		return;
	}

	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (browserSource_) {
			obs_source_release(created);
			return;
		}
		browserSource_ = created;
	}

	obs_source_add_audio_capture_callback(created, vdoninja_source_child_audio_capture, callbackState_.get());
	signal_handler_t *sh = obs_source_get_signal_handler(created);
	signal_handler_connect(sh, "audio_activate", vdoninja_source_child_audio_activate, callbackState_.get());
	signal_handler_connect(sh, "audio_deactivate", vdoninja_source_child_audio_deactivate, callbackState_.get());
	setObsSourceAudioActive(obs_source_audio_active(created));
	syncChildLifecycleState(created);
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		browserSourceUrl_ = url;
		browserSourceWidth_ = dimensions.width;
		browserSourceHeight_ = dimensions.height;
		browserSourceConfigApplied_ = true;
	}

	logInfo("Created internal Browser Source for VDO.Ninja Source");
}

void VDONinjaSource::releaseBrowserSource()
{
	obs_source_t *child = nullptr;
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		child = browserSource_;
		if (!child) {
			return;
		}
		browserSource_ = nullptr;
		browserSourceConfigApplied_ = false;
		browserSourceWidth_ = 0;
		browserSourceHeight_ = 0;
		browserSourceUrl_.clear();
	}

	if (!child) {
		return;
	}

	signal_handler_t *sh = obs_source_get_signal_handler(child);
	signal_handler_disconnect(sh, "audio_activate", vdoninja_source_child_audio_activate, callbackState_.get());
	signal_handler_disconnect(sh, "audio_deactivate", vdoninja_source_child_audio_deactivate, callbackState_.get());
	obs_source_remove_audio_capture_callback(child, vdoninja_source_child_audio_capture, callbackState_.get());
	detachChildLifecycleState(child);

	setObsSourceAudioActive(false);
	obs_source_release(child);
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
	obs_source_t *child = nullptr;
	bool configApplied = false;
	uint32_t configuredWidth = 0;
	uint32_t configuredHeight = 0;
	std::string configuredUrl;
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		child = browserSource_;
		if (child) {
			child = obs_source_get_ref(child);
		}
		configApplied = browserSourceConfigApplied_;
		configuredWidth = browserSourceWidth_;
		configuredHeight = browserSourceHeight_;
		configuredUrl = browserSourceUrl_;
	}

	if (!child) {
		return;
	}

	const auto dimensions = outputDimensions();
	if (configApplied && configuredWidth == dimensions.width && configuredHeight == dimensions.height &&
	    configuredUrl == url) {
		syncChildLifecycleState(child);
		obs_source_release(child);
		return;
	}

	obs_data_t *browserSettings = createBrowserSourceSettings(url, dimensions.width, dimensions.height);
	obs_source_update(child, browserSettings);
	obs_data_release(browserSettings);
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (browserSource_ == child) {
			browserSourceUrl_ = url;
			browserSourceWidth_ = dimensions.width;
			browserSourceHeight_ = dimensions.height;
			browserSourceConfigApplied_ = true;
		}
	}
	syncChildLifecycleState(child);
	obs_source_release(child);
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
	const bool shouldBeActive = active_.load() || (source_ && obs_source_active(source_));
	bool incShowing = false;
	bool decShowing = false;
	bool incActive = false;
	bool decActive = false;
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (shouldShow && !childShowing_) {
			childShowing_ = true;
			incShowing = true;
		} else if (!shouldShow && childShowing_) {
			childShowing_ = false;
			decShowing = true;
		}

		if (shouldBeActive && !childActive_) {
			childActive_ = true;
			incActive = true;
		} else if (!shouldBeActive && childActive_) {
			childActive_ = false;
			decActive = true;
		}
	}

	if (incShowing) {
		obs_source_inc_showing(child);
	} else if (decShowing) {
		obs_source_dec_showing(child);
	}

	if (incActive) {
		obs_source_inc_active(child);
	} else if (decActive) {
		obs_source_dec_active(child);
	}
}

void VDONinjaSource::detachChildLifecycleState(obs_source_t *child)
{
	bool decShowing = false;
	bool decActive = false;

	if (!child) {
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		childShowing_ = false;
		childActive_ = false;
		return;
	}

	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (childShowing_) {
			childShowing_ = false;
			decShowing = true;
		}
		if (childActive_) {
			childActive_ = false;
			decActive = true;
		}
	}

	if (decShowing) {
		obs_source_dec_showing(child);
	}
	if (decActive) {
		obs_source_dec_active(child);
	}
}

obs_source_t *VDONinjaSource::acquireActiveChildSource() const
{
	if (isInternalNativeSource()) {
		return nullptr;
	}

	std::lock_guard<std::mutex> lock(childSourceMutex_);
	obs_source_t *child = usingNativeReceiver() ? nativeReceiverSource_ : browserSource_;
	if (child) {
		child = obs_source_get_ref(child);
	}
	return child;
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
	setObsSourceAudioActive(true);
}

void VDONinjaSource::onChildAudioDeactivated()
{
	setObsSourceAudioActive(false);
}

void VDONinjaSource::setObsSourceAudioActive(bool active)
{
	if (sourceAudioActive_.exchange(active, std::memory_order_relaxed) == active) {
		return;
	}

	setObsWeakSourceAudioActiveSafe(sourceWeak_, active);
}

void VDONinjaSource::drainAsyncCallbacks()
{
	if (!callbackState_) {
		return;
	}

	AsyncCallbackGuard<VDONinjaSource>::detach(callbackState_.get());
	if (!AsyncCallbackGuard<VDONinjaSource>::waitForIdle(callbackState_.get(), 10000)) {
		logWarning("Timed out waiting for VDO.Ninja source callbacks to drain during teardown");
	}
	callbackState_.reset();
}

} // namespace vdoninja
