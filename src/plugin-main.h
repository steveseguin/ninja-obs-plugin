/*
 * OBS VDO.Ninja Plugin
 * Main plugin header
 */

#pragma once

#include <obs-module.h>

#define PLUGIN_VERSION "1.0.0"

#ifdef __cplusplus
extern "C" {
#endif

// Plugin module functions
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-vdoninja", "en-US")

const char *obs_module_name(void);
const char *obs_module_description(void);
bool obs_module_load(void);
void obs_module_unload(void);

#ifdef __cplusplus
}
#endif
