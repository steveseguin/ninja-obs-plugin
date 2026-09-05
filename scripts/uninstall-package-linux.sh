#!/usr/bin/env bash
set -euo pipefail

REMOVE_DATA=0
if [[ "${1:-}" == "--remove-data" ]]; then
  REMOVE_DATA=1
fi

if [[ "${EUID}" -eq 0 ]]; then
  # Remove every copy left by current or older system-wide installers.
  DST_PLUGIN_DIRS=(/usr/lib/obs-plugins /usr/lib64/obs-plugins /usr/lib/*/obs-plugins /usr/lib64/*/obs-plugins
                  /usr/local/lib/obs-plugins /usr/local/lib64/obs-plugins
                  /usr/local/lib/*/obs-plugins /usr/local/lib64/*/obs-plugins)
  DST_DATA_DIRS=(/usr/share/obs/obs-plugins/obs-vdoninja /usr/local/share/obs/obs-plugins/obs-vdoninja)
else
  DST_PLUGIN_DIRS=("${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/obs-vdoninja/bin/64bit")
  DST_DATA_DIRS=("${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/obs-vdoninja/data")
fi

echo "Uninstalling OBS VDO.Ninja plugin..."
printf 'Plugin dir: %s\n' "${DST_PLUGIN_DIRS[@]}"
printf 'Data dir: %s\n' "${DST_DATA_DIRS[@]}"

for DST_PLUGIN_DIR in "${DST_PLUGIN_DIRS[@]}"; do
  if [[ -f "$DST_PLUGIN_DIR/obs-vdoninja.so" ]]; then
    rm -f "$DST_PLUGIN_DIR/obs-vdoninja.so"
    echo "Removed: $DST_PLUGIN_DIR/obs-vdoninja.so"
  fi
  if [[ -f "$DST_PLUGIN_DIR/libobs-vdoninja.so" ]]; then
    rm -f "$DST_PLUGIN_DIR/libobs-vdoninja.so"
    echo "Removed: $DST_PLUGIN_DIR/libobs-vdoninja.so"
  fi

  if [[ -d "$DST_PLUGIN_DIR/obs-vdoninja" ]]; then
    rm -rf "$DST_PLUGIN_DIR/obs-vdoninja"
    echo "Removed private runtime: $DST_PLUGIN_DIR/obs-vdoninja"
  fi
done

if [[ "$REMOVE_DATA" -eq 1 ]]; then
  for DST_DATA_DIR in "${DST_DATA_DIRS[@]}"; do
    if [[ -d "$DST_DATA_DIR" ]]; then
      rm -rf "$DST_DATA_DIR"
      echo "Removed data: $DST_DATA_DIR"
    fi
  done
fi

echo
echo "Uninstall complete. Restart OBS Studio."
