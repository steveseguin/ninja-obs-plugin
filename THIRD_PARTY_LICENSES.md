# Third-Party Licenses

This repository is licensed under `AGPL-3.0-only` (see `LICENSE`).

The plugin depends on third-party projects. This file summarizes the key
licenses and where to find their full terms.

## Runtime / Build Dependencies

1. OBS Studio (`libobs`, `obs-frontend-api`)
- License: `GPL-2.0-or-later`
- Source: https://github.com/obsproject/obs-studio
- Notes: OBS plugin integration links against OBS-provided components.

2. libdatachannel
- License: `MPL-2.0`
- Source: https://github.com/paullouisageneau/libdatachannel
- Notes: Used for WebRTC peer connections and data channels.

3. OpenSSL
- License: `Apache-2.0`
- Source: https://www.openssl.org/
- Notes: Used for cryptographic operations.

## Test / Dev Dependencies

1. GoogleTest / GoogleMock
- License: `BSD-3-Clause`
- Source: https://github.com/google/googletest
- Notes: Used only for unit testing.

2. Playwright
- License: `Apache-2.0`
- Source: https://github.com/microsoft/playwright
- Notes: Used only for end-to-end browser tests.

## Distribution Notes

1. Keep this file and `LICENSE` with release artifacts.
2. Keep upstream notices intact for bundled third-party binaries/libraries.
3. If dependencies change, update this file in the same pull request.
