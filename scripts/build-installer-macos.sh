#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PACKAGE_ROOT="$REPO_ROOT/install"
OUTPUT_DIR="$REPO_ROOT/artifacts"
IDENTIFIER="com.vdoninja.obs-plugin"
ARCH="$(uname -m)"
VERSION="$(sed -nE 's/^project\(obs-vdoninja VERSION ([0-9.]+).*/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -1)"
OBS_COMPAT_MAJOR="32"
APP_SIGN_IDENTITY="${OBS_VDONINJA_APP_SIGN_IDENTITY:-}"
SIGN_IDENTITY="${OBS_VDONINJA_INSTALLER_SIGN_IDENTITY:-}"
NOTARIZE=0
STAPLE=1
NOTARY_PROFILE="${OBS_VDONINJA_NOTARY_PROFILE:-}"
APPLE_ID="${OBS_VDONINJA_APPLE_ID:-${APPLE_ID:-${appleId-}}}"
NOTARY_TEAM_ID="${OBS_VDONINJA_TEAM_ID:-${APPLE_TEAM_ID:-${TEAM_ID:-${teamId-}}}}"
APPLE_ID_PASSWORD="${OBS_VDONINJA_APPLE_ID_PASSWORD:-${APPLE_ID_PASSWORD:-${appleIdPassword-}}}"
DYLIB_SEARCH_DIRS=()

usage() {
  cat <<USAGE
Usage: $0 [options]

Build a macOS .pkg installer from a staged OBS plugin package root.

The package stores a copy under /Library and the postinstall script syncs it
into the active user's OBS plugin directory. OBS 32.x Apple Silicon loads
third-party .plugin bundles from the user Application Support path.

Options:
  --package-root PATH       Staged package root containing lib/obs-plugins and share/obs/obs-plugins
  --output-dir PATH         Directory for the generated .pkg
  --identifier ID           Package identifier (default: $IDENTIFIER)
  --version VERSION         Package version (default: project version)
  --obs-major VERSION       Expected OBS major version (default: $OBS_COMPAT_MAJOR)
  --arch ARCH               Output architecture label (default: uname -m)
  --dylib-search-dir PATH   Extra directory for non-system dylibs such as libdatachannel
  --app-sign IDENTITY       Developer ID Application identity for the plugin bundle
  --sign IDENTITY           Developer ID Installer identity for pkgbuild signing
  --notarize                Submit the signed .pkg to Apple notarization and wait
  --notary-profile PROFILE  notarytool keychain profile
  --apple-id ID             Apple ID for notarytool (also reads OBS_VDONINJA_APPLE_ID/appleId)
  --team-id ID              Apple team ID for notarytool (also reads OBS_VDONINJA_TEAM_ID/teamId)
  --apple-id-password PASS  App-specific password for notarytool (prefer env/keychain profile)
  --no-staple               Do not staple after successful notarization
  -h, --help                Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --package-root)
      PACKAGE_ROOT="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --identifier)
      IDENTIFIER="$2"
      shift 2
      ;;
    --version)
      VERSION="$2"
      shift 2
      ;;
    --arch)
      ARCH="$2"
      shift 2
      ;;
    --obs-major)
      OBS_COMPAT_MAJOR="$2"
      shift 2
      ;;
    --dylib-search-dir)
      DYLIB_SEARCH_DIRS+=("$2")
      shift 2
      ;;
    --app-sign)
      APP_SIGN_IDENTITY="$2"
      shift 2
      ;;
    --sign)
      SIGN_IDENTITY="$2"
      shift 2
      ;;
    --notarize)
      NOTARIZE=1
      shift
      ;;
    --notary-profile)
      NOTARY_PROFILE="$2"
      shift 2
      ;;
    --apple-id)
      APPLE_ID="$2"
      shift 2
      ;;
    --team-id)
      NOTARY_TEAM_ID="$2"
      shift 2
      ;;
    --apple-id-password)
      APPLE_ID_PASSWORD="$2"
      shift 2
      ;;
    --no-staple)
      STAPLE=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$VERSION" ]]; then
  echo "Could not determine package version." >&2
  exit 1
fi

if ! command -v pkgbuild >/dev/null 2>&1; then
  echo "pkgbuild is required. Install Xcode command line tools." >&2
  exit 1
fi

if ! command -v otool >/dev/null 2>&1 ||
  ! command -v install_name_tool >/dev/null 2>&1 ||
  ! command -v codesign >/dev/null 2>&1; then
  echo "otool, install_name_tool, and codesign are required. Install Xcode command line tools." >&2
  exit 1
fi

if [[ "$NOTARIZE" -eq 1 ]]; then
  if ! command -v xcrun >/dev/null 2>&1; then
    echo "xcrun is required for notarization. Install Xcode command line tools." >&2
    exit 1
  fi
  if [[ -z "$APP_SIGN_IDENTITY" || -z "$SIGN_IDENTITY" ]]; then
    echo "--notarize requires both --app-sign and --sign identities." >&2
    exit 1
  fi
  if [[ -z "$NOTARY_PROFILE" && ( -z "$APPLE_ID" || -z "$NOTARY_TEAM_ID" || -z "$APPLE_ID_PASSWORD" ) ]]; then
    echo "--notarize requires --notary-profile or Apple ID/team ID/app-specific password credentials." >&2
    exit 1
  fi
fi

PACKAGE_ROOT="$(cd "$PACKAGE_ROOT" && pwd -P)"
OUTPUT_DIR="$(mkdir -p "$OUTPUT_DIR" && cd "$OUTPUT_DIR" && pwd -P)"

SRC_PLUGIN_DIR=""
if [[ -d "$PACKAGE_ROOT/lib/obs-plugins" ]]; then
  SRC_PLUGIN_DIR="$PACKAGE_ROOT/lib/obs-plugins"
elif [[ -d "$PACKAGE_ROOT/obs-plugins/64bit" ]]; then
  SRC_PLUGIN_DIR="$PACKAGE_ROOT/obs-plugins/64bit"
else
  echo "Could not find plugin binaries in package root: $PACKAGE_ROOT" >&2
  exit 1
fi

SRC_DATA_DIR=""
if [[ -d "$PACKAGE_ROOT/share/obs/obs-plugins/obs-vdoninja" ]]; then
  SRC_DATA_DIR="$PACKAGE_ROOT/share/obs/obs-plugins/obs-vdoninja"
elif [[ -d "$PACKAGE_ROOT/data/obs-plugins/obs-vdoninja" ]]; then
  SRC_DATA_DIR="$PACKAGE_ROOT/data/obs-plugins/obs-vdoninja"
else
  echo "Could not find plugin data directory in package root: $PACKAGE_ROOT" >&2
  exit 1
fi

PLUGIN_BIN=""
for candidate in obs-vdoninja.so libobs-vdoninja.so obs-vdoninja.dylib obs-vdoninja; do
  if [[ -f "$SRC_PLUGIN_DIR/$candidate" ]]; then
    PLUGIN_BIN="$SRC_PLUGIN_DIR/$candidate"
    break
  fi
done

if [[ -z "$PLUGIN_BIN" ]]; then
  echo "Could not find plugin binary in $SRC_PLUGIN_DIR" >&2
  exit 1
fi

STAGE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/obs-vdoninja-pkg-root.XXXXXX")"
SCRIPTS_DIR="$(mktemp -d "${TMPDIR:-/tmp}/obs-vdoninja-pkg-scripts.XXXXXX")"
trap 'rm -rf "$STAGE_ROOT" "$SCRIPTS_DIR"' EXIT

BUNDLE_DIR="$STAGE_ROOT/Library/Application Support/obs-studio/plugins/obs-vdoninja.plugin"
DST_PLUGIN_DIR="$BUNDLE_DIR/Contents/MacOS"
DST_DATA_DIR="$BUNDLE_DIR/Contents/Resources/data"

mkdir -p "$DST_PLUGIN_DIR" "$DST_DATA_DIR"
cp -a "$PLUGIN_BIN" "$DST_PLUGIN_DIR/obs-vdoninja"
cp -a "$SRC_DATA_DIR"/. "$DST_DATA_DIR"/
ln -s data/locale "$BUNDLE_DIR/Contents/Resources/locale"

for dylib in "$SRC_PLUGIN_DIR"/*.dylib; do
  [[ -f "$dylib" ]] && cp -a "$dylib" "$DST_PLUGIN_DIR/"
done

cat > "$BUNDLE_DIR/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>obs-vdoninja</string>
    <key>CFBundleIdentifier</key>
    <string>$IDENTIFIER</string>
    <key>CFBundleVersion</key>
    <string>$VERSION</string>
    <key>CFBundleShortVersionString</key>
    <string>$VERSION</string>
    <key>CFBundleExecutable</key>
    <string>obs-vdoninja</string>
    <key>CFBundlePackageType</key>
    <string>BNDL</string>
</dict>
</plist>
PLIST

find_dylib() {
  local name="$1"
  local search_dir

  for search_dir in "$SRC_PLUGIN_DIR" "${DYLIB_SEARCH_DIRS[@]}" /opt/homebrew/opt/openssl@3/lib /opt/homebrew/lib /usr/local/lib; do
    if [[ -f "$search_dir/$name" ]]; then
      printf '%s\n' "$search_dir/$name"
      return 0
    fi
  done

  return 1
}

bundle_dependency() {
  local dep="$1"
  local name
  local source_path

  name="$(basename "$dep")"
  if [[ -f "$DST_PLUGIN_DIR/$name" ]]; then
    return 0
  fi

  if [[ "$dep" = /* && -f "$dep" ]]; then
    source_path="$dep"
  elif source_path="$(find_dylib "$name")"; then
    :
  else
    return 1
  fi

  cp -a "$source_path" "$DST_PLUGIN_DIR/$name"
}

sign_plugin_bundle() {
  if [[ -n "$APP_SIGN_IDENTITY" ]]; then
    while IFS= read -r -d '' file; do
      codesign --force --timestamp --options runtime --sign "$APP_SIGN_IDENTITY" "$file"
    done < <(find "$DST_PLUGIN_DIR" -maxdepth 1 -type f \( -name "*.dylib" -o -name "obs-vdoninja" \) -print0)

    codesign --force --timestamp --options runtime --sign "$APP_SIGN_IDENTITY" "$BUNDLE_DIR"
    codesign --verify --deep --strict --verbose=2 "$BUNDLE_DIR"
  else
    codesign --force --deep --sign - "$BUNDLE_DIR" >/dev/null 2>&1 || true
  fi
}

notarize_package() {
  local -a notary_args

  notary_args=(notarytool submit "$OUTPUT_PKG" --wait --timeout 30m)
  if [[ -n "$NOTARY_PROFILE" ]]; then
    notary_args+=(--keychain-profile "$NOTARY_PROFILE")
  else
    notary_args+=(--apple-id "$APPLE_ID" --team-id "$NOTARY_TEAM_ID" --password "$APPLE_ID_PASSWORD")
  fi

  xcrun "${notary_args[@]}"

  if [[ "$STAPLE" -eq 1 ]]; then
    xcrun stapler staple "$OUTPUT_PKG"
    xcrun stapler validate "$OUTPUT_PKG"
  fi
}

PLUGIN="$DST_PLUGIN_DIR/obs-vdoninja"

# Rewrite Qt framework references to OBS-provided @rpath frameworks and bundle
# non-system dylibs (OpenSSL/libdatachannel) next to the plugin.
otool -L "$PLUGIN" | awk 'NR > 1 {print $1}' | while read -r dep; do
  case "$dep" in
    /usr/lib/*|/System/*) continue ;;
    @rpath/Qt*.framework/*|@rpath/libobs.framework/*|@rpath/obs-frontend-api.dylib|@rpath/libav*.dylib|@rpath/libsw*.dylib)
      continue
      ;;
    */Qt*.framework/*)
      framework_rel="$(printf '%s\n' "$dep" | sed 's|.*/\(Qt[^/]*\.framework/.*\)|\1|')"
      [[ -n "$framework_rel" ]] && install_name_tool -change "$dep" "@rpath/$framework_rel" "$PLUGIN" || true
      ;;
    *)
      if bundle_dependency "$dep"; then
        install_name_tool -change "$dep" "@loader_path/$(basename "$dep")" "$PLUGIN" || true
      fi
      ;;
  esac
done

# Fix bundled dylib install names and cross-references.
for dylib in "$DST_PLUGIN_DIR"/*.dylib; do
  [[ -f "$dylib" ]] || continue
  libbase="$(basename "$dylib")"
  install_name_tool -id "@loader_path/$libbase" "$dylib" 2>/dev/null || true
  otool -L "$dylib" | awk 'NR > 1 {print $1}' | while read -r dep; do
    case "$dep" in
      @*|/usr/lib/*|/System/*) continue ;;
    esac
    if bundle_dependency "$dep"; then
      install_name_tool -change "$dep" "@loader_path/$(basename "$dep")" "$dylib" 2>/dev/null || true
    fi
  done
done

PLUGIN_ARCHES="$(lipo -archs "$PLUGIN" 2>/dev/null || true)"
if [[ -z "$PLUGIN_ARCHES" ]]; then
  echo "Could not determine plugin architecture slices for $PLUGIN" >&2
  exit 1
fi

cat > "$BUNDLE_DIR/Contents/Resources/install-metadata.env" <<METADATA
PLUGIN_VERSION='$VERSION'
PLUGIN_ARCHES='$PLUGIN_ARCHES'
OBS_COMPAT_MAJOR='$OBS_COMPAT_MAJOR'
METADATA

xattr -dr com.apple.quarantine "$BUNDLE_DIR" 2>/dev/null || true
sign_plugin_bundle

cat > "$SCRIPTS_DIR/preinstall" <<PREINSTALL
#!/bin/sh
PLUGIN_ARCHES="$PLUGIN_ARCHES"
LOG_FILE="\${OBS_VDONINJA_INSTALL_LOG:-/Library/Logs/obs-vdoninja-installer.log}"

log() {
  mkdir -p "\$(dirname "\$LOG_FILE")" 2>/dev/null || true
  printf '%s %s\\n' "\$(date '+%Y-%m-%d %H:%M:%S')" "\$*" >> "\$LOG_FILE" 2>/dev/null || true
  echo "\$*"
}

HOST_ARCH="\${OBS_VDONINJA_HOST_ARCH:-\$(uname -m)}"
ARCH_SUPPORTED=0
for arch in \$PLUGIN_ARCHES; do
  if [ "\$arch" = "\$HOST_ARCH" ]; then
    ARCH_SUPPORTED=1
  elif [ "\$arch" = "arm64" ] && [ "\$HOST_ARCH" = "arm64e" ]; then
    ARCH_SUPPORTED=1
  elif [ "\$arch" = "arm64e" ] && [ "\$HOST_ARCH" = "arm64" ]; then
    ARCH_SUPPORTED=1
  fi
done

if [ "\$ARCH_SUPPORTED" -ne 1 ]; then
  log "ERROR: this package contains plugin architecture(s): \$PLUGIN_ARCHES, but this Mac reports: \$HOST_ARCH."
  log "Install the matching OBS VDO.Ninja package for this Mac/OBS architecture."
  exit 1
fi

exit 0
PREINSTALL
chmod +x "$SCRIPTS_DIR/preinstall"

cat > "$SCRIPTS_DIR/postinstall" <<'POSTINSTALL'
#!/bin/sh
SYSTEM_PLUGIN_DIR="${OBS_VDONINJA_SYSTEM_PLUGIN_DIR:-/Library/Application Support/obs-studio/plugins/obs-vdoninja.plugin}"
LOG_FILE="${OBS_VDONINJA_INSTALL_LOG:-/Library/Logs/obs-vdoninja-installer.log}"

log() {
  mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null || true
  printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG_FILE" 2>/dev/null || true
  echo "$*"
}

read_info_plist() {
  /usr/libexec/PlistBuddy -c "Print :$2" "$1/Contents/Info.plist" 2>/dev/null || true
}

bundle_arches() {
  local binary="$1/Contents/MacOS/OBS"
  [ -f "$binary" ] || binary="$1/Contents/MacOS/obs"
  [ -f "$binary" ] || return 0
  lipo -archs "$binary" 2>/dev/null || true
}

arch_overlap() {
  local left="$1"
  local right="$2"
  local arch
  for arch in $left; do
    case " $right " in
      *" $arch "*) return 0 ;;
    esac
    if [ "$arch" = "arm64" ]; then
      case " $right " in *" arm64e "*) return 0 ;; esac
    elif [ "$arch" = "arm64e" ]; then
      case " $right " in *" arm64 "*) return 0 ;; esac
    fi
  done
  return 1
}

find_obs_apps() {
  local user_home="$1"
  local dir

  for dir in /Applications "$user_home/Applications"; do
    [ -d "$dir" ] || continue
    find "$dir" -maxdepth 3 -name "OBS.app" -type d -print 2>/dev/null
  done

  if command -v mdfind >/dev/null 2>&1; then
    mdfind "kMDItemCFBundleIdentifier == 'com.obsproject.obs-studio'" 2>/dev/null | while read -r app; do
      [ -d "$app" ] && printf '%s\n' "$app"
    done
  fi
}

log "Installing OBS VDO.Ninja plugin."

METADATA="$SYSTEM_PLUGIN_DIR/Contents/Resources/install-metadata.env"
if [ -f "$METADATA" ]; then
  # shellcheck disable=SC1090
  . "$METADATA"
fi

PLUGIN_VERSION="${PLUGIN_VERSION:-unknown}"
PLUGIN_ARCHES="${PLUGIN_ARCHES:-$(lipo -archs "$SYSTEM_PLUGIN_DIR/Contents/MacOS/obs-vdoninja" 2>/dev/null || true)}"
OBS_COMPAT_MAJOR="${OBS_COMPAT_MAJOR:-32}"

if [ -z "$PLUGIN_ARCHES" ]; then
  log "WARNING: could not determine installed plugin architecture slices."
else
  log "Plugin version: $PLUGIN_VERSION; architecture slice(s): $PLUGIN_ARCHES."
fi

verify_bundle_signature() {
  if codesign --verify --deep --strict "$1" >/dev/null 2>&1; then
    log "Code signature verified: $1"
  else
    log "WARNING: code signature verification failed for $1"
  fi
}

xattr -dr com.apple.quarantine "$SYSTEM_PLUGIN_DIR" 2>/dev/null || true
verify_bundle_signature "$SYSTEM_PLUGIN_DIR"

CONSOLE_USER="${OBS_VDONINJA_INSTALL_USER:-$(stat -f %Su /dev/console 2>/dev/null || true)}"
if [ -z "$CONSOLE_USER" ] || [ "$CONSOLE_USER" = "root" ] || [ "$CONSOLE_USER" = "_mbsetupuser" ]; then
  CONSOLE_USER="${SUDO_USER:-}"
fi

if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != "root" ] && id "$CONSOLE_USER" >/dev/null 2>&1; then
  USER_HOME="${OBS_VDONINJA_USER_HOME:-$(dscl . -read "/Users/$CONSOLE_USER" NFSHomeDirectory 2>/dev/null | awk '{print $2}')}"
  if [ -z "$USER_HOME" ]; then
    USER_HOME="/Users/$CONSOLE_USER"
  fi

  USER_PLUGIN_DIR="${OBS_VDONINJA_USER_PLUGIN_DIR:-$USER_HOME/Library/Application Support/obs-studio/plugins/obs-vdoninja.plugin}"
  mkdir -p "$(dirname "$USER_PLUGIN_DIR")"
  rm -rf "$USER_PLUGIN_DIR"
  ditto "$SYSTEM_PLUGIN_DIR" "$USER_PLUGIN_DIR"

  USER_GROUP="$(id -gn "$CONSOLE_USER" 2>/dev/null || echo staff)"
  chown -R "$CONSOLE_USER:$USER_GROUP" "$USER_PLUGIN_DIR" 2>/dev/null || true
  xattr -dr com.apple.quarantine "$USER_PLUGIN_DIR" 2>/dev/null || true
  verify_bundle_signature "$USER_PLUGIN_DIR"

  log "Installed user OBS plugin bundle: $USER_PLUGIN_DIR"

  OBS_APPS="$(find_obs_apps "$USER_HOME" | sort -u)"
  if [ -n "$OBS_APPS" ]; then
    printf '%s\n' "$OBS_APPS" | while read -r obs_app; do
      [ -n "$obs_app" ] || continue
      obs_version="$(read_info_plist "$obs_app" CFBundleShortVersionString)"
      obs_arches="$(bundle_arches "$obs_app")"

      log "Detected OBS app: $obs_app; version: ${obs_version:-unknown}; architecture slice(s): ${obs_arches:-unknown}."

      case "$obs_version" in
        "$OBS_COMPAT_MAJOR".*) ;;
        "")
          log "WARNING: could not determine OBS version for $obs_app. This plugin package targets OBS $OBS_COMPAT_MAJOR.x."
          ;;
        *)
          log "WARNING: $obs_app is OBS $obs_version. This plugin package targets OBS $OBS_COMPAT_MAJOR.x."
          ;;
      esac

      if [ -n "$obs_arches" ] && [ -n "$PLUGIN_ARCHES" ] && ! arch_overlap "$obs_arches" "$PLUGIN_ARCHES"; then
        log "WARNING: $obs_app architecture slice(s) [$obs_arches] do not overlap plugin slice(s) [$PLUGIN_ARCHES]. OBS will not load this package."
      fi
    done
  else
    log "WARNING: OBS.app was not found in /Applications or $USER_HOME/Applications. The plugin was still installed to OBS's user plugin directory."
  fi
else
  log "WARNING: could not determine the active console user; installed only the system copy at $SYSTEM_PLUGIN_DIR."
fi

exit 0
POSTINSTALL
chmod +x "$SCRIPTS_DIR/postinstall"

OUTPUT_PKG="$OUTPUT_DIR/obs-vdoninja-macos-$ARCH.pkg"
PKGBUILD_ARGS=(
  --root "$STAGE_ROOT"
  --scripts "$SCRIPTS_DIR"
  --identifier "$IDENTIFIER"
  --version "$VERSION"
  --install-location "/"
)

if [[ -n "$SIGN_IDENTITY" ]]; then
  PKGBUILD_ARGS+=(--sign "$SIGN_IDENTITY")
fi

pkgbuild "${PKGBUILD_ARGS[@]}" "$OUTPUT_PKG"

if [[ "$NOTARIZE" -eq 1 ]]; then
  notarize_package
fi

echo "Installer created: $OUTPUT_PKG"
