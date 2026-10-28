#pragma once

#include "obs.h"
inline const char *obs_module_text(const char *text)
{
	return text;
}
inline obs_property_t *obs_properties_add_text(obs_properties_t *, const char *, const char *, int)
{
	return nullptr;
}
inline void obs_property_set_long_description(obs_property_t *, const char *) {}
inline obs_property_t *obs_properties_add_bool(obs_properties_t *, const char *, const char *)
{
	return nullptr;
}
