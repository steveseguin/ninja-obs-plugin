#include "vdoninja-guest-return.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "vdoninja-publish-obs.h"
#include "vdoninja-utils.h"

namespace vdoninja
{

static std::string opusEncoder()
{
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
		const char *codec = obs_get_encoder_codec(id);
		if (obs_get_encoder_type(id) == OBS_ENCODER_AUDIO && codec && std::strcmp(codec, "opus") == 0 &&
		    !(obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)))
			return id;
	}
	return {};
}

std::vector<GuestReturnEncoder> GuestReturnOutput::videoEncoders()
{
	std::vector<GuestReturnEncoder> result;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
		const char *codec = obs_get_encoder_codec(id);
		if (obs_get_encoder_type(id) == OBS_ENCODER_VIDEO && codec && std::strcmp(codec, "h264") == 0 &&
		    !(obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)))
			result.push_back({id, obs_encoder_get_display_name(id)});
	}
	// Prefer known hardware encoders when available, with x264 as a portable fallback.
	const auto rank = [](const std::string &id) {
		if (id.find("nvenc") != std::string::npos)
			return 0;
		if (id.find("qsv") != std::string::npos || id.find("amf") != std::string::npos ||
		    id.find("videotoolbox") != std::string::npos)
			return 1;
		return 2;
	};
	std::stable_sort(result.begin(), result.end(),
	                 [&](const auto &a, const auto &b) { return rank(a.id) < rank(b.id); });
	return result;
}

GuestReturnOutput::~GuestReturnOutput()
{
	stop();
}

bool GuestReturnOutput::start(const GuestReturnSettings &settings)
{
	if (busy())
		return false;
	stop();
	clearError();
	if (settings.identity.streamId.empty() || settings.audioTrack < 1 || settings.audioTrack > 6 ||
	    settings.videoBitrate < 100 || settings.videoBitrate > 50000 || settings.maxViewers < 1 ||
	    settings.maxViewers > 50 || settings.frameRateDivisor < 1 || settings.frameRateDivisor > 4 ||
	    (settings.maxHeight != 0 && settings.maxHeight != 360 && settings.maxHeight != 540 &&
	     settings.maxHeight != 720 && settings.maxHeight != 1080)) {
		error_ = "GuestReturn.InvalidSettings";
		return false;
	}
	if (settings.forceTurn) {
		const auto servers = parseIceServers(settings.customIceServers);
		if (std::none_of(servers.begin(), servers.end(), [](const auto &server) {
			    std::string url = server.urls;
			    std::transform(url.begin(), url.end(), url.begin(),
			                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			    return url.rfind("turn:", 0) == 0 || url.rfind("turns:", 0) == 0;
		    })) {
			error_ = "GuestReturn.TurnRequired";
			return false;
		}
	}
	const auto encoders = videoEncoders();
	const auto audioId = opusEncoder();
	if (audioId.empty() || std::none_of(encoders.begin(), encoders.end(),
	                                    [&](const auto &encoder) { return encoder.id == settings.videoEncoder; })) {
		error_ = "GuestReturn.EncoderUnavailable";
		return false;
	}
	settings_ = settings;
	auto *data = obs_data_create();
	writePublishIdentity(data, settings.identity);
	obs_data_set_int(data, "max_viewers", settings.maxViewers);
	obs_data_set_string(data, "custom_ice_servers", settings.customIceServers.c_str());
	obs_data_set_bool(data, "force_turn", settings.forceTurn);
	obs_data_set_bool(data, "auto_inbound_enabled", false);
	obs_data_set_bool(data, "enable_remote", false);
	obs_data_set_int(data, "audio_encoder_index", 0);
	service_ = obs_service_create("vdoninja_service", "VDO.Ninja Guest Return", data, nullptr);
	output_ = obs_output_create("vdoninja_output", "VDO.Ninja Guest Return", data, nullptr);
	obs_data_release(data);
	if (!service_ || !output_) {
		error_ = "GuestReturn.CreateFailed";
		stop();
		return false;
	}
	obs_output_set_service(output_, service_);
	obs_output_set_reconnect_settings(output_, 0, 0); // Transport owns its reconnect behavior.

	auto *videoSettings = obs_encoder_defaults(settings.videoEncoder.c_str());
	obs_data_set_string(videoSettings, "rate_control", "CBR");
	obs_data_set_int(videoSettings, "bitrate", settings.videoBitrate);
	applyPublishEncoderCompatibility(videoSettings);
	auto *video =
	    obs_video_encoder_create(settings.videoEncoder.c_str(), "VDO.Ninja Guest Return Video", videoSettings, nullptr);
	obs_data_release(videoSettings);
	auto *audioSettings = obs_encoder_defaults(audioId.c_str());
	obs_data_set_int(audioSettings, "bitrate", 128);
	auto *audio = obs_audio_encoder_create(audioId.c_str(), "VDO.Ninja Guest Return Audio", audioSettings,
	                                       static_cast<size_t>(settings.audioTrack - 1), nullptr);
	obs_data_release(audioSettings);
	bool videoConfigured = video != nullptr;
	if (video) {
		obs_encoder_set_video(video, obs_get_video());
		videoConfigured = obs_encoder_set_frame_rate_divisor(video, settings.frameRateDivisor);
		if (settings.maxHeight) {
			obs_video_info info = {};
			if (!obs_get_video_info(&info) || !info.output_width || !info.output_height) {
				videoConfigured = false;
			} else if (info.output_height > static_cast<uint32_t>(settings.maxHeight)) {
				const auto width = static_cast<uint32_t>(static_cast<uint64_t>(info.output_width) * settings.maxHeight /
				                                         info.output_height);
				obs_encoder_set_scaled_size(video, std::max(2u, width & ~1u), settings.maxHeight);
			}
		}
		obs_output_set_video_encoder(output_, video);
		obs_encoder_release(video);
	}
	if (audio) {
		obs_encoder_set_audio(audio, obs_get_audio());
		obs_output_set_audio_encoder(output_, audio, 0);
		obs_encoder_release(audio);
	}
	if (!videoConfigured || !audio) {
		error_ = "GuestReturn.EncoderFailed";
		stop();
		return false;
	}
	signal_handler_connect(obs_output_get_signal_handler(output_), "stop", onStopped, this);
	stopped_ = false;
	startAttempted_ = true;
	if (!obs_output_start(output_)) {
		error_ = "GuestReturn.StartFailed";
		stop();
		return false;
	}
	return true;
}

void GuestReturnOutput::stop()
{
	if (output_) {
		signal_handler_disconnect(obs_output_get_signal_handler(output_), "stop", onStopped, this);
		// force_stop also cancels a publisher still waiting for signaling, before
		// libobs considers it active. Release waits for encoder capture to drain.
		if (startAttempted_)
			obs_output_force_stop(output_);
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (service_) {
		obs_service_release(service_);
		service_ = nullptr;
	}
	startAttempted_ = false;
	stopped_ = true;
}

void GuestReturnOutput::onStopped(void *data, calldata_t *params)
{
	auto *self = static_cast<GuestReturnOutput *>(data);
	self->stopCode_ = static_cast<int>(calldata_int(params, "code"));
	self->stopped_ = true;
}

void GuestReturnOutput::clearError()
{
	error_.clear();
	stopCode_ = 0;
}

std::string GuestReturnOutput::error() const
{
	if (!error_.empty())
		return error_;
	return stopCode_.load() != OBS_OUTPUT_SUCCESS ? "GuestReturn.ConnectionFailed" : "";
}

} // namespace vdoninja
