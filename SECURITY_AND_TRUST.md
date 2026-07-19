# Security and Trust Notes

## Release Verification

Every release includes `checksums.txt` with SHA-256 hashes for packaged artifacts.

## Binary Signing Status

- Windows: the release workflow signs the plugin DLL and installer when the repository signing secrets are present.
- macOS: GitHub release builds require Developer ID signing and Apple notarization. The plugin bundle and bundled
  dylibs are signed with `Developer ID Application`, the `.pkg` is signed with `Developer ID Installer`, and the
  notarization ticket is stapled and validated before upload.
- Linux: unsigned tarball distribution

Windows trust warnings can still appear when the signing certificate is not rooted in the user's trust store. The
macOS ZIP is a manual-install/troubleshooting fallback; use the signed and notarized `.pkg` for the normal install.

## Recommended Verification Flow

1. Download artifact and `checksums.txt` from the same release tag.
2. Verify SHA-256 before install.
3. Use only official repository release URLs.

## Operational Security Guidance

- Keep OBS and plugin versions up to date.
- Use passwords for streams when practical.
- For controlled environments, set custom signaling and ICE servers.
- Treat remote-control style data-channel actions as sensitive; keep disabled unless explicitly trusted.

## Roadmap

- Add signed SBOM/provenance metadata if distribution requirements demand it.
