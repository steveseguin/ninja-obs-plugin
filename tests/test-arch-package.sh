#!/usr/bin/env bash
# Run in an isolated Arch container; installs/removes the package with pacman.
set -euo pipefail
if [[ $EUID -ne 0 || ! -f /.dockerenv ]]; then
  echo 'Run this test as root in an isolated Arch Docker container.' >&2
  exit 1
fi
repo="$(cd "$(dirname "$0")/.." && pwd)"
pacman -Syu --noconfirm --needed base-devel cmake ninja git obs-studio libdatachannel \
  openssl qt6-base ffmpeg namcap xorg-server-xvfb xorg-xauth python-pyqt6
work="$(mktemp -d /tmp/vdoninja-arch.XXXXXX)"
id -u package-test >/dev/null 2>&1 || useradd -m package-test
chmod 755 "$work"
git -c safe.directory="$repo" -c safe.directory="$repo/.git" clone --no-hardlinks "$repo" "$work/source"
cp "$repo/packaging/arch/PKGBUILD" "$work/PKGBUILD"
# Build the checked-out CI revision, including PR changes, rather than remote main.
sed -i "s|^source=.*|source=('ninja-obs-plugin::git+file://$work/source')|" "$work/PKGBUILD"
chown -R package-test:package-test "$work"
runuser -u package-test -- bash -c 'cd "$1"; CMAKE_BUILD_PARALLEL_LEVEL=3 makepkg --noconfirm' bash "$work"
mapfile -t packages < <(find "$work" -maxdepth 1 -name 'obs-vdoninja-git-[0-9]*-x86_64.pkg.tar.zst')
test "${#packages[@]}" -eq 1
package="${packages[0]}"
namcap "$work/PKGBUILD" "$package" | tee "$work/namcap.log"
if grep -q " E: " "$work/namcap.log"; then exit 1; fi
pacman -U --noconfirm "$package"
# Reinstall exercises upgrade/overwrite ownership as well as first installation.
pacman -U --noconfirm "$package"
pacman -Qkk obs-vdoninja-git
plugin=/usr/lib/obs-plugins/obs-vdoninja.so
data=/usr/share/obs/obs-plugins/obs-vdoninja
ldd "$plugin" > "$work/dependencies.txt"
if grep -q 'not found' "$work/dependencies.txt"; then cat "$work/dependencies.txt"; exit 1; fi
version="$(pkg-config --modversion libobs)"
timeout -k 5s 60s xvfb-run -a --server-args='-screen 0 1280x720x24 -extension GLX' \
  python "$repo/scripts/validate-linux-obs-runtime.py" "$plugin" "$data" \
  --runtime-version "$version" --expect compatible --initialize
pacman -R --noconfirm obs-vdoninja-git
test ! -e "$plugin"
test ! -e "$data"
if [[ -n "${ARCH_ARTIFACT_DIR:-}" ]]; then
  mkdir -p "$ARCH_ARTIFACT_DIR"
  cp "$package" "$work/namcap.log" "$work/dependencies.txt" "$ARCH_ARTIFACT_DIR/"
fi
echo 'PASS: Arch source build, linked media tests, install/reinstall, OBS initialization, and pacman removal'
