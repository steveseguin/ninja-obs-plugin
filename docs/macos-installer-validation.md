# macOS Installer Validation

This project targets OBS `32.x` and currently builds release artifacts against OBS `32.0.4`.

## OBS Load Paths

OBS 32.x on macOS loads third-party `.plugin` bundles from the user's OBS plugin directory:

```text
~/Library/Application Support/obs-studio/plugins/%module%.plugin
```

The OBS app bundle path does not need to be `/Applications/OBS.app` for this user-level plugin path to work. A user can run OBS from `/Applications`, `~/Applications`, a renamed folder, or a temporary validation folder and OBS will still scan the user plugin directory.

The `.pkg` installer stores a system copy at:

```text
/Library/Application Support/obs-studio/plugins/obs-vdoninja.plugin
```

During `postinstall`, it syncs that bundle into the active user's OBS plugin directory. This avoids the Apple Silicon issue where OBS does not scan the system-level `.plugin` bundle path by itself.

## Compatibility Checks

The macOS `.pkg` scripts perform these checks:

- Fail before install when the Mac CPU architecture is not present in the package's plugin binary.
- Install the bundle into the active user's OBS plugin directory.
- Remove quarantine attributes and verify the installed bundle signature.
- Log detected OBS apps from `/Applications`, `~/Applications`, and Spotlight metadata.
- Warn when detected OBS is not `32.x`.
- Warn when detected OBS architecture slices do not overlap the plugin architecture slices.
- Warn when OBS is not found, while still installing the plugin into the user OBS plugin directory.

Installer diagnostics are written to:

```text
/Library/Logs/obs-vdoninja-installer.log
```

## Apple Silicon And Intel

The current macOS release artifact is `arm64`:

```text
obs-vdoninja-macos-arm64.pkg
```

That package is valid for Apple Silicon OBS running natively. It is not valid for:

- Intel Macs.
- Apple Silicon Macs running the Intel OBS build under Rosetta.

Those users need a separate `x86_64` package, or a universal package that contains both `arm64` and `x86_64` slices. The current local Xcode SDK does not include `AGL.framework`, which blocks building the OBS 32.x Intel dependency chain on this machine. Build the Intel artifact on a compatible older macOS/Xcode SDK or a known-good Intel runner.

## Signing And Notarization

Release `.pkg` artifacts should be:

- Signed internally with `Developer ID Application` for the `.plugin` bundle and bundled dylibs.
- Signed externally with `Developer ID Installer` for the `.pkg`.
- Submitted to Apple's notary service.
- Stapled before upload.

The package builder supports the same Apple ID/team/app-specific-password environment pattern used by other macOS release scripts:

```bash
export OBS_VDONINJA_APPLE_ID="apple@example.com"
export OBS_VDONINJA_TEAM_ID="TEAMID1234"
export OBS_VDONINJA_APPLE_ID_PASSWORD="app-specific-password"

scripts/build-installer-macos.sh \
  --package-root install \
  --output-dir artifacts \
  --arch arm64 \
  --dylib-search-dir /path/to/libdatachannel/lib \
  --app-sign "Developer ID Application: Steve Seguin (H3CKR5XB3J)" \
  --sign "Developer ID Installer: Steve Seguin (H3CKR5XB3J)" \
  --notarize
```

Alternatively, store notary credentials once and build with `--notary-profile`:

```bash
xcrun notarytool store-credentials obs-vdoninja-notary \
  --apple-id "$OBS_VDONINJA_APPLE_ID" \
  --team-id "$OBS_VDONINJA_TEAM_ID"

scripts/build-installer-macos.sh \
  --package-root install \
  --output-dir artifacts \
  --arch arm64 \
  --app-sign "Developer ID Application: Steve Seguin (H3CKR5XB3J)" \
  --sign "Developer ID Installer: Steve Seguin (H3CKR5XB3J)" \
  --notarize \
  --notary-profile obs-vdoninja-notary
```

If `pkgbuild` hangs while signing on a local machine, unlock the login keychain and grant Apple signing tools access to the private keys:

```bash
security unlock-keychain ~/Library/Keychains/login.keychain-db
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KEYCHAIN_PASSWORD" ~/Library/Keychains/login.keychain-db
```

If multiple valid certificates have the same display name, pass the exact SHA-1 identity from `security find-identity -v -p basic` to `--app-sign` or `--sign`.

Every GitHub Actions run of the release build requires these repository secrets for macOS packages. The workflow
fails during its initial validation job if any are missing, before platform builds begin:

- `MACOS_CERTIFICATE_P12`: base64-encoded `.p12` containing the Developer ID Application and Installer certificates.
- `MACOS_CERTIFICATE_PASSWORD`: password for that `.p12`.
- `MACOS_NOTARY_APPLE_ID`: Apple ID used for notarization.
- `MACOS_NOTARY_TEAM_ID`: Apple developer team ID.
- `MACOS_NOTARY_PASSWORD`: app-specific password for the Apple ID.

Optional identity override secrets are `MACOS_APP_SIGN_IDENTITY` and `MACOS_INSTALLER_SIGN_IDENTITY`.

## Validation Commands

Validate a package payload without installing it:

```bash
scripts/validate-macos-installer.sh \
  --pkg artifacts/obs-vdoninja-macos-arm64.pkg \
  --obs-app /Applications/OBS.app
```

Validate and launch OBS briefly using the package payload:

```bash
scripts/validate-macos-installer.sh \
  --pkg artifacts/obs-vdoninja-macos-arm64.pkg \
  --obs-app /Applications/OBS.app \
  --launch-test
```

Strict release validation requires Developer ID signatures, a notarization staple, and Gatekeeper acceptance:

```bash
scripts/validate-macos-installer.sh \
  --pkg artifacts/obs-vdoninja-macos-arm64.pkg \
  --obs-app /Applications/OBS.app \
  --launch-test \
  --require-notarized
```

Install the package:

```bash
sudo installer -pkg artifacts/obs-vdoninja-macos-arm64.pkg -target /
```

Validate installed user/system bundles and launch OBS:

```bash
scripts/validate-macos-installer.sh \
  --installed \
  --obs-app /Applications/OBS.app \
  --launch-test
```

For release packages, validate installed bundles still carry the Developer ID Application signature:

```bash
scripts/validate-macos-installer.sh \
  --installed \
  --obs-app /Applications/OBS.app \
  --launch-test \
  --require-signed
```

## Manual Matrix

Before publishing a macOS release, cover this matrix:

| Scenario | Expected result |
| --- | --- |
| Apple Silicon Mac, native OBS 32.0.4 from `/Applications/OBS.app` | Loads `obs-vdoninja` successfully. |
| Apple Silicon Mac, native OBS 32.x from a nonstandard path | Loads from the user plugin directory. |
| Apple Silicon Mac, OBS not installed yet | Installer succeeds and logs that OBS was not found. |
| Apple Silicon Mac, OBS 31.x or 33.x | Installer succeeds but logs an OBS version warning. |
| Apple Silicon Mac running Intel OBS under Rosetta | Installer logs architecture mismatch; OBS will not load the arm64 package. |
| Intel Mac with arm64 package | Installer preinstall fails with a clear architecture error. |
| Intel Mac with future x86_64 package | Should load in Intel OBS 32.x. |

The OBS log should include:

```text
Loading module: obs-vdoninja
[VDO.Ninja] Loading VDO.Ninja plugin v<version>
[VDO.Ninja] VDO.Ninja plugin loaded successfully
```
