#!/usr/bin/env python3
"""Validate a staged/extracted Linux release, including immediate ELF symbol resolution."""
import argparse
import ctypes
import os
from pathlib import Path
import re
import subprocess


def validate(plugin, obs_version):
    plugin = Path(plugin).resolve(strict=True)
    dynamic = subprocess.check_output(['readelf', '-d', str(plugin)], text=True)
    needed = re.findall(r'\(NEEDED\).*\[(libdatachannel[^]]+)\]', dynamic)
    if len(needed) != 1:
        raise RuntimeError('Expected exactly one shared libdatachannel dependency')
    runtime = plugin.parent / 'obs-vdoninja' / needed[0]
    if not runtime.is_file() or runtime.resolve().parent != runtime.parent:
        raise RuntimeError(f'Missing or external private runtime: {runtime}')
    paths = re.findall(r'\((?:RUNPATH|RPATH)\).*\[([^]]*)\]', dynamic)
    if paths != ['$ORIGIN/obs-vdoninja']:
        raise RuntimeError(f'Unexpected runtime search paths: {paths}')
    dependencies = subprocess.check_output(['ldd', str(plugin)], text=True)
    if 'not found' in dependencies:
        raise RuntimeError(dependencies)
    resolved = re.search(re.escape(needed[0]) + r'\s+=>\s+(\S+)', dependencies)
    if not resolved or Path(resolved[1]).resolve() != runtime.resolve():
        raise RuntimeError('libdatachannel resolved outside the package:\n' + dependencies)
    # RTLD_NOW catches missing symbols too; ldd alone only checks library names.
    module = ctypes.CDLL(str(plugin), mode=os.RTLD_NOW | os.RTLD_LOCAL)
    module.obs_module_ver.restype = ctypes.c_uint32
    major, minor, patch = map(int, obs_version.split('.'))
    expected = (major << 24) | (minor << 16) | patch
    actual = module.obs_module_ver()
    if actual != expected:
        raise RuntimeError(f'OBS API version mismatch: {actual:#x} != {expected:#x}')
    print(f'PASS: {plugin.name}: private runtime, resolved symbols, OBS {obs_version}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plugin')
    parser.add_argument('--obs-version', default='32.2.0')
    args = parser.parse_args()
    validate(args.plugin, args.obs_version)
