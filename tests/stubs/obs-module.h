/*
 * OBS Studio API Stubs for Testing
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstdarg>
#include <cstdio>

// Log levels matching OBS
#define LOG_ERROR 100
#define LOG_WARNING 200
#define LOG_INFO 300
#define LOG_DEBUG 400

#ifdef __cplusplus
extern "C" {
#endif

// Stub for OBS logging function
void blog(int log_level, const char *format, ...);

#ifdef __cplusplus
}
#endif
