# OBS compatibility and release validation

Reviewed on 2026-09-05. Latest stable OBS: [32.2.2](https://github.com/obsproject/obs-studio/releases/tag/32.2.2).

## How upgrades are handled

OBS checks the API version embedded by the OBS SDK used to build a plugin. The
plugin's own version number does not determine OBS compatibility. Since OBS 32,
the loader rejects a plugin built for a newer major/minor OBS version; it ignores
the patch portion of that check. See the
[OBS module loader](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/obs-module.c).

Keep the release SDK at **32.2.0**, and test the resulting binary on later 32.2
patch releases. Do not raise the SDK baseline to 32.2.2 merely because it is newer.
Passing the API check is necessary but not sufficient: shared libraries, exported
symbols, CPU architecture, and platform runtime requirements must also match.

| Plugin package | Supported OBS band | Build SDK |
| --- | --- | --- |
| Linux native x86-64, macOS Apple silicon, primary Windows x64 | 32.2.x | 32.2.0 |
| Windows x64 legacy (`obs32.0-32.1` filenames) | 32.0.x–32.1.x | 32.0.4 |

The Windows split is necessary because OBS 32.2 moved from FFmpeg 7 to FFmpeg 8,
changing the DLLs imported by the plugin. Linux and macOS do not currently have
legacy packages. Stock Ubuntu 24.04 OBS 30.0.2 is outside the supported range.
Snap and Flatpak require their own runtime/package integration; the native Linux
archive is not a Snap or Flatpak plugin package.

OBS 32.2.2 includes a Windows fix for plugins not loading on the first start after
an OBS upgrade. OBS 32.2 also changes macOS support and may migrate Intel OBS on
Apple silicon to native OBS, which requires matching Apple silicon plugins. See
[upstream release notes](https://github.com/obsproject/obs-studio/releases/tag/32.2.2).

## Validation performed

The **same plugin built with OBS 32.2.0** was tested against libobs and the frontend
API extracted from the official Ubuntu 24.04 packages:

| Runtime | `obs_open_module` result | Expected outcome |
| --- | --- | --- |
| 32.2.0 | `0` | Accepted |
| 32.2.1 | `0` | Accepted |
| 32.2.2 | `0` | Accepted |
| 32.1.2 | `-4` | Rejected: newer plugin API |
| 32.0.4 | `-4` | Rejected: newer plugin API |

The runtime check starts a real libobs context under Xvfb and calls OBS's module
loader. It does not initialize the plugin UI, start streaming, or validate live
WebRTC. Windows and macOS runtime behavior must still be tested on those systems.

The extracted Linux archive also passed immediate ELF symbol resolution, private
libdatachannel lookup, and embedded SDK-version checks. Install/uninstall tests
exercise real ELF fixtures inside a disposable chroot, including both `/usr` and
`/usr/local`, per-user installs, duplicate copies, missing dependencies, and
incompatible OBS versions. The normal and ASan/UBSan unit suites passed all 707
tests with the single-byte test locale available.

## Repeat the checks

```bash
sudo python3 tests/test-linux-package.py

# Supply only the OBS runtime library directory, not the libdatachannel build directory.
LD_LIBRARY_PATH=/path/to/obs/runtime/lib \
  python3 scripts/validate-linux-package.py /path/to/obs-vdoninja.so

LD_LIBRARY_PATH=/path/to/obs/runtime/lib \
  xvfb-run -a --server-args='-screen 0 1280x720x24 -extension GLX' \
  python3 scripts/validate-linux-obs-runtime.py \
  /path/to/obs-vdoninja.so /path/to/plugin/data \
  --runtime-version 32.2.2 --expect compatible
```

`.github/workflows/linux.yml` runs the release build, extracted-archive checks,
installer regressions, and the official-runtime matrix on main and pull requests.
The tag workflow reuses that exact workflow before publishing artifacts.

For each new OBS release, review upstream release notes and dependencies, add its
runtime to the compatibility checks, and test the existing plugin binary first.
For a new minor/major version, do not widen the advertised range until runtime
and live OBS tests pass. Only introduce another binary when changed dependencies
or required newer APIs make it necessary. Keep `build.yml`, `linux.yml`, Windows
matrix settings, installation guidance, and release notes aligned.

## Release entrypoint

Use `scripts/release.ps1`. A local rehearsal in an isolated clone with no remote
successfully ran `-Action cut -Bump patch`, including formatting, all unit tests,
version synchronization, changelog promotion, release commit, and annotated tag.
That rehearsal did not publish anything or change the real repository's version.

For an actual release, after reviewing green CI and platform validation:

```powershell
./scripts/release.ps1 -Action cut -Bump patch -Push
```

Tag CI checks metadata before packaging. The release publication job runs only
for `v*` tags. A manual run of `build.yml` on main builds and validates artifacts
without creating a release. Policy tests also enforce that the Windows linked
runtime checks remain required and that malformed workflow wiring is rejected.
