#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Exercise the validator function with real otool output shapes, without
# requiring macOS tools or a packaged binary on the Linux unit-test runner.
eval "$(sed -n '/^check_macos_deployment_target() {/,/^}/p' "$repo_root/scripts/validate-macos-installer.sh")"
validation_failures=0
test_load_commands=""
fail() { validation_failures=$((validation_failures + 1)); }
otool() { printf '%s\n' "$test_load_commands"; }
check_case() {
  local expected="$1"
  test_load_commands="$2"
  validation_failures=0
  check_macos_deployment_target fixture.dylib
  if [[ "$validation_failures" -ne "$expected" ]]; then
    echo "Unexpected deployment-target result: expected $expected, got $validation_failures" >&2
    exit 1
  fi
}
check_case 0 $'cmd LC_BUILD_VERSION\nplatform MACOS\nminos 13.0\nsdk 26.4'
check_case 0 $'cmd LC_BUILD_VERSION\nminos 12.0'
check_case 0 $'cmd LC_BUILD_VERSION\nminos 9.0'
check_case 1 $'cmd LC_BUILD_VERSION\nminos 26.0'
check_case 1 $'cmd LC_BUILD_VERSION\nminos 13.1'
check_case 1 $'cmd LC_BUILD_VERSION\nminos 13.0.1'
check_case 0 $'cmd LC_VERSION_MIN_MACOSX\ncmdsize 16\nversion 11.0\nsdk 26.4'
check_case 1 $'cmd LC_VERSION_MIN_MACOSX\ncmdsize 16\nversion 14.0\nsdk 26.4'
check_case 1 $'cmd LC_BUILD_VERSION\nminos 13.0\ncmd LC_BUILD_VERSION\nminos 26.0'
check_case 1 ''
echo 'Passed 10 macOS deployment-target cases'
