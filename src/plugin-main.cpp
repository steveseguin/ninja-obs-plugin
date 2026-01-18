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

#include <obs-frontend-api.h>

#include "vdoninja-output.h"
#include "vdoninja-source.h"
#include "vdoninja-utils.h"

using namespace vdoninja;

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
	return obs_module_text("VDONinjaService");
}

static void *vdoninja_service_create(obs_data_t *settings, obs_service_t *service)
{
	UNUSED_PARAMETER(service);

	// Service just holds settings, actual work is in output
	obs_data_t *data = obs_data_create();
	obs_data_apply(data, settings);
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
	obs_data_apply(svc_settings, settings);
}

static obs_properties_t *vdoninja_service_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "stream_id", obs_module_text("StreamID"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "room_id", obs_module_text("RoomID"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "password", obs_module_text("Password"), OBS_TEXT_PASSWORD);

	obs_property_t *codec = obs_properties_add_list(props, "video_codec", obs_module_text("VideoCodec"),
	                                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(codec, "H.264", 0);
	obs_property_list_add_int(codec, "VP8", 1);
	obs_property_list_add_int(codec, "VP9", 2);

	obs_properties_add_int(props, "max_viewers", obs_module_text("MaxViewers"), 1, 50, 1);

	return props;
}

static void vdoninja_service_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "stream_id", "");
	obs_data_set_default_string(settings, "room_id", "");
	obs_data_set_default_string(settings, "password", "");
	obs_data_set_default_int(settings, "video_codec", 0);
	obs_data_set_default_int(settings, "max_viewers", 10);
}

static const char *vdoninja_service_url(void *data)
{
	UNUSED_PARAMETER(data);
	return "https://vdo.ninja";
}

static const char *vdoninja_service_key(void *data)
{
	obs_data_t *settings = static_cast<obs_data_t *>(data);
	return obs_data_get_string(settings, "stream_id");
}

static bool vdoninja_service_can_try_connect(void *data)
{
	obs_data_t *settings = static_cast<obs_data_t *>(data);
	const char *stream_id = obs_data_get_string(settings, "stream_id");
	return stream_id && *stream_id;
}

static obs_service_info vdoninja_service_info = {
    .id = "vdoninja_service",
    .get_name = vdoninja_service_getname,
    .create = vdoninja_service_create,
    .destroy = vdoninja_service_destroy,
    .update = vdoninja_service_update,
    .get_defaults = vdoninja_service_defaults,
    .get_properties = vdoninja_service_properties,
    .get_url = vdoninja_service_url,
    .get_key = vdoninja_service_key,
    .get_output_type = [](void *) -> const char * { return "vdoninja_output"; },
    .can_try_to_connect = vdoninja_service_can_try_connect,
};

// Frontend event callback for virtual camera integration
static void frontend_event_callback(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
		logInfo("Virtual camera started");
		// Could optionally auto-start VDO.Ninja output here
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		logInfo("Virtual camera stopped");
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		logInfo("Streaming started");
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		logInfo("Streaming stopped");
		break;
	default:
		break;
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

	// Register service
	obs_register_service(&vdoninja_service_info);
	logInfo("Registered VDO.Ninja service");

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

	logInfo("VDO.Ninja plugin unloaded");
}
