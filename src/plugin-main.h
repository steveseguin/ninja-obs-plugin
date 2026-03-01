/*
 * OBS VDO.Ninja Plugin
 * Main plugin header
 */

#pragma once

#include <obs-module.h>

#define PLUGIN_VERSION "1.1.16"

#ifdef __cplusplus
extern "C" {
#endif

const char *obs_module_name(void);
const char *obs_module_description(void);
bool obs_module_load(void);
void obs_module_unload(void);

#ifdef __cplusplus
}

bool activateVdoNinjaServiceFromSettings(obs_data_t *sourceSettings, bool generateStreamIdIfMissing,
                                         bool temporarySwitch);

// Forward chat messages from output thread to dock (must be called on UI thread)
void vdo_dock_show_chat(const char *sender, const char *message);

// Handle remote control actions from data channel (must be called on UI thread)
void vdo_handle_remote_control(const char *action, const char *value);
#endif
