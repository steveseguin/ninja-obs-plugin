#!/usr/bin/env python3
"""Check a packaged plugin with an official OBS runtime's real module loader.

This checks dependencies and SDK compatibility, not GUI initialization or media.
Run in a fresh process per runtime. On macOS, supply DYLD_FRAMEWORK_PATH and
DYLD_LIBRARY_PATH pointing to the tested OBS app's Frameworks directory.
"""
import argparse
import ctypes as c
import os
from pathlib import Path
import sys


def version_number(version):
    major, minor, patch = map(int, version.split('.'))
    return (major << 24) | (minor << 16) | patch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plugin', type=Path)
    parser.add_argument('data', type=Path)
    parser.add_argument('--obs-library', type=Path, required=True)
    parser.add_argument('--runtime-version', required=True)
    parser.add_argument('--build-version', required=True)
    args = parser.parse_args()
    plugin = args.plugin.resolve(strict=True)
    data = args.data.resolve(strict=True)
    library = args.obs_library.resolve(strict=True)
    display = None
    if sys.platform.startswith('linux'):
        x11 = c.CDLL('libX11.so.6')
        x11.XInitThreads()
        x11.XOpenDisplay.restype = c.c_void_p
        display = x11.XOpenDisplay(None)
        assert display, 'Run the Linux probe under Xvfb'
    dll_dirs = []
    if os.name == 'nt':
        dll_dirs = [os.add_dll_directory(str(p)) for p in (library.parent, plugin.parent)]
    obs = c.CDLL(str(library), mode=getattr(os, 'RTLD_GLOBAL', 0))
    if display:
        obs.obs_set_nix_platform(1)
        obs.obs_set_nix_platform_display.argtypes = [c.c_void_p]
        obs.obs_set_nix_platform_display(display)
    obs.obs_get_version.restype = c.c_uint32
    actual = obs.obs_get_version()
    assert actual == version_number(args.runtime_version), f'Wrong OBS runtime: {actual:#x}'
    module_library = c.CDLL(str(plugin))
    module_library.obs_module_ver.restype = c.c_uint32
    built = module_library.obs_module_ver()
    assert built == version_number(args.build_version), f'Wrong plugin SDK: {built:#x}'
    obs.obs_startup.argtypes = [c.c_char_p, c.c_char_p, c.c_void_p]
    obs.obs_startup.restype = c.c_bool
    assert obs.obs_startup(b'en-US', None, None), 'OBS startup failed'
    try:
        obs.obs_open_module.argtypes = [c.POINTER(c.c_void_p), c.c_char_p, c.c_char_p]
        obs.obs_open_module.restype = c.c_int
        module = c.c_void_p()
        result = obs.obs_open_module(c.byref(module), os.fsencode(plugin), os.fsencode(data))
        assert result == 0 and module.value, f'OBS module loader rejected plugin: {result}'
        print(f'PASS: OBS {args.runtime_version} loads plugin built with SDK {args.build_version}: {plugin}')
    finally:
        obs.obs_shutdown()
        if display:
            x11.XCloseDisplay.argtypes = [c.c_void_p]
            x11.XCloseDisplay(display)
        for directory in dll_dirs:
            directory.close()


if __name__ == '__main__':
    main()
