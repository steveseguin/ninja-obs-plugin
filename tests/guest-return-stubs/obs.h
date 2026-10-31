#pragma once

// Deliberately no frontend streaming APIs: the return must own its resources.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

enum obs_encoder_type { OBS_ENCODER_VIDEO, OBS_ENCODER_AUDIO };
enum obs_data_type { OBS_DATA_NUMBER, OBS_DATA_BOOLEAN };
constexpr int OBS_ENCODER_CAP_DEPRECATED = 1;
constexpr int OBS_ENCODER_CAP_INTERNAL = 8;
constexpr int OBS_OUTPUT_SUCCESS = 0;
constexpr int OBS_TEXT_DEFAULT = 0;
struct obs_data_t {
	std::map<std::string, std::string> strings;
	std::map<std::string, int64_t> numbers;
};
struct obs_data_item_t {
};
struct obs_properties_t {
};
struct obs_property_t {
};
struct calldata_t {
	int code = 0;
};
using signal_callback_t = void (*)(void *, calldata_t *);
struct signal_handler_t {
	signal_callback_t callback = nullptr;
	void *data = nullptr;
};
struct obs_encoder_t {
	std::string id;
	obs_data_t settings;
	size_t mixer = 0;
	int refs = 1;
	uint32_t width = 0, height = 0, divisor = 1;
};
struct obs_service_t {
	obs_data_t settings;
};
struct obs_output_t {
	obs_data_t settings;
	obs_service_t *service = nullptr;
	obs_encoder_t *video = nullptr;
	obs_encoder_t *audio = nullptr;
	size_t audioSlot = 99;
	signal_handler_t signals;
};

namespace guest_stub
{
inline int outputs = 0, services = 0, encoders = 0, forceStops = 0;
inline bool failService = false, failOutput = false, failVideo = false, failAudio = false, failStart = false;
inline std::vector<std::string> types = {"obs_x264", "ffmpeg_opus", "obs_nvenc_h264_tex", "hevc"};
} // namespace guest_stub

inline obs_data_t *obs_data_create()
{
	return new obs_data_t;
}
inline void obs_data_release(obs_data_t *d)
{
	delete d;
}
inline void obs_data_apply(obs_data_t *d, obs_data_t *from)
{
	*d = *from;
}
inline void obs_data_set_string(obs_data_t *d, const char *k, const char *v)
{
	d->strings[k] = v;
}
inline const char *obs_data_get_string(obs_data_t *d, const char *k)
{
	return d->strings[k].c_str();
}
inline void obs_data_set_int(obs_data_t *d, const char *k, int64_t v)
{
	d->numbers[k] = v;
}
inline int64_t obs_data_get_int(obs_data_t *d, const char *k)
{
	return d->numbers[k];
}
inline void obs_data_set_bool(obs_data_t *d, const char *k, bool v)
{
	d->numbers[k] = v;
}
inline bool obs_data_get_bool(obs_data_t *d, const char *k)
{
	return d->numbers[k] != 0;
}
inline obs_data_item_t *obs_data_item_byname(obs_data_t *, const char *)
{
	return nullptr;
}
inline obs_data_type obs_data_item_gettype(obs_data_item_t *)
{
	return OBS_DATA_BOOLEAN;
}
inline void obs_data_item_release(obs_data_item_t **) {}
inline bool obs_enum_encoder_types(size_t i, const char **id)
{
	if (i >= guest_stub::types.size())
		return false;
	*id = guest_stub::types[i].c_str();
	return true;
}
inline const char *obs_get_encoder_codec(const char *id)
{
	return std::string(id) == "ffmpeg_opus" ? "opus" : std::string(id) == "hevc" ? "hevc" : "h264";
}
inline obs_encoder_type obs_get_encoder_type(const char *id)
{
	return std::string(id) == "ffmpeg_opus" ? OBS_ENCODER_AUDIO : OBS_ENCODER_VIDEO;
}
inline uint32_t obs_get_encoder_caps(const char *)
{
	return 0;
}
inline const char *obs_encoder_get_display_name(const char *id)
{
	return id;
}
inline obs_data_t *obs_encoder_defaults(const char *)
{
	return obs_data_create();
}
inline obs_encoder_t *obs_video_encoder_create(const char *id, const char *, obs_data_t *d, obs_data_t *)
{
	if (guest_stub::failVideo)
		return nullptr;
	++guest_stub::encoders;
	return new obs_encoder_t{id, *d};
}
inline obs_encoder_t *obs_audio_encoder_create(const char *id, const char *, obs_data_t *d, size_t mixer, obs_data_t *)
{
	if (guest_stub::failAudio)
		return nullptr;
	++guest_stub::encoders;
	return new obs_encoder_t{id, *d, mixer};
}
inline void obs_encoder_release(obs_encoder_t *e)
{
	if (e && --e->refs == 0) {
		--guest_stub::encoders;
		delete e;
	}
}
inline void *obs_get_video()
{
	return nullptr;
}
inline void *obs_get_audio()
{
	return nullptr;
}
inline void obs_encoder_set_video(obs_encoder_t *, void *) {}
struct obs_video_info {
	uint32_t output_width = 1920, output_height = 1080;
};
inline bool obs_get_video_info(obs_video_info *info)
{
	*info = {};
	return true;
}
inline bool obs_encoder_set_frame_rate_divisor(obs_encoder_t *encoder, uint32_t divisor)
{
	encoder->divisor = divisor;
	return true;
}
inline void obs_encoder_set_scaled_size(obs_encoder_t *encoder, uint32_t width, uint32_t height)
{
	encoder->width = width;
	encoder->height = height;
}
inline void obs_encoder_set_audio(obs_encoder_t *, void *) {}
inline obs_service_t *obs_service_create(const char *, const char *, obs_data_t *d, obs_data_t *)
{
	if (guest_stub::failService)
		return nullptr;
	++guest_stub::services;
	return new obs_service_t{*d};
}
inline void obs_service_release(obs_service_t *s)
{
	if (s) {
		--guest_stub::services;
		delete s;
	}
}
inline const char *obs_service_get_type(obs_service_t *)
{
	return "vdoninja_service";
}
inline obs_data_t *obs_service_get_settings(obs_service_t *s)
{
	return new obs_data_t(s->settings);
}
inline obs_output_t *obs_output_create(const char *, const char *, obs_data_t *d, obs_data_t *)
{
	if (guest_stub::failOutput)
		return nullptr;
	++guest_stub::outputs;
	return new obs_output_t{*d};
}
inline void obs_output_set_service(obs_output_t *o, obs_service_t *s)
{
	o->service = s;
}
inline void obs_output_set_video_encoder(obs_output_t *o, obs_encoder_t *e)
{
	o->video = e;
	++e->refs;
}
inline void obs_output_set_audio_encoder(obs_output_t *o, obs_encoder_t *e, size_t slot)
{
	o->audio = e;
	o->audioSlot = slot;
	++e->refs;
}
inline void obs_output_set_reconnect_settings(obs_output_t *, int, int) {}
inline signal_handler_t *obs_output_get_signal_handler(obs_output_t *o)
{
	return &o->signals;
}
inline void signal_handler_connect(signal_handler_t *s, const char *, signal_callback_t cb, void *data)
{
	s->callback = cb;
	s->data = data;
}
inline void signal_handler_disconnect(signal_handler_t *s, const char *, signal_callback_t, void *)
{
	s->callback = nullptr;
}
inline int64_t calldata_int(calldata_t *d, const char *)
{
	return d->code;
}
inline bool obs_output_start(obs_output_t *)
{
	return !guest_stub::failStart;
}
inline void obs_output_force_stop(obs_output_t *)
{
	++guest_stub::forceStops;
}
inline void obs_output_release(obs_output_t *o)
{
	obs_encoder_release(o->video);
	obs_encoder_release(o->audio);
	--guest_stub::outputs;
	delete o;
}
