// Test-only source: derive its counter from the OBS video clock, without a
// separately clocked file decoder or a browser-source GPU process.
#include <obs-module.h>

#include <graphics/vec4.h>

#include "counter.h"

OBS_DECLARE_MODULE()

namespace
{
struct ClockSource {
	uint64_t start = 0;
	uint16_t frame = 0;
};
void tick(void *data, float)
{
	auto &source = *static_cast<ClockSource *>(data);
	const uint64_t now = obs_get_video_frame_time();
	if (!source.start)
		source.start = now;
	obs_video_info info{};
	if (obs_get_video_info(&info) && info.fps_den) {
		source.frame = obsClockMarker(now - source.start, info.fps_num, info.fps_den);
	}
}
void rectangle(float x, float y, uint32_t width, uint32_t height, float level)
{
	vec4 color;
	vec4_set(&color, level, level, level, 1.0f);
	auto *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0);
	while (gs_effect_loop(effect, "Solid"))
		gs_draw_sprite(nullptr, 0, width, height);
	gs_matrix_pop();
}
void render(void *data, gs_effect_t *)
{
	const auto frame = static_cast<ClockSource *>(data)->frame;
	rectangle(0, 0, 1920, 1080, 0.15f);
	// Same sync / 16-bit counter / complemented low byte as the MP4 fixture.
	const uint32_t marker = (0xd3u << 24) | (uint32_t(frame) << 8) | ((~frame) & 255u);
	rectangle(192, 920, 1536, 96, 0.5f);
	for (unsigned cell = 0; cell < 32; ++cell) {
		rectangle(float(195 + cell * 48), 923, 42, 90, (marker & (1u << (31 - cell))) ? 1.0f : 0.0f);
	}
}
} // namespace

bool obs_module_load(void)
{
	obs_source_info info{};
	info.id = "vdoninja_clock_fixture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = [](void *) { return "VDO.Ninja test clock (fixture only)"; };
	info.create = [](obs_data_t *, obs_source_t *) -> void * { return new ClockSource; };
	info.destroy = [](void *p) { delete static_cast<ClockSource *>(p); };
	info.get_width = [](void *) -> uint32_t { return 1920; };
	info.get_height = [](void *) -> uint32_t { return 1080; };
	info.video_tick = tick;
	info.video_render = render;
	obs_register_source(&info);
	return true;
}
