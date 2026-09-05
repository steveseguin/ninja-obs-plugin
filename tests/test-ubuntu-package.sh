#!/usr/bin/env bash
# Usage in an isolated Ubuntu container: bash test-ubuntu-package.sh PACKAGE.tar.gz OBS.deb
set -euo pipefail
if [[ $EUID -ne 0 || ! -f /.dockerenv ]]; then
  echo 'Run this test as root in an isolated Ubuntu Docker container.' >&2
  exit 1
fi
archive="$(readlink -f "$1")"
obs_deb="$(readlink -f "$2")"
repo="$(cd "$(dirname "$0")/.." && pwd)"
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y "$obs_deb" python3-pyqt6 xvfb xauth
version="$(obs --version | sed -E 's/.* ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
package_dir="$(mktemp -d)"
chmod 755 "$package_dir"
tar -xf "$archive" -C "$package_dir"
validate() {
  local launcher=()
  if [[ -n "${3:-}" ]]; then launcher=(runuser -u "$3" --); fi
  "${launcher[@]}" timeout -k 5s 60s xvfb-run -a --server-args='-screen 0 1280x720x24 -extension GLX' \
    /usr/bin/python3 "$repo/scripts/validate-linux-obs-runtime.py" "$1" "$2" \
    --runtime-version "$version" --expect compatible --initialize
}
# Official Ubuntu OBS packages install below /usr/local.
plugin=/usr/local/lib/x86_64-linux-gnu/obs-plugins/obs-vdoninja.so
data=/usr/local/share/obs/obs-plugins/obs-vdoninja
bash "$package_dir/install.sh"
bash "$package_dir/install.sh"
validate "$plugin" "$data"
bash "$package_dir/uninstall.sh"
test ! -e "$plugin"
test ! -e "$(dirname "$plugin")/obs-vdoninja"
test -d "$data"
bash "$package_dir/uninstall.sh" --remove-data
test ! -e "$data"
# Custom XDG path, including spaces, must match OBS's per-user search directory.
useradd -m package-test
config='/home/package-test/custom config'
runuser -u package-test -- env XDG_CONFIG_HOME="$config" bash "$package_dir/install.sh"
user_plugin="$config/obs-studio/plugins/obs-vdoninja/bin/64bit/obs-vdoninja.so"
user_data="$config/obs-studio/plugins/obs-vdoninja/data"
validate "$user_plugin" "$user_data" package-test
runuser -u package-test -- env XDG_CONFIG_HOME="$config" bash "$package_dir/uninstall.sh" --remove-data
test ! -e "$user_plugin"
test ! -e "$(dirname "$user_plugin")/obs-vdoninja"
test ! -e "$user_data"
echo 'PASS: official Ubuntu OBS installation, plugin install/reinstall/load/remove, and custom-XDG user install/remove'
