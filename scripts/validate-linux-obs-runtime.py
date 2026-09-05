#!/usr/bin/env python3
"""Exercise OBS's real module acceptance check under Xvfb (without starting media/UI)."""
import argparse
import ctypes as c
import os
from pathlib import Path


def api_version(version):
    major, minor, patch = map(int, version.split('.'))
    return (major << 24) | (minor << 16) | patch


def validate(plugin, data, runtime_version, expect):
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
        print(f'PASS: OBS {runtime_version}: {expect} (obs_open_module={result})')
    finally:
        if started:
            obs.obs_shutdown()
        x11.XCloseDisplay(display)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plugin')
    parser.add_argument('data')
    parser.add_argument('--runtime-version', required=True)
    parser.add_argument('--expect', choices=['compatible', 'incompatible'], required=True)
    args = parser.parse_args()
    validate(args.plugin, args.data, args.runtime_version, args.expect)
