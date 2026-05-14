#!/usr/bin/env bash
set -euo pipefail

PKG_PATH=""
OBS_APP=""
CHECK_INSTALLED=0
LAUNCH_TEST=0
LAUNCH_SECONDS=12
EXPECTED_OBS_MAJOR=32
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
EXPECTED_PLUGIN_VERSION=""
if [[ -f "$REPO_ROOT/CMakeLists.txt" ]]; then
  EXPECTED_PLUGIN_VERSION="$(sed -nE 's/^project\(obs-vdoninja VERSION ([0-9.]+).*/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -1)"
elif [[ -f "$SCRIPT_DIR/VERSION" ]]; then
  EXPECTED_PLUGIN_VERSION="$(tr -d '[:space:]' < "$SCRIPT_DIR/VERSION")"
fi
TMP_DIR=""
FAILURES=0
WARNINGS=0
REQUIRE_SIGNED=0
REQUIRE_NOTARIZED=0

usage() {
  cat <<USAGE
Usage: $0 [options]

Validate a macOS OBS VDO.Ninja installer package, installed bundle, and
optionally a real OBS launch.

Options:
  --pkg PATH             Validate a generated .pkg payload
  --installed            Validate installed user/system plugin bundles
  --obs-app PATH         OBS.app to inspect or launch
  --launch-test          Launch OBS briefly and require obs-vdoninja to load
  --launch-seconds N     Seconds to keep OBS running for launch test (default: $LAUNCH_SECONDS)
  --obs-major N          Expected OBS major version (default: $EXPECTED_OBS_MAJOR)
  --require-signed       Fail unless the package and plugin bundle use Developer ID signatures
  --require-notarized    Fail unless the package has a valid notarization staple
  -h, --help             Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pkg)
      PKG_PATH="$2"
      shift 2
      ;;
    --installed)
      CHECK_INSTALLED=1
      shift
      ;;
    --obs-app)
      OBS_APP="$2"
      shift 2
      ;;
    --launch-test)
      LAUNCH_TEST=1
      shift
      ;;
    --launch-seconds)
      LAUNCH_SECONDS="$2"
      shift 2
      ;;
    --obs-major)
      EXPECTED_OBS_MAJOR="$2"
      shift 2
      ;;
    --require-signed)
      REQUIRE_SIGNED=1
      shift
      ;;
    --require-notarized)
      REQUIRE_NOTARIZED=1
      REQUIRE_SIGNED=1
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

log() {
  printf '%s\n' "$*"
}

warn() {
  WARNINGS=$((WARNINGS + 1))
  printf 'WARNING: %s\n' "$*" >&2
}

fail() {
  FAILURES=$((FAILURES + 1))
  printf 'ERROR: %s\n' "$*" >&2
}

cleanup() {
  if [[ -n "$TMP_DIR" ]]; then
    rm -rf "$TMP_DIR"
  fi
}
trap cleanup EXIT

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    fail "$1 is required"
    return 1
  fi
}

plist_value() {
  /usr/libexec/PlistBuddy -c "Print :$2" "$1/Contents/Info.plist" 2>/dev/null || true
}

binary_arches() {
  local binary="$1"
  [[ -f "$binary" ]] || return 0
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
    if [[ "$arch" == "arm64" ]]; then
      case " $right " in *" arm64e "*) return 0 ;; esac
    elif [[ "$arch" == "arm64e" ]]; then
      case " $right " in *" arm64 "*) return 0 ;; esac
    fi
  done

  return 1
}

find_plugin_bundle_in_payload() {
  local expanded="$1"
  find "$expanded/Payload" -maxdepth 8 -path "*/obs-vdoninja.plugin" -type d 2>/dev/null | head -1
}

inspect_plugin_bundle() {
  local bundle="$1"
  local label="$2"
  local binary="$bundle/Contents/MacOS/obs-vdoninja"
  local version
  local arches
  local dep
  local signature_details

  log "== $label =="
  log "Bundle: $bundle"

  if [[ ! -d "$bundle" ]]; then
    fail "$label bundle is missing"
    return
  fi

  if [[ ! -f "$bundle/Contents/Info.plist" ]]; then
    fail "$label Info.plist is missing"
  fi

  version="$(plist_value "$bundle" CFBundleShortVersionString)"
  log "Info.plist version: ${version:-unknown}"

  if [[ ! -x "$binary" ]]; then
    fail "$label executable is missing or not executable: $binary"
    return
  fi

  arches="$(binary_arches "$binary")"
  log "Plugin architecture slice(s): ${arches:-unknown}"
  if [[ -z "$arches" ]]; then
    fail "Could not determine plugin architecture slices"
  fi

  if [[ -n "$EXPECTED_PLUGIN_VERSION" ]] &&
    strings "$binary" | awk -v version="$EXPECTED_PLUGIN_VERSION" 'index($0, version) { found = 1 } END { exit found ? 0 : 1 }'; then
    log "Runtime version string: $EXPECTED_PLUGIN_VERSION"
  else
    warn "Expected runtime version string ${EXPECTED_PLUGIN_VERSION:-unknown} was not found"
  fi

  if codesign --verify --deep --strict "$bundle" >/dev/null 2>&1; then
    log "Bundle code signature: valid"
    signature_details="$(codesign -dv --verbose=4 "$bundle" 2>&1 || true)"
    if printf '%s\n' "$signature_details" | grep -q "Authority=Developer ID Application"; then
      log "Bundle signing identity: Developer ID Application"
    elif printf '%s\n' "$signature_details" | grep -q "Signature=adhoc"; then
      if [[ "$REQUIRE_SIGNED" -eq 1 ]]; then
        fail "$label uses an ad-hoc signature; Developer ID Application signing is required"
      else
        warn "$label uses an ad-hoc signature"
      fi
    else
      if [[ "$REQUIRE_SIGNED" -eq 1 ]]; then
        fail "$label is not signed with a Developer ID Application identity"
      else
        warn "$label signature is not Developer ID Application"
      fi
    fi
  else
    fail "Bundle code signature verification failed"
  fi

  if [[ -L "$bundle/Contents/Resources/locale" ]]; then
    log "Locale symlink: present"
  else
    fail "Contents/Resources/locale symlink is missing"
  fi

  if [[ ! -f "$bundle/Contents/Resources/data/locale/en-US.ini" ]]; then
    fail "English locale file is missing"
  fi

  while read -r dep; do
    [[ -n "$dep" ]] || continue
    case "$dep" in
      /System/*|/usr/lib/*|@rpath/*|@loader_path/*) ;;
      *)
        fail "Non-relocatable dependency remains: $dep"
        ;;
    esac
  done < <(otool -L "$binary" | awk 'NR > 1 {print $1}')

  for dep in libdatachannel.0.dylib libssl.3.dylib libcrypto.3.dylib; do
    if [[ -f "$bundle/Contents/MacOS/$dep" ]]; then
      log "Bundled dependency: $dep"
    else
      fail "Missing bundled dependency: $dep"
    fi
  done
}

check_package_notarization() {
  local pkg="$1"
  local staple_report="$TMP_DIR/pkg-stapler.txt"
  local spctl_report="$TMP_DIR/pkg-spctl.txt"

  if command -v xcrun >/dev/null 2>&1; then
    if xcrun stapler validate "$pkg" >"$staple_report" 2>&1; then
      log "Package notarization staple: valid"
    elif [[ "$REQUIRE_NOTARIZED" -eq 1 ]]; then
      fail "Package notarization staple is missing or invalid"
      cat "$staple_report"
    else
      warn "Package notarization staple is missing or invalid"
    fi
  elif [[ "$REQUIRE_NOTARIZED" -eq 1 ]]; then
    fail "xcrun is required to validate notarization"
  else
    warn "xcrun not found; skipping notarization staple validation"
  fi

  if command -v spctl >/dev/null 2>&1; then
    if spctl -a -vv -t install "$pkg" >"$spctl_report" 2>&1; then
      log "Gatekeeper install assessment: accepted"
    elif [[ "$REQUIRE_NOTARIZED" -eq 1 ]]; then
      fail "Gatekeeper install assessment failed"
      cat "$spctl_report"
    else
      warn "Gatekeeper install assessment failed"
    fi
  fi
}

inspect_obs_app() {
  local obs_app="$1"
  local obs_bin="$obs_app/Contents/MacOS/OBS"
  local version
  local arches

  log "== OBS App =="
  log "OBS app: $obs_app"

  if [[ ! -d "$obs_app" ]]; then
    fail "OBS app does not exist: $obs_app"
    return
  fi

  if [[ ! -x "$obs_bin" ]]; then
    fail "OBS executable is missing: $obs_bin"
    return
  fi

  version="$(plist_value "$obs_app" CFBundleShortVersionString)"
  arches="$(binary_arches "$obs_bin")"
  log "OBS version: ${version:-unknown}"
  log "OBS architecture slice(s): ${arches:-unknown}"

  case "$version" in
    "$EXPECTED_OBS_MAJOR".*) ;;
    "")
      warn "Could not determine OBS version; this package targets OBS $EXPECTED_OBS_MAJOR.x"
      ;;
    *)
      fail "OBS $version is outside the expected OBS $EXPECTED_OBS_MAJOR.x line"
      ;;
  esac
}

launch_obs_and_require_plugin() {
  local obs_app="$1"
  local plugin_parent="${2:-}"
  local temp_home="$TMP_DIR/obs-home"
  local log_dir
  local log_file
  local launch_output
  local obs_bin="$obs_app/Contents/MacOS/OBS"
  local pid

  if [[ -z "$TMP_DIR" ]]; then
    TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/obs-vdoninja-validator.XXXXXX")"
    temp_home="$TMP_DIR/obs-home"
  fi

  mkdir -p "$temp_home"
  launch_output="$TMP_DIR/obs-vdoninja-launch.out"

  log "== OBS Launch Test =="
  log "Launching: $obs_bin"
  if [[ -n "$plugin_parent" ]]; then
    log "Using package plugin path: $plugin_parent"
    HOME="$temp_home" \
      OBS_PLUGINS_PATH="$plugin_parent" \
      OBS_PLUGINS_DATA_PATH="$plugin_parent" \
      "$obs_bin" --multi --disable-updater --disable-missing-files-check --verbose >"$launch_output" 2>&1 &
  else
    "$obs_bin" --multi --disable-updater --disable-missing-files-check --verbose >"$launch_output" 2>&1 &
  fi

  pid=$!
  sleep "$LAUNCH_SECONDS"
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  sleep 2

  if [[ -n "$plugin_parent" ]]; then
    log_dir="$temp_home/Library/Application Support/obs-studio/logs"
  else
    log_dir="$HOME/Library/Application Support/obs-studio/logs"
  fi

  log_file="$(ls -t "$log_dir"/*.txt 2>/dev/null | head -1 || true)"
  if [[ -z "$log_file" ]]; then
    if [[ -s "$launch_output" ]]; then
      warn "OBS launch did not produce a log file; using captured stdout/stderr"
      log_file="$launch_output"
    else
      fail "OBS launch did not produce a log file"
      return
    fi
  fi

  log "OBS log: $log_file"
  if grep -q "Loading module: obs-vdoninja" "$log_file" &&
    grep -q "VDO.Ninja plugin loaded successfully" "$log_file"; then
    grep -En "OBS $EXPECTED_OBS_MAJOR\\.|Loading module: obs-vdoninja|Loading VDO\\.Ninja plugin|VDO\\.Ninja plugin loaded successfully|VDO\\.Ninja plugin unloaded" "$log_file" || true
  else
    fail "OBS did not load obs-vdoninja successfully"
    grep -En "obs-vdoninja|VDO\\.Ninja|Failed to load|incompatible" "$log_file" || true
  fi
}

require_command pkgutil || true
require_command otool || true
require_command lipo || true
require_command codesign || true
require_command strings || true

PACKAGE_PLUGIN_PARENT=""

if [[ -n "$PKG_PATH" ]]; then
  if [[ ! -f "$PKG_PATH" ]]; then
    fail "Package not found: $PKG_PATH"
  else
    if [[ -z "$TMP_DIR" ]]; then
      TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/obs-vdoninja-validator.XXXXXX")"
    fi

    log "== Package =="
    log "Package: $PKG_PATH"
    signature_report="$TMP_DIR/pkg-signature.txt"
    if pkgutil --check-signature "$PKG_PATH" >"$signature_report" 2>&1; then
      log "Package signature: valid"
      if grep -q "Developer ID Installer" "$signature_report"; then
        log "Package signing identity: Developer ID Installer"
      elif [[ "$REQUIRE_SIGNED" -eq 1 ]]; then
        fail "Package is not signed with a Developer ID Installer certificate"
      else
        warn "Package signature is not Developer ID Installer"
      fi
    else
      if grep -q "Status: no signature" "$signature_report"; then
        if [[ "$REQUIRE_SIGNED" -eq 1 ]]; then
          fail "Package is not signed with a Developer ID Installer certificate"
        else
          warn "Package is not signed with a Developer ID Installer certificate"
        fi
      else
        fail "Package signature check failed"
        cat "$signature_report"
      fi
    fi

    check_package_notarization "$PKG_PATH"

    pkgutil --expand-full "$PKG_PATH" "$TMP_DIR/pkg-expanded"
    package_bundle="$(find_plugin_bundle_in_payload "$TMP_DIR/pkg-expanded")"
    if [[ -z "$package_bundle" ]]; then
      fail "Package payload does not contain obs-vdoninja.plugin"
    else
      inspect_plugin_bundle "$package_bundle" "Package Payload"
      PACKAGE_PLUGIN_PARENT="$(dirname "$package_bundle")"
    fi

    if [[ -x "$TMP_DIR/pkg-expanded/Scripts/preinstall" ]]; then
      log "Package preinstall: present"
    else
      fail "Package preinstall script is missing"
    fi

    if [[ -x "$TMP_DIR/pkg-expanded/Scripts/postinstall" ]]; then
      log "Package postinstall: present"
    else
      fail "Package postinstall script is missing"
    fi
  fi
fi

if [[ "$CHECK_INSTALLED" -eq 1 ]]; then
  inspect_plugin_bundle "$HOME/Library/Application Support/obs-studio/plugins/obs-vdoninja.plugin" "Installed User Bundle"
  inspect_plugin_bundle "/Library/Application Support/obs-studio/plugins/obs-vdoninja.plugin" "Installed System Bundle"
fi

if [[ -n "$OBS_APP" ]]; then
  inspect_obs_app "$OBS_APP"

  if [[ "$LAUNCH_TEST" -eq 1 ]]; then
    launch_obs_and_require_plugin "$OBS_APP" "$PACKAGE_PLUGIN_PARENT"
  fi
elif [[ "$LAUNCH_TEST" -eq 1 ]]; then
  fail "--launch-test requires --obs-app"
fi

if [[ "$FAILURES" -gt 0 ]]; then
  log
  log "Validation failed: $FAILURES error(s), $WARNINGS warning(s)."
  exit 1
fi

log
log "Validation passed with $WARNINGS warning(s)."
