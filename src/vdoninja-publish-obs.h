#pragma once

#include <obs-module.h>

#include <cstring>

#include "vdoninja-publish-settings.h"

namespace vdoninja
{

inline bool isVdoNinjaPublishService(obs_service_t *service)
{
	if (!service)
		return false;
	const char *type = obs_service_get_type(service);
	if (!type)
		return false;
	if (std::strcmp(type, "vdoninja_service") == 0)
		return true;
	if (std::strcmp(type, "rtmp_common") != 0)
		return false;
	obs_data_t *settings = obs_service_get_settings(service);
	const bool matches = std::strcmp(obs_data_get_string(settings, "service"), "VDO.Ninja") == 0;
	obs_data_release(settings);
	return matches;
}

inline PublishIdentity readPublishIdentity(obs_data_t *settings, bool keyOnly = false)
{
	PublishIdentity identity;
	if (!settings)
		return identity;
	if (!keyOnly) {
		identity = {obs_data_get_string(settings, "stream_id"), obs_data_get_string(settings, "password"),
		            obs_data_get_string(settings, "room_id"), obs_data_get_string(settings, "salt"),
		            obs_data_get_string(settings, "wss_host")};
	}
	parsePublishStreamKey(obs_data_get_string(settings, "key"), identity);
	if (identity.wssHost.empty())
		identity.wssHost = obs_data_get_string(settings, "server");
	return identity;
}

inline void writePublishIdentity(obs_data_t *settings, const PublishIdentity &identity)
{
	obs_data_set_string(settings, "stream_id", identity.streamId.c_str());
	obs_data_set_string(settings, "password", identity.password.c_str());
	obs_data_set_string(settings, "room_id", identity.roomId.c_str());
	obs_data_set_string(settings, "salt", identity.salt.c_str());
	obs_data_set_string(settings, "wss_host", identity.wssHost.c_str());
	// OBS's Stream dialog can re-create our service as rtmp_common. Keep its
	// Stream Key representation in sync, including password/room/salt/signaling.
	obs_data_set_string(settings, "key", buildPublishUrl(identity, true).c_str());
}

inline obs_data_t *copyPublishServiceSettings(obs_service_t *service)
{
	if (!isVdoNinjaPublishService(service))
		return nullptr;
	obs_data_t *original = obs_service_get_settings(service);
	obs_data_t *copy = obs_data_create();
	obs_data_apply(copy, original);
	const bool common = std::strcmp(obs_service_get_type(service), "rtmp_common") == 0;
	writePublishIdentity(copy, readPublishIdentity(original, common));
	obs_data_release(original);
	return copy;
}

inline void applyPublishEncoderCompatibility(obs_data_t *settings)
{
	if (!settings)
		return;
	obs_data_set_int(settings, "bf", 0);
	// QSV uses a frame count; VideoToolbox uses a boolean with the same name.
	obs_data_item_t *bframes = obs_data_item_byname(settings, "bframes");
	const bool numericBframes = bframes && obs_data_item_gettype(bframes) == OBS_DATA_NUMBER;
	obs_data_item_release(&bframes);
	if (numericBframes)
		obs_data_set_int(settings, "bframes", 0);
	else
		obs_data_set_bool(settings, "bframes", false);
	obs_data_set_bool(settings, "repeat_headers", true);
	obs_data_set_int(settings, "keyint_sec", publishKeyframeInterval(obs_data_get_int(settings, "keyint_sec")));
}

inline obs_property_t *addPublishUdpPortProperty(obs_properties_t *properties)
{
	auto *property =
	    obs_properties_add_text(properties, "udp_port_range", obs_module_text("UDPPorts"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(property, obs_module_text("UDPPorts.Help"));
	return property;
}

} // namespace vdoninja
