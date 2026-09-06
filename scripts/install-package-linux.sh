#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Locate the plugin binaries inside an extracted package. The layout depends on
# how CMAKE_INSTALL_LIBDIR expanded at build time: plain "lib" on most distros,
# "lib64" on RPM-based ones, and "lib/<arch-triplet>" on Debian/Ubuntu multiarch
# (e.g. lib/x86_64-linux-gnu/obs-plugins). "obs-plugins/64bit" is the portable
# layout. Unmatched globs simply fail the -d test, so no nullglob is needed.
find_plugin_dir() {
  local root="$1"
  local dir
  for dir in "$root"/lib/obs-plugins "$root"/lib64/obs-plugins "$root"/obs-plugins/64bit \
             "$root"/lib/*/obs-plugins "$root"/lib64/*/obs-plugins; do
    if [[ -d "$dir" ]]; then
      printf '%s\n' "$dir"
      return 0
    fi
  done
  return 1
}

PKG_ROOT="$SCRIPT_DIR"
SRC_PLUGIN_DIR="$(find_plugin_dir "$PKG_ROOT" || true)"
if [[ -z "$SRC_PLUGIN_DIR" ]]; then
  PARENT_DIR="$(dirname "$SCRIPT_DIR")"
  SRC_PLUGIN_DIR="$(find_plugin_dir "$PARENT_DIR" || true)"
  if [[ -n "$SRC_PLUGIN_DIR" ]]; then
    PKG_ROOT="$PARENT_DIR"
  fi
fi

if [[ -z "$SRC_PLUGIN_DIR" ]]; then
  echo "Could not find plugin binaries in package." >&2
  echo "Searched under $PKG_ROOT for: lib/obs-plugins, lib64/obs-plugins," >&2
  echo "obs-plugins/64bit, lib/<arch>/obs-plugins, lib64/<arch>/obs-plugins" >&2
  exit 1
fi

SRC_DATA_DIR=""
if [[ -d "$PKG_ROOT/share/obs/obs-plugins/obs-vdoninja" ]]; then
  SRC_DATA_DIR="$PKG_ROOT/share/obs/obs-plugins/obs-vdoninja"
elif [[ -d "$PKG_ROOT/data/obs-plugins/obs-vdoninja" ]]; then
  SRC_DATA_DIR="$PKG_ROOT/data/obs-plugins/obs-vdoninja"
else
  echo "Could not find plugin data directory in package."
  exit 1
fi

if ! command -v obs >/dev/null 2>&1; then
  echo "Native OBS Studio 32.2.x is required. This installer does not install into Snap or Flatpak." >&2
  exit 1
fi
OBS_PATH="$(readlink -f "$(command -v obs)")"
case "$OBS_PATH" in
  /snap/*|*/snap)
    echo "This package is for native OBS Studio, not Snap. See INSTALL.md." >&2
    exit 1
    ;;
esac
OBS_VERSION_TEXT="$(obs --version 2>&1)" || {
  echo "Could not determine the native OBS version: $OBS_VERSION_TEXT" >&2
  exit 1
}
if [[ ! "$OBS_VERSION_TEXT" =~ OBS[[:space:]]+Studio[[:space:]]+(-[[:space:]]+)?32\.2\.[0-9]+([^0-9]|$) ]]; then
  echo "This package requires OBS Studio 32.2.x; detected: $OBS_VERSION_TEXT" >&2
  exit 1
fi

# Resolve dependencies from the extracted package before copying anything.
# The release binary's relative RUNPATH finds its private libdatachannel here.
PLUGIN_FILE="$SRC_PLUGIN_DIR/obs-vdoninja.so"
if [[ ! -f "$PLUGIN_FILE" ]]; then
  echo "Package is missing obs-vdoninja.so." >&2
  exit 1
fi
if ! DEPENDENCIES="$(ldd "$PLUGIN_FILE" 2>&1)" || [[ "$DEPENDENCIES" == *"not found"* ]]; then
  echo "Plugin runtime dependencies could not be resolved; nothing was installed:" >&2
  echo "$DEPENDENCIES" >&2
  exit 1
fi

if [[ "${EUID}" -eq 0 ]]; then
  # Official OBS Ubuntu packages use /usr/local; distro packages use /usr.
  # Follow the libobs resolved by this executable when both installs exist.
  DST_PLUGIN_DIR=""
  DST_DATA_DIR="/usr/share/obs/obs-plugins/obs-vdoninja"
  OBS_DEPENDENCIES="$(LC_ALL=C ldd "$OBS_PATH" 2>/dev/null || true)"
  OBS_LIB_PATTERN='libobs\.so[^[:space:]]*[[:space:]]+=>[[:space:]]+(/[^[:space:]]+)'
  if [[ "$OBS_DEPENDENCIES" =~ $OBS_LIB_PATTERN ]]; then
    OBS_LIB_DIR="$(dirname "$(readlink -f "${BASH_REMATCH[1]}")")"
    case "$OBS_LIB_DIR" in
      /usr/local/lib|/usr/local/lib64|/usr/local/lib/*|/usr/local/lib64/*)
        DST_PLUGIN_DIR="$OBS_LIB_DIR/obs-plugins"
        DST_DATA_DIR="/usr/local/share/obs/obs-plugins/obs-vdoninja"
        ;;
      /usr/lib|/usr/lib64|/usr/lib/*|/usr/lib64/*)
        DST_PLUGIN_DIR="$OBS_LIB_DIR/obs-plugins"
        ;;
    esac
  fi
  if [[ -z "$DST_PLUGIN_DIR" ]]; then
    # Fallback for launch wrappers. Prefer multiarch over directories left by
    # older installers, and retain the original /usr/lib fallback.
    for prefix in /usr/local /usr; do
      for dir in "$prefix"/lib/*/obs-plugins "$prefix"/lib64/*/obs-plugins \
                 "$prefix"/lib64/obs-plugins "$prefix"/lib/obs-plugins; do
        if [[ -d "$dir" ]]; then
          DST_PLUGIN_DIR="$dir"
          DST_DATA_DIR="$prefix/share/obs/obs-plugins/obs-vdoninja"
          break 2
        fi
      done
    done
    DST_PLUGIN_DIR="${DST_PLUGIN_DIR:-/usr/lib/obs-plugins}"
  fi
else
  DST_PLUGIN_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/obs-vdoninja/bin/64bit"
  DST_DATA_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/obs-vdoninja/data"
fi

echo "Installing OBS VDO.Ninja plugin from package..."
echo "Source:      $PKG_ROOT"
echo "Plugin dst:  $DST_PLUGIN_DIR"
echo "Data dst:    $DST_DATA_DIR"

mkdir -p "$DST_PLUGIN_DIR" "$DST_DATA_DIR"
cp -a "$SRC_PLUGIN_DIR"/. "$DST_PLUGIN_DIR"/
cp -a "$SRC_DATA_DIR"/. "$DST_DATA_DIR"/

QUICKSTART_PATH="$PKG_ROOT/QUICKSTART.md"
echo
echo "Install complete."
echo
echo "Next steps:"
echo "1. Restart OBS Studio"
echo "2. Open Settings -> Stream and select VDO.Ninja"
echo "3. Set Stream ID (and optional password/room)"
echo "4. Start streaming and open your view URL"
if [[ -f "$QUICKSTART_PATH" ]]; then
  echo
  echo "Quick guide: $QUICKSTART_PATH"
  if [[ -t 0 && -t 1 ]]; then
    read -r -p "Open QUICKSTART.md now? [Y/n] " RESP
    if [[ ! "$RESP" =~ ^[Nn]$ ]] && command -v xdg-open >/dev/null 2>&1; then
      xdg-open "$QUICKSTART_PATH" >/dev/null 2>&1 || true
    fi
  fi
fi
