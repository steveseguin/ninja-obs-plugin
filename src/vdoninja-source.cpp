/*
 * OBS VDO.Ninja Plugin
 * Source module implementation
 */

#include "vdoninja-source.h"

#include <rtc/rtcpreceivingsession.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
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

#include "vdoninja-rtp-utils.h"
#include "vdoninja-utils.h"
#include "plugin-main.h"

namespace vdoninja
{

namespace
{

constexpr const char *kInternalNativeSourceId = "vdoninja_native_source_internal";
constexpr const char *kInternalNativeSourceSetting = "internal_native_receiver_source";
constexpr int kViewRequestTimeoutMs = 15000;
constexpr int kMinViewRequestGapMs = 1500;
constexpr int64_t kNativeVideoStallBlankMs = 4000;

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

void runUiTaskThunk(void *param)
{
	auto *fn = static_cast<std::function<void()> *>(param);
	(*fn)();
	delete fn;
}

void setObsSourceAudioActiveSafe(obs_source_t *source, bool active)
{
	if (!source) {
		return;
	}
	if (obs_source_audio_active(source) == active) {
		return;
	}

	if (obs_in_task_thread(OBS_TASK_UI)) {
		obs_source_set_audio_active(source, active);
		return;
	}

	obs_source_t *sourceRef = obs_source_get_ref(source);
	if (!sourceRef) {
		return;
	}
	auto *task = new std::function<void()>([sourceRef, active]() {
		obs_source_set_audio_active(sourceRef, active);
		obs_source_release(sourceRef);
	});
	obs_queue_task(OBS_TASK_UI, runUiTaskThunk, task, false);
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

		// VP9 with D3D11VA/DXVA2 allocates too few surfaces by default and hits
		// "Static surface pool size exceeded" (FFmpeg trac #10608). Extra frames fix this.
		if (isVP9) {
			decoderContext->extra_hw_frames = 6;
		}

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

	// With RTP/RTCP mux enabled, track callbacks can surface RTCP control packets
	// alongside media packets. RTCP packet types occupy the raw second-octet range
	// 192-223, while our RTP payload types are dynamic values outside that band.
	const uint8_t rawPacketType = packetData[1];
	if (rawPacketType >= 192 && rawPacketType <= 223) {
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
		const size_t extensionWords = static_cast<size_t>((static_cast<uint16_t>(packetData[headerSize + 2]) << 8) |
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
	if (!track) {
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
	if (!track || bitrateBps == 0) {
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
		track->onMessage(std::function<void(rtc::message_variant)>{});
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
	       "VP9/H.264/Opus WebRTC receive path. Compatible dual-track VP9 senders can preserve transparency."),
	    OBS_TEXT_INFO);
	obs_property_text_set_info_type(note, OBS_TEXT_INFO_NORMAL);
	obs_property_text_set_info_word_wrap(note, true);

	obs_property_t *useNative = obs_properties_add_bool(
	    props, "use_native_receiver", tr("VDONinjaSource.UseNativeReceiver", "Use Native Receiver (Experimental)"));
	obs_property_set_long_description(
	    useNative,
	    tr("VDONinjaSource.UseNativeReceiver.Description",
	       "Unchecked uses the simple browser-backed viewer path. Checked enables the experimental native "
	       "VP9/H.264/Opus receiver path with slower retry/backoff after failures. Dual-track VP9 alpha "
	       "transparency requires this mode."));
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
	    "VP9/H.264/Opus WebRTC receive path. Compatible dual-track VP9 senders can preserve transparency.");
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
		logWarning("VDO.Ninja Source native receiver mode is experimental (VP9/H.264 video + Opus audio)");
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
	setObsSourceAudioActiveSafe(source_, false);
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
		signaling_->setOnStreamRemoved(nullptr);
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

		peerManager_->setOnTrack(
		    [callbackState](const std::string &uuid, TrackType type, std::shared_ptr<rtc::Track> track) {
			    AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			    if (!guard) {
				    return;
			    }

			    if (type == TrackType::Video) {
				    guard.owner()->onVideoTrack(uuid, track);
			    } else if (type == TrackType::AlphaVideo) {
				    guard.owner()->onAlphaVideoTrack(uuid, track);
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
			std::shared_ptr<rtc::Track> videoTrack;
			{
				std::lock_guard<std::mutex> stateLock(self->nativeStateMutex_);
				videoTrack = self->videoTrack_;
			}
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
			if (safeRequestKeyframe(videoTrack, "peer-connected")) {
				self->lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
				logInfo("Requested initial video keyframe for native receiver");
			}
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
			    (message.find("\"description\"") != std::string::npos ||
			     message.find("\"candidate\"") != std::string::npos ||
			     message.find("\"candidates\"") != std::string::npos ||
			     message.find("\"request\"") != std::string::npos || message.find("\"bye\"") != std::string::npos)) {
				self->signaling_->processIncomingMessage(message);
				if (targetUuid.empty() && message.find("\"bye\"") != std::string::npos) {
					self->handlePeerCleanupSignal(uuid);
				}
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
			if (self->matchesTargetStreamId(streamId)) {
				logInfo("Target stream appeared in room, connecting...");
				self->requestViewStream("stream-added", false);
			}
		});
		signaling_->setOnStreamRemoved([callbackState](const std::string &streamId, const std::string &uuid) {
			AsyncCallbackGuard<VDONinjaSource> guard(callbackState.get());
			if (!guard) {
				return;
			}
			guard.owner()->handleStreamRemovedSignal(streamId, uuid);
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

	const std::string preferences =
	    buildViewerRequestMessage(width_, height_, !settings_.roomId.empty(), buildNativeViewerInfoJson(source_));
	peerManager_->sendDataToPeer(uuid, preferences);
	logInfo("Sent viewer preferences to %s (%s): %s", uuid.c_str(), reason ? reason : "unknown", preferences.c_str());
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
		logInfo("Requested native video REMB target of %u bps (%s)", targetBitrateBps, reason ? reason : "unknown");
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
	if (!source_) {
		return;
	}

	const bool hadVideo = videoOutputActive_.exchange(false, std::memory_order_relaxed);
	if (hadVideo && reason && *reason) {
		logInfo("Clearing native video output (%s)", reason);
	}
	obs_source_output_video(source_, nullptr);
}

void VDONinjaSource::handleStreamRemovedSignal(const std::string &streamId, const std::string &uuid)
{
	bool activePeerMatches = false;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		activePeerMatches =
		    (!uuid.empty() && (uuid == videoTrackPeerUuid_ || uuid == alphaVideoTrackPeerUuid_ ||
		                       uuid == audioTrackPeerUuid_));
	}

	if (!matchesTargetStreamId(streamId) && !activePeerMatches) {
		return;
	}

	logInfo(
	    "Native receiver got stream-removed for target stream (%s) from %s; clearing active receiver state",
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

void VDONinjaSource::onVideoTrack(const std::string &uuid, std::shared_ptr<rtc::Track> track)
{
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
		nativeVideoCodec_ = negotiatedCodec;
		videoAssemblyBuffer_.clear();
		videoAssemblyTimestamp_ = 0;
		videoAssemblyActive_ = false;
		if (replacedExistingTrack) {
			logInfo("Replacing native video track for peer %s; resetting decoder state", uuid.c_str());
			resetVideoDecoder();
			loggedFirstVideoRtpPacket_ = false;
			loggedFirstVideoPacket_ = false;
			loggedFirstDecodedVideoFrame_ = false;
			lastVideoTime_.store(0, std::memory_order_relaxed);
			lastKeyframeRequestTime_.store(0, std::memory_order_relaxed);
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
		            static_cast<unsigned>(rtpHeader->payloadType()), payloadSize,
		            static_cast<unsigned>(rtpHeader->marker()), rtpHeader->timestamp());
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
		lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
		logInfo("Requested video keyframe after replacing native video track");
	}
	requestNativeTargetBitrate("video-track-attached");
}

void VDONinjaSource::onAlphaVideoTrack(const std::string &uuid, std::shared_ptr<rtc::Track> track)
{
	logInfo("Received VP9 alpha video track from %s", uuid.c_str());

	if (!track) {
		return;
	}

	bool resetPrimaryVp9Decoder = false;
	{
		std::scoped_lock stateLock(nativeStateMutex_, videoDecodeMutex_, alphaAssemblyMutex_, alphaDecodeMutex_);
		if (alphaVideoTrack_ == track) {
			return;
		}
		if (!alphaVideoTrackPeerUuid_.empty() && alphaVideoTrackPeerUuid_ != uuid) {
			logWarning("Ignoring alpha video track from %s while peer %s remains active", uuid.c_str(),
			           alphaVideoTrackPeerUuid_.c_str());
			return;
		}
		clearTrackCallbacks(alphaVideoTrack_);
		alphaVideoTrack_ = track;
		alphaVideoTrackPeerUuid_ = uuid;
		alphaAssemblyBuffer_.clear();
		alphaAssemblyTimestamp_ = 0;
		alphaAssemblyActive_ = false;
		alphaTrackActive_.store(true, std::memory_order_relaxed);
		preferSoftwareVp9DecodeForAlpha_.store(true, std::memory_order_relaxed);
		loggedAlphaSoftwareDecodeMode_.store(false, std::memory_order_relaxed);
		loggedAlphaCompositionActive_.store(false, std::memory_order_relaxed);
		loggedAlphaTimestampSyncWait_.store(false, std::memory_order_relaxed);
		loggedAlphaTimestampMiss_.store(false, std::memory_order_relaxed);
		loggedAlphaPixelFormatMismatch_.store(false, std::memory_order_relaxed);
		loggedAlphaDimensionMismatch_.store(false, std::memory_order_relaxed);
		alphaTimestampPendingStreak_ = 0;
		alphaTimestampMissStreak_ = 0;
		resetAlphaDecoder();
		loggedFirstAlphaRtpPacket_ = false;
		if (nativeVideoCodec_ == NativeVideoCodec::VP9 && videoDecoder_) {
			resetVideoDecoder();
			resetPrimaryVp9Decoder = true;
		}
	}

	if (resetPrimaryVp9Decoder) {
		logInfo("Reset primary VP9 decoder so alpha composition uses software frames");
	}

	logInfo("Attaching native VP9 alpha video receive callbacks (mid=%s)", track->mid().c_str());

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
			if (self->alphaVideoTrack_ != strongTrack) {
				return;
			}
		}
		if (!std::holds_alternative<rtc::binary>(message)) {
			return;
		}
		const auto &packet = std::get<rtc::binary>(message);
		self->processAlphaRtpPacket(reinterpret_cast<const uint8_t *>(packet.data()), packet.size());
	});
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
			lastAudioTime_.store(0, std::memory_order_relaxed);
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
	NativeVideoCodec codec = NativeVideoCodec::H264;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
		currentVideoTrack = videoTrack_;
		codec = nativeVideoCodec_;
	}
	const char *codecName = codec == NativeVideoCodec::VP9 ? "VP9" : "H.264";

	std::lock_guard<std::mutex> lock(videoDecodeMutex_);
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
	const int sendResult = avcodec_send_packet(videoDecoder_, videoPacket_);
	if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
		logWarning("Failed to submit %s packet: %s", codecName, ffmpegErrorString(sendResult).c_str());
		if (safeRequestKeyframe(currentVideoTrack, "send-packet-failure")) {
			lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
		}
		return;
	}
	if (sendResult >= 0) {
		pendingVideoDecodeTimestamps_.push_back(rtpTimestamp);
	}

	while (true) {
		const int receiveResult = avcodec_receive_frame(videoDecoder_, videoFrame_);
		if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
			break;
		}
		if (receiveResult < 0) {
			logWarning("Failed to decode %s frame: %s", codecName, ffmpegErrorString(receiveResult).c_str());
			if (safeRequestKeyframe(currentVideoTrack, "decode-failure")) {
				lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
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
		if (videoHwDecodeConfigured_ && videoHwPixelFormat_ != AV_PIX_FMT_NONE &&
		    videoFrame_->format == videoHwPixelFormat_) {
			av_frame_unref(videoTransferFrame_);
			const int transferResult = av_hwframe_transfer_data(videoTransferFrame_, videoFrame_, 0);
			if (transferResult < 0) {
				logWarning("Failed to transfer hardware-decoded %s frame from %s: %s; disabling hardware decode for "
				           "this session",
				           codecName, videoHwDeviceName_.c_str(), ffmpegErrorString(transferResult).c_str());
				videoHwDecodeDisabled_ = true;
				resetVideoDecoder();
				if (safeRequestKeyframe(currentVideoTrack, "hw-transfer-failure")) {
					lastKeyframeRequestTime_.store(currentTimeMs(), std::memory_order_relaxed);
				}
				return;
			}
			av_frame_copy_props(videoTransferFrame_, videoFrame_);
			frameToOutput = videoTransferFrame_;
		}

		uint32_t decodedRtpTimestamp = rtpTimestamp;
		if (!pendingVideoDecodeTimestamps_.empty()) {
			decodedRtpTimestamp = pendingVideoDecodeTimestamps_.front();
			pendingVideoDecodeTimestamps_.pop_front();
		}
		outputDecodedVideoFrame(frameToOutput, mapVideoTimestamp(decodedRtpTimestamp), decodedRtpTimestamp);
		av_frame_unref(videoFrame_);
		if (frameToOutput == videoTransferFrame_) {
			av_frame_unref(videoTransferFrame_);
		}
	}

	lastVideoTime_.store(currentTimeMs(), std::memory_order_relaxed);
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
	NativeVideoCodec codec;
	{
		std::lock_guard<std::mutex> stateLock(nativeStateMutex_);
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
		processVP9RtpPacket(payload, payloadSize, rtpHeader->timestamp());
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

void VDONinjaSource::processVP9RtpPacket(const uint8_t *payload, size_t payloadSize, uint32_t rtpTimestamp)
{
	if (payloadSize == 0) {
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

	{
		std::lock_guard<std::mutex> lock(videoAssemblyMutex_);

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
			processVideoData(frame.first.data(), frame.first.size(), frame.second);
		}
	}
}

void VDONinjaSource::processAlphaRtpPacket(const uint8_t *packetData, size_t packetSize)
{
	if (!nativeRunning_.load() || !packetData || packetSize < sizeof(rtc::RtpHeader)) {
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

	processAlphaVP9RtpPacket(packetData + payloadView->offset, payloadView->size, rtpHeader->timestamp());
}

void VDONinjaSource::processAlphaVP9RtpPacket(const uint8_t *payload, size_t payloadSize, uint32_t rtpTimestamp)
{
	if (payloadSize == 0) {
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

	{
		std::lock_guard<std::mutex> lock(alphaAssemblyMutex_);

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
			processAlphaVideoData(frame.first.data(), frame.first.size(), frame.second);
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
	const int64_t lastVideoTime = lastVideoTime_.load(std::memory_order_relaxed);
	if (videoOutputActive_.load(std::memory_order_relaxed) && lastVideoTime != 0 &&
	    now - lastVideoTime >= kNativeVideoStallBlankMs) {
		if (!loggedVideoStallClear_.exchange(true, std::memory_order_relaxed)) {
			logWarning("No native video packets for %lld ms; clearing stale frame",
			           static_cast<long long>(now - lastVideoTime));
		}
		clearNativeVideoOutput("stale-video-timeout");
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
	if (!isInternalNativeSource()) {
		obs_source_t *child = acquireActiveChildSource();
		const uint32_t childWidth = child ? obs_source_get_width(child) : 0;
		if (child) {
			obs_source_release(child);
		}
		if (childWidth != 0) {
			return childWidth;
		}
	}
	return width_;
}

uint32_t VDONinjaSource::getHeight() const
{
	if (!isInternalNativeSource()) {
		obs_source_t *child = acquireActiveChildSource();
		const uint32_t childHeight = child ? obs_source_get_height(child) : 0;
		if (child) {
			obs_source_release(child);
		}
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
	const bool preferSoftwareForAlpha =
	    isVP9 && preferSoftwareVp9DecodeForAlpha_.load(std::memory_order_relaxed);

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
	pendingVideoDecodeTimestamps_.clear();
	videoHwDecodeConfigured_ = false;
	videoHwStatusLogged_ = false;
	videoHwPixelFormat_ = AV_PIX_FMT_NONE;
	videoHwDeviceName_.clear();
}

void VDONinjaSource::resetAlphaDecoder()
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
	{
		std::lock_guard<std::mutex> lock(pendingAlphaMutex_);
		pendingAlphaFrames_.clear();
	}
	pendingAlphaDecodeTimestamps_.clear();
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

	const int openResult = avcodec_open2(alphaDecoder_, codec, nullptr);
	if (openResult < 0) {
		logError("Failed to open VP9 alpha decoder: %s", ffmpegErrorString(openResult).c_str());
		resetAlphaDecoder();
		return false;
	}

	logInfo("VP9 alpha decoder initialized (software libvpx-vp9)");
	return true;
}

void VDONinjaSource::processAlphaVideoData(const uint8_t *data, size_t size, uint32_t rtpTimestamp)
{
	if (!nativeRunning_.load() || !data || size == 0) {
		return;
	}

	std::lock_guard<std::mutex> lock(alphaDecodeMutex_);
	if (!initializeAlphaDecoder()) {
		return;
	}

	av_packet_unref(alphaPacket_);
	const int allocResult = av_new_packet(alphaPacket_, static_cast<int>(size));
	if (allocResult < 0) {
		return;
	}

	std::memcpy(alphaPacket_->data, data, size);
	const int sendResult = avcodec_send_packet(alphaDecoder_, alphaPacket_);
	if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
		if (!loggedAlphaDecodeSubmitFailure_.exchange(true, std::memory_order_relaxed)) {
			logWarning("Failed to submit VP9 alpha packet: %s", ffmpegErrorString(sendResult).c_str());
		}
		return;
	}
	if (sendResult >= 0) {
		pendingAlphaDecodeTimestamps_.push_back(rtpTimestamp);
	}

	while (true) {
		const int receiveResult = avcodec_receive_frame(alphaDecoder_, alphaFrame_);
		if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
			break;
		}
		if (receiveResult < 0) {
			if (!loggedAlphaDecodeReceiveFailure_.exchange(true, std::memory_order_relaxed)) {
				logWarning("Failed to decode VP9 alpha frame: %s", ffmpegErrorString(receiveResult).c_str());
			}
			break;
		}

		// We only need the Y plane — it carries the alpha values.
		const int w = alphaFrame_->width;
		const int h = alphaFrame_->height;
		const int linesize = alphaFrame_->linesize[0];
		uint32_t decodedRtpTimestamp = rtpTimestamp;
		if (!pendingAlphaDecodeTimestamps_.empty()) {
			decodedRtpTimestamp = pendingAlphaDecodeTimestamps_.front();
			pendingAlphaDecodeTimestamps_.pop_front();
		}
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
				logInfo("Native receiver decoded first alpha frame (%dx%d, format=%d, rtp ts=%u, alpha range=%u-%u)",
				        w, h, alphaFrame_->format, decodedRtpTimestamp, static_cast<unsigned>(minAlpha),
				        static_cast<unsigned>(maxAlpha));
			}
			loggedAlphaDecodeSubmitFailure_.store(false, std::memory_order_relaxed);
			loggedAlphaDecodeReceiveFailure_.store(false, std::memory_order_relaxed);
			PendingAlphaFrame pendingFrame;
			pendingFrame.width = w;
			pendingFrame.height = h;
			pendingFrame.yLinesize = linesize;
			pendingFrame.rtpTimestamp = decodedRtpTimestamp;
			pendingFrame.yData.resize(static_cast<size_t>(linesize) * static_cast<size_t>(h));
			std::memcpy(pendingFrame.yData.data(), alphaFrame_->data[0],
			            static_cast<size_t>(linesize) * static_cast<size_t>(h));

			std::lock_guard<std::mutex> alphaLock(pendingAlphaMutex_);
			upsertPendingAlphaFrame(pendingAlphaFrames_, std::move(pendingFrame));
		}
		av_frame_unref(alphaFrame_);
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
	audioTimingInitialized_ = false;
	audioBaseRtpTimestamp_ = 0;
	audioBaseTimestampNs_ = 0;
	lastAudioTimestampNs_ = 0;
}

void VDONinjaSource::resetNativeState()
{
	std::scoped_lock nativeLock(nativeStateMutex_, videoAssemblyMutex_, videoDecodeMutex_, alphaAssemblyMutex_,
	                            alphaDecodeMutex_, audioDecodeMutex_);
	clearTrackCallbacks(videoTrack_);
	clearTrackCallbacks(alphaVideoTrack_);
	clearTrackCallbacks(audioTrack_);
	loggedFirstVideoRtpPacket_ = false;
	loggedFirstVideoPacket_ = false;
	loggedFirstDecodedVideoFrame_ = false;
	loggedFirstAlphaRtpPacket_ = false;
	loggedFirstAudioPacket_ = false;
	loggedFirstDecodedAudioFrame_ = false;
	loggedFirstDecodedAlphaFrame_ = false;
	loggedAlphaDecodeSubmitFailure_ = false;
	loggedAlphaDecodeReceiveFailure_ = false;
	alphaTrackActive_.store(false, std::memory_order_relaxed);
	preferSoftwareVp9DecodeForAlpha_.store(false, std::memory_order_relaxed);
	loggedAlphaSoftwareDecodeMode_.store(false, std::memory_order_relaxed);
	loggedAlphaCompositionActive_.store(false, std::memory_order_relaxed);
	loggedAlphaTimestampSyncWait_.store(false, std::memory_order_relaxed);
	loggedAlphaTimestampMiss_.store(false, std::memory_order_relaxed);
	loggedAlphaPixelFormatMismatch_.store(false, std::memory_order_relaxed);
	loggedAlphaDimensionMismatch_.store(false, std::memory_order_relaxed);
	alphaTimestampPendingStreak_ = 0;
	alphaTimestampMissStreak_ = 0;
	videoTrack_.reset();
	alphaVideoTrack_.reset();
	audioTrack_.reset();
	videoTrackPeerUuid_.clear();
	alphaVideoTrackPeerUuid_.clear();
	audioTrackPeerUuid_.clear();
	videoRedPayloadTypes_.clear();
	videoAssemblyBuffer_.clear();
	videoAssemblyTimestamp_ = 0;
	videoAssemblyActive_ = false;
	alphaAssemblyBuffer_.clear();
	alphaAssemblyTimestamp_ = 0;
	alphaAssemblyActive_ = false;
	videoHwDecodeDisabled_ = false;
	videoOutputActive_.store(false, std::memory_order_relaxed);
	loggedVideoStallClear_.store(false, std::memory_order_relaxed);
	resetVideoDecoder();
	resetAlphaDecoder();
	resetAudioDecoder();
	if (source_) {
		setObsSourceAudioActiveSafe(source_, false);
		clearNativeVideoOutput("reset-native-state");
	}
}

void VDONinjaSource::handlePeerDisconnected(const std::string &uuid)
{
	bool videoRemoved = false;
	bool audioRemoved = false;

	{
		std::scoped_lock nativeLock(nativeStateMutex_, videoAssemblyMutex_, videoDecodeMutex_, alphaAssemblyMutex_,
		                            alphaDecodeMutex_, audioDecodeMutex_);

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

		if (!uuid.empty() && uuid == alphaVideoTrackPeerUuid_) {
			clearTrackCallbacks(alphaVideoTrack_);
			alphaVideoTrack_.reset();
			alphaVideoTrackPeerUuid_.clear();
			alphaAssemblyBuffer_.clear();
			alphaAssemblyTimestamp_ = 0;
			alphaAssemblyActive_ = false;
			alphaTrackActive_.store(false, std::memory_order_relaxed);
			preferSoftwareVp9DecodeForAlpha_.store(false, std::memory_order_relaxed);
			loggedAlphaSoftwareDecodeMode_.store(false, std::memory_order_relaxed);
			loggedAlphaCompositionActive_.store(false, std::memory_order_relaxed);
			loggedAlphaTimestampSyncWait_.store(false, std::memory_order_relaxed);
			loggedAlphaTimestampMiss_.store(false, std::memory_order_relaxed);
			loggedAlphaPixelFormatMismatch_.store(false, std::memory_order_relaxed);
			loggedAlphaDimensionMismatch_.store(false, std::memory_order_relaxed);
			alphaTimestampPendingStreak_ = 0;
			alphaTimestampMissStreak_ = 0;
			loggedFirstDecodedAlphaFrame_.store(false, std::memory_order_relaxed);
			loggedAlphaDecodeSubmitFailure_.store(false, std::memory_order_relaxed);
			loggedAlphaDecodeReceiveFailure_.store(false, std::memory_order_relaxed);
			resetAlphaDecoder();
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
		setObsSourceAudioActiveSafe(source_, false);
	}

	if (videoRemoved && source_) {
		clearNativeVideoOutput("peer-disconnected");
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

void VDONinjaSource::outputDecodedVideoFrame(const AVFrame *frame, uint64_t timestampNs, uint32_t rtpTimestamp)
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

	// Check for a pending alpha Y-plane (VP9 alpha dual-track support).
	// We require a matching RTP timestamp so stale alpha cannot bleed into newer
	// primary frames if network ordering shifts between the two tracks.
	std::vector<uint8_t> alphaYCopy;
	int alphaYLinesize = 0;
	bool hasAlpha = false;
	bool alphaTimestampPending = false;
	bool alphaDimensionsMismatch = false;
	int alphaWidth = 0;
	int alphaHeight = 0;
	{
		std::lock_guard<std::mutex> alphaLock(pendingAlphaMutex_);
		const auto alphaResult =
		    consumePendingAlphaFrame(pendingAlphaFrames_, rtpTimestamp, frame->width, frame->height);
		if (alphaResult.hasMatch) {
			if (alphaResult.dimensionsMatch) {
				alphaYCopy = alphaResult.yData;
				alphaYLinesize = alphaResult.yLinesize;
				hasAlpha = true;
			} else {
				alphaDimensionsMismatch = true;
				alphaWidth = alphaResult.width;
				alphaHeight = alphaResult.height;
			}
		} else {
			alphaTimestampPending = alphaResult.futureFramePending;
		}
	}

	if (alphaDimensionsMismatch &&
	    !loggedAlphaDimensionMismatch_.exchange(true, std::memory_order_relaxed)) {
		logWarning("Discarded VP9 alpha frame for RTP timestamp %u because dimensions did not match primary "
		           "video (%dx%d vs %dx%d)",
		           rtpTimestamp, alphaWidth, alphaHeight, frame->width, frame->height);
	}
	const bool alphaTrackActive = alphaTrackActive_.load(std::memory_order_relaxed);
	if (!alphaTrackActive || hasAlpha || alphaDimensionsMismatch) {
		alphaTimestampPendingStreak_ = 0;
		alphaTimestampMissStreak_ = 0;
	} else if (alphaTimestampPending) {
		alphaTimestampPendingStreak_++;
		alphaTimestampMissStreak_ = 0;
	} else {
		alphaTimestampPendingStreak_ = 0;
		alphaTimestampMissStreak_++;
	}
	constexpr uint32_t kAlphaTimestampLogThreshold = 15;
	if (!hasAlpha && alphaTrackActive && alphaTimestampPending &&
	    alphaTimestampPendingStreak_ >= kAlphaTimestampLogThreshold &&
	    !loggedAlphaTimestampSyncWait_.exchange(true, std::memory_order_relaxed)) {
		logInfo("Waiting for matching VP9 alpha RTP timestamp before compositing transparency");
	}
	if (!hasAlpha && alphaTrackActive && !alphaTimestampPending &&
	    alphaTimestampMissStreak_ >= kAlphaTimestampLogThreshold &&
	    !loggedAlphaTimestampMiss_.exchange(true, std::memory_order_relaxed)) {
		logInfo("No matching VP9 alpha frame available for primary RTP timestamp %u", rtpTimestamp);
	}

	// Build a synthetic YUVA420P view from the primary YUV planes + alpha Y plane.
	// We only do this for planar YUV420P output (the typical SW decode result).
	AVFrame yuvaView = {};
	const AVFrame *frameToScale = frame;
	const AVPixelFormat primaryFmt = static_cast<AVPixelFormat>(frame->format);
	if (hasAlpha && (primaryFmt == AV_PIX_FMT_YUV420P || primaryFmt == AV_PIX_FMT_YUVJ420P) && frame->data[0] &&
	    frame->data[1] && frame->data[2]) {
		yuvaView.width = frame->width;
		yuvaView.height = frame->height;
		yuvaView.format = AV_PIX_FMT_YUVA420P;
		yuvaView.data[0] = frame->data[0];
		yuvaView.data[1] = frame->data[1];
		yuvaView.data[2] = frame->data[2];
		yuvaView.data[3] = alphaYCopy.data();
		yuvaView.linesize[0] = frame->linesize[0];
		yuvaView.linesize[1] = frame->linesize[1];
		yuvaView.linesize[2] = frame->linesize[2];
		yuvaView.linesize[3] = alphaYLinesize;
		frameToScale = &yuvaView;
		if (!loggedAlphaCompositionActive_.exchange(true, std::memory_order_relaxed)) {
			logInfo("Native receiver alpha composition active");
		}
		alphaTimestampPendingStreak_ = 0;
		alphaTimestampMissStreak_ = 0;
		loggedAlphaTimestampSyncWait_.store(false, std::memory_order_relaxed);
		loggedAlphaTimestampMiss_.store(false, std::memory_order_relaxed);
		loggedAlphaPixelFormatMismatch_.store(false, std::memory_order_relaxed);
		loggedAlphaDimensionMismatch_.store(false, std::memory_order_relaxed);
	} else if (hasAlpha && !loggedAlphaPixelFormatMismatch_.exchange(true, std::memory_order_relaxed)) {
		logWarning("Skipping VP9 alpha composition because primary frame format %s is not planar YUV420P",
		           pixelFormatName(primaryFmt));
	}

	const AVPixelFormat inputFormat = static_cast<AVPixelFormat>(frameToScale->format);
	const AspectFitLayout layout = computeAspectFitLayout(static_cast<uint32_t>(frameToScale->width),
	                                                      static_cast<uint32_t>(frameToScale->height), width_, height_);
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

	obs_source_frame obsFrame = {};
	obsFrame.width = layout.outputWidth;
	obsFrame.height = layout.outputHeight;
	obsFrame.format = VIDEO_FORMAT_BGRA;
	obsFrame.timestamp = timestampNs;
	obsFrame.full_range = true;
	obsFrame.data[0] = output.data();
	obsFrame.linesize[0] = static_cast<uint32_t>(outputStride);
	videoOutputActive_.store(true, std::memory_order_relaxed);
	loggedVideoStallClear_.store(false, std::memory_order_relaxed);
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

	setObsSourceAudioActiveSafe(source_, true);
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
	if (isInternalNativeSource() || !usingNativeReceiver() || settings_.streamId.empty()) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (nativeReceiverSource_) {
			return;
		}
	}

	obs_data_t *nativeSettings = createNativeReceiverSourceSettings(settings_, width_, height_);
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
	setObsSourceAudioActiveSafe(source_, obs_source_audio_active(created));
	syncChildLifecycleState(created);
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		nativeReceiverSettings_ = settings_;
		nativeReceiverWidth_ = width_;
		nativeReceiverHeight_ = height_;
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

	setObsSourceAudioActiveSafe(source_, false);
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

	if (configApplied && configuredWidth == width_ && configuredHeight == height_ &&
	    sourceSettingsEqualForChild(configuredSettings, settings_)) {
		syncChildLifecycleState(child);
		obs_source_release(child);
		return;
	}

	obs_data_t *nativeSettings = createNativeReceiverSourceSettings(settings_, width_, height_);
	obs_source_update(child, nativeSettings);
	obs_data_release(nativeSettings);
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (nativeReceiverSource_ == child) {
			nativeReceiverSettings_ = settings_;
			nativeReceiverWidth_ = width_;
			nativeReceiverHeight_ = height_;
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

	obs_data_t *browserSettings = createBrowserSourceSettings(url, width_, height_);
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
	setObsSourceAudioActiveSafe(source_, obs_source_audio_active(created));
	syncChildLifecycleState(created);
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		browserSourceUrl_ = url;
		browserSourceWidth_ = width_;
		browserSourceHeight_ = height_;
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

	setObsSourceAudioActiveSafe(source_, false);
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

	if (configApplied && configuredWidth == width_ && configuredHeight == height_ && configuredUrl == url) {
		syncChildLifecycleState(child);
		obs_source_release(child);
		return;
	}

	obs_data_t *browserSettings = createBrowserSourceSettings(url, width_, height_);
	obs_source_update(child, browserSettings);
	obs_data_release(browserSettings);
	{
		std::lock_guard<std::mutex> lock(childSourceMutex_);
		if (browserSource_ == child) {
			browserSourceUrl_ = url;
			browserSourceWidth_ = width_;
			browserSourceHeight_ = height_;
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
	setObsSourceAudioActiveSafe(source_, true);
}

void VDONinjaSource::onChildAudioDeactivated()
{
	setObsSourceAudioActiveSafe(source_, false);
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
