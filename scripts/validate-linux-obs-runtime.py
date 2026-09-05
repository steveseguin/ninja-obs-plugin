#!/usr/bin/env python3
"""Check OBS module compatibility and optionally initialize the plugin under Xvfb."""
import argparse
import ctypes as c
import os
from pathlib import Path


def api_version(version):
    major, minor, patch = map(int, version.split('.'))
    return (major << 24) | (minor << 16) | patch


def initialize_and_exercise(obs, module, clock_fixture=None):
    obs.obs_init_module.argtypes = [c.c_void_p]
    obs.obs_init_module.restype = c.c_bool
    if not obs.obs_init_module(module):
        raise RuntimeError('OBS plugin initialization failed')
    if clock_fixture:
        fixture = c.c_void_p()
        result = obs.obs_open_module(c.byref(fixture), os.fsencode(Path(clock_fixture).resolve(strict=True)), b'.')
        if result != 0 or not obs.obs_init_module(fixture):
            raise RuntimeError('OBS-clock fixture failed to initialize')
    obs.obs_enum_source_types.argtypes = [c.c_size_t, c.POINTER(c.c_char_p)]
    obs.obs_enum_source_types.restype = c.c_bool
    registered = set()
    source_id = c.c_char_p()
    index = 0
    while obs.obs_enum_source_types(index, c.byref(source_id)):
        registered.add(source_id.value)
        index += 1
    required = {b'vdoninja_source', b'vdoninja_native_source_internal', b'vdoninja_control_center'}
    if clock_fixture:
        required.add(b'vdoninja_clock_fixture')
    if not required <= registered:
        raise RuntimeError(f'Missing source registrations: {required - registered}')
    obs.obs_source_create_private.argtypes = [c.c_char_p, c.c_char_p, c.c_void_p]
    obs.obs_source_create_private.restype = c.c_void_p
    obs.obs_source_release.argtypes = [c.c_void_p]
    obs.obs_data_create.restype = c.c_void_p
    obs.obs_data_set_bool.argtypes = [c.c_void_p, c.c_char_p, c.c_bool]
    obs.obs_data_release.argtypes = [c.c_void_p]
    if clock_fixture:
        source = obs.obs_source_create_private(b'vdoninja_clock_fixture', b'Clock fixture smoke', None)
        if not source:
            raise RuntimeError('OBS-clock fixture creation failed')
        try:
            for name, expected in [('obs_source_get_width', 1920), ('obs_source_get_height', 1080)]:
                fn = getattr(obs, name)
                fn.argtypes = [c.c_void_p]
                fn.restype = c.c_uint32
                if fn(source) != expected:
                    raise RuntimeError('Wrong clock fixture dimensions')
        finally:
            obs.obs_source_release(source)
            obs.obs_wait_for_destroy_queue()
    settings = obs.obs_data_create()
    try:
        obs.obs_data_set_bool(settings, b'internal_native_receiver_source', True)
        obs.obs_data_set_bool(settings, b'use_native_receiver', True)
        for _ in range(3):
            source = obs.obs_source_create_private(b'vdoninja_native_source_internal', b'Linux runtime smoke', settings)
            if not source:
                raise RuntimeError('Native receiver source creation failed')
            obs.obs_source_release(source)
            obs.obs_wait_for_destroy_queue()
    finally:
        obs.obs_data_release(settings)


def validate(plugin, data, runtime_version, expect, initialize=False, clock_fixture=None):
    plugin = Path(plugin).resolve(strict=True)
    data = Path(data).resolve(strict=True)
    obs = c.CDLL('libobs.so.30', mode=os.RTLD_GLOBAL | os.RTLD_NOW)
    obs.obs_get_version.restype = c.c_uint32
    actual_version = obs.obs_get_version()
    if actual_version != api_version(runtime_version):
        raise RuntimeError(f'Wrong OBS runtime loaded: {actual_version:#x}, expected {runtime_version}')

    x11 = c.CDLL('libX11.so.6')
    x11.XInitThreads()
    x11.XOpenDisplay.argtypes = [c.c_char_p]
    x11.XOpenDisplay.restype = c.c_void_p
    x11.XCloseDisplay.argtypes = [c.c_void_p]
    display = x11.XOpenDisplay(None)
    if not display:
        raise RuntimeError('No X display; run this check with xvfb-run')
    # Keep Qt alive through obs_shutdown, which destroys plugin widgets.
    application = None
    if initialize:
        from PyQt6.QtWidgets import QApplication
        application = QApplication([])
    started = False
    try:
        obs.obs_set_nix_platform(1)  # OBS_NIX_PLATFORM_X11_EGL
        obs.obs_set_nix_platform_display.argtypes = [c.c_void_p]
        obs.obs_set_nix_platform_display(display)
        obs.obs_startup.argtypes = [c.c_char_p, c.c_char_p, c.c_void_p]
        obs.obs_startup.restype = c.c_bool
        started = obs.obs_startup(b'en-US', None, None)
        if not started:
            raise RuntimeError('OBS startup failed')
        obs.obs_open_module.argtypes = [c.POINTER(c.c_void_p), c.c_char_p, c.c_char_p]
        obs.obs_open_module.restype = c.c_int
        module = c.c_void_p()
        result = obs.obs_open_module(c.byref(module), os.fsencode(plugin), os.fsencode(data))
        expected = 0 if expect == 'compatible' else -4  # MODULE_SUCCESS / MODULE_INCOMPATIBLE_VER
        if result != expected:
            raise RuntimeError(f'OBS {runtime_version}: module result {result}, expected {expected}')
        if initialize:
            initialize_and_exercise(obs, module, clock_fixture)
    finally:
        if started:
            obs.obs_shutdown()
        x11.XCloseDisplay(display)
    print(f'PASS: OBS {runtime_version}: {expect} (obs_open_module={result}, initialized={initialize}, shutdown complete)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plugin')
    parser.add_argument('data')
    parser.add_argument('--runtime-version', required=True)
    parser.add_argument('--expect', choices=['compatible', 'incompatible'], required=True)
    parser.add_argument('--initialize', action='store_true', help='Initialize Qt/plugin and exercise native sources')
    parser.add_argument('--clock-fixture', help='Also load and exercise the test-only OBS-clock source')
    args = parser.parse_args()
    if args.clock_fixture and not args.initialize:
        parser.error('--clock-fixture requires --initialize')
    if args.initialize and args.expect != 'compatible':
        parser.error('--initialize requires --expect compatible')
    validate(args.plugin, args.data, args.runtime_version, args.expect, args.initialize, args.clock_fixture)
