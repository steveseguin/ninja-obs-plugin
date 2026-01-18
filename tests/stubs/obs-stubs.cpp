/*
 * OBS Studio API Stubs for Testing
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "obs-module.h"
#include <cstdarg>
#include <cstdio>

// Global flag to enable/disable test logging output
static bool g_test_logging_enabled = false;

extern "C" {

void blog(int log_level, const char *format, ...)
{
    if (!g_test_logging_enabled) {
        return;
    }

    const char *level_str;
    switch (log_level) {
    case LOG_ERROR:
        level_str = "ERROR";
        break;
    case LOG_WARNING:
        level_str = "WARNING";
        break;
    case LOG_INFO:
        level_str = "INFO";
        break;
    case LOG_DEBUG:
        level_str = "DEBUG";
        break;
    default:
        level_str = "UNKNOWN";
        break;
    }

    fprintf(stderr, "[%s] ", level_str);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");
}

} // extern "C"

// Helper to enable/disable logging in tests
namespace testing_utils
{
void enableLogging(bool enable)
{
    g_test_logging_enabled = enable;
}
} // namespace testing_utils
