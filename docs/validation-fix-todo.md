# Validation and Fix TODO

This backlog tracks behavior that is confirmed by code inspection, still needs runtime validation, or needs a product decision before it should be changed. It is intentionally separate from the changelog: an item belongs here until it is reproduced, fixed, and covered by an appropriate test.

Current reference point: plugin v1.1.65, OBS 32.2.x, reviewed 2026-08-09.

## Status Definitions

- **Confirmed in code:** The implementation path is clear, but the user-visible result should still be reproduced in OBS before changing it.
- **Validation gap:** The intended behavior looks correct in code or scripts but needs a real application/package test.
- **Decision needed:** The code and current UI disagree about which behavior should remain.

## High Priority

### 1. Studio Go Live can discard advanced service settings

- [ ] Reproduce in portable OBS 32.2.x.
- [ ] Decide whether Studio should preserve advanced settings or expose them directly.
- [ ] Implement the selected behavior.
- [ ] Add a regression test covering every advanced service field.

Status: **Confirmed in code; runtime validation pending.**

2026-08-09 validation note: portable OBS launched with sentinel settings, but the Windows UI automation helper failed before any Studio input could be made. `Go Live` was not clicked, so this item was **not reproduced** and no behavior change is justified yet.

The Studio dock sends only stream ID, room ID, password, maximum viewers, and auto-inbound values when `Go Live` is clicked. `activateVdoNinjaServiceFromSettings()` creates a new service from defaults and then applies only those submitted values. Existing values for signaling, salt, custom ICE/TURN, Force TURN, packet protection, Audio RED, and adaptive bitrate are therefore not copied into the replacement service.

Relevant code:

- [`VDONinjaDock::onGoLiveClicked()`](../src/vdoninja-dock.cpp)
- [`activateVdoNinjaServiceFromSettings()`](../src/plugin-main.cpp)
- [`vdoninja_service_defaults()`](../src/plugin-main.cpp)

Validation procedure:

1. In portable OBS, select the VDO.Ninja service and set distinctive non-default values for every advanced field.
2. Save the settings and confirm they remain present after reopening `Settings -> Stream`.
3. Open `Tools -> VDO.Ninja Studio` and click `Go Live`.
4. Stop publishing and reopen the service settings.
5. Record which advanced values were retained or reset.

Expected fix direction: seed the new service settings from the active VDO.Ninja service before applying the dock's basic fields, unless the intended product behavior is to expose and own every advanced setting in Studio.

### 2. Studio may retain an old password when the active service has no password

- [ ] Reproduce in portable OBS 32.2.x.
- [ ] Confirm the desired behavior when switching between password-protected and passwordless sessions.
- [ ] Clear or replace the dock password deterministically when syncing from the active service.
- [ ] Add a regression test for protected-to-passwordless transitions.

Status: **Confirmed code path; user-visible effect needs runtime validation.**

2026-08-09 validation note: the same UI-helper failure prevented the required Studio transition. The seeded portable profile was restored without pressing `Go Live`; this item remains **unreproduced**.

`VDONinjaDock::loadFromServiceSettings()` updates the password field only when the active service password is non-empty. If the dock previously stored a password and the active service is passwordless, the old dock password can remain visible internally and can be reused by the next Studio `Go Live` action.

Relevant code: [`VDONinjaDock::loadFromServiceSettings()`](../src/vdoninja-dock.cpp).

Validation procedure:

1. Publish a password-protected session from Studio, then stop it.
2. Configure the active VDO.Ninja service with an empty password.
3. Reopen or resync Studio.
4. Confirm whether the old password remains in the dock and whether `Go Live` republishes with it.

## Medium Priority

### 3. Built-in English fallback still describes Studio as full setup

- [ ] Update the fallback text after the intended Studio/advanced-settings behavior is settled.
- [ ] Test OBS with the locale data directory temporarily unavailable.
- [ ] Confirm the fallback matches the packaged English locale wording.

Status: **Confirmed text mismatch.**

The packaged English locale now accurately separates Studio's basic controls from advanced service settings. The built-in fallback string in `vdoninja_service_properties()` still says Studio provides full setup including salt and signaling. Users normally receive the locale text, but missing or unreadable locale files expose the stale fallback.

Relevant code: [`vdoninja_service_properties()`](../src/plugin-main.cpp) and [`data/locale/en-US.ini`](../data/locale/en-US.ini).

### 4. Legacy Control Center implementation is registered but has no Tools menu entry

- [ ] Decide whether Control Center is intentionally retained for compatibility or should be removed.
- [ ] Confirm that no profile, scene collection, automation, or test depends on its private source ID.
- [ ] If retained, document its purpose in code and remove misleading unused menu locale keys.
- [ ] If removed, update lifecycle tests and verify unload/reload safety.

Status: **Decision needed.**

The plugin registers the private `vdoninja_control_center` source and maintains its implementation and lifecycle tracking, but the only exposed Tools action opens VDO.Ninja Studio. `getOrCreateControlCenterSource()` currently has no caller.

Relevant code: [`plugin-main.cpp`](../src/plugin-main.cpp).

This may be harmless legacy code, but it increases the lifecycle surface and previously caused the documentation to describe an interface users cannot open.

### 5. Newer locale keys are absent from most translations

- [ ] Start OBS once with each supported locale family, prioritizing `en-GB` and one fully translated non-English locale.
- [ ] Confirm missing keys fall back to readable en-US text rather than displaying raw key names.
- [ ] Decide whether English fallback is acceptable or translations should be refreshed.
- [ ] Add a locale-key parity check that distinguishes required translations from permitted fallback keys.

Status: **Validation gap; fallback is expected but has not been checked across all locales.**

The non-English locale files predate several Studio, native receiver, packet-protection, and Control Center strings. The default en-US locale should supply missing values, but the rendered behavior should be confirmed in OBS.

Relevant files: [`data/locale`](../data/locale).

## Packaging and Documentation Follow-up

### 6. Validate macOS installer warnings against real OBS versions and locations

- [ ] Test native Apple Silicon OBS 32.2.x from `/Applications/OBS.app`.
- [ ] Test native Apple Silicon OBS 32.2.x from a renamed or nonstandard path.
- [ ] Confirm OBS 32.0-32.1 and a future/nonmatching version produce warnings without blocking installation.
- [ ] Confirm Intel OBS under Rosetta produces the architecture warning and does not load the arm64 plugin.

Status: **Script behavior confirmed; application-level matrix still needs real-machine coverage.**

The expected matrix is documented in [`macos-installer-validation.md`](macos-installer-validation.md).

### 7. Publish the corrected documentation

- [ ] Review the pending documentation diff.
- [ ] Commit and push the documentation changes.
- [ ] Confirm the GitHub Pages deployment succeeds.
- [ ] Verify the public quick-start says `Tools -> VDO.Ninja Studio` and explains the advanced-settings limitation.
- [ ] Ensure the next release archives contain the corrected `README.md`, `INSTALL.md`, and `QUICKSTART.md`.

Status: **Pending deployment.**

The repository source has been corrected locally, but the public page and existing v1.1.65 archives will retain their current contents until the changes are published or a new release is built.

## Additional Code Review Findings

### 8. Native receiver settings can race signaling and RTC callbacks

- [ ] Add a stress test that repeatedly updates an active native receiver while signaling, retry, and track callbacks are running.
- [ ] Run the stress path under ThreadSanitizer on a supported build host.
- [ ] Protect `SourceSettings` with a mutex and immutable snapshots, or stop and drain every reader before replacing it.
- [ ] Ensure one connection generation consistently uses one settings snapshot.

Status: **Static-analysis concern only; runtime symptom not reproduced. Do not change production behavior yet.**

`VDONinjaSource::loadSettings()` mutates strings, vectors, and booleans in `settings_` without synchronization. The connection thread and asynchronous signaling/RTC callbacks read the same object. During an active native-source update, the new settings are written before `disconnect()` drains those readers. Concurrent read/write access to `std::string` or `std::vector` is undefined behavior and can produce corrupted connection parameters or a crash.

The Windows build used for this review has no ThreadSanitizer coverage for this path. Passing ordinary stress or unit tests would not prove that the suspected race occurred, so this remains a hypothesis until a sanitizer report, crash, or deterministic test reproduces it.

Relevant code:

- [`VDONinjaSource::loadSettings()` and `VDONinjaSource::update()`](../src/vdoninja-source.cpp)
- [`VDONinjaSource::connectionThread()` and its callbacks](../src/vdoninja-source.cpp)
- [`VDONinjaSource::settings_`](../src/vdoninja-source.h)

### 9. Auto-inbound treats a matching source name as managed ownership

- [x] Pre-create an unrelated OBS source whose name matches an auto-inbound generated name.
- [ ] Test two inbound identifiers that sanitize to the same name, such as `a/b` and `a?b`.
- [x] Observe the matching source through inbound add and output stop.
- [ ] Add an ownership marker and collision-resistant identity before enabling removal.

Status: **Behavior reproduced in portable OBS; ownership intent still requires a product decision.**

Auto-inbound derives source identity solely from a sanitized display name. Distinct identifiers can collapse to the same name because every unsupported character becomes `_`. When that name already exists, the manager updates it without checking its source type or ownership. Disconnect and stop paths later call `obs_source_remove()` by that name. A user source or another inbound stream can therefore be changed or removed.

2026-08-09 runtime result on portable OBS 32.1.0: a pre-existing `color_source_v3` named for the incoming stream remained a color source, but auto-inbound added Browser Source fields (`url`, `width`, `height`, `fps`, and audio/restart settings) to it. After stopping the VDO output, `GetInputList` no longer contained that source. This confirms the current name-based management behavior. It does not by itself establish whether reusing a matching name is intentional, so no ownership change should be made until that policy is decided.

Relevant code: [`VDOAutoSceneManager::onStreamAdded()`, `queueStreamRemovalLocked()`, `stop()`, and `sanitizeNameToken()`](../src/vdoninja-auto-scene-manager.cpp).

### 10. Auto-inbound capacity limits need intent and load validation

- [ ] Flood a test room listing with unique stream IDs and measure UI responsiveness and memory use.
- [ ] Send many unique playback hints from one connected data-channel peer.
- [ ] Define a configurable source cap, identifier/URL length limits, and per-peer rate limiting.
- [x] Confirm the current playback-hint host behavior in tests.

Status: **No runtime defect reproduced; capacity policy remains undecided.**

Room listings add every unique non-owned stream, and connected peers can submit HTTP(S) or WHEP playback hints that use the same creation path. There is no maximum managed-source count or per-peer rate limit, but no practical exhaustion case was run during this validation pass. Existing `DataChannelTest` cases explicitly expect URLs on `example.com` to be accepted, so arbitrary HTTP(S) host acceptance is current tested behavior rather than a newly discovered defect. Do not restrict hosts or add caps until the intended feature contract and a realistic load failure are established.

Relevant code:

- [`VDOAutoSceneManager::onRoomListing()` and `onStreamAdded()`](../src/vdoninja-auto-scene-manager.cpp)
- [`VDONinjaDataChannel::extractInboundPlaybackHint()`](../src/vdoninja-data-channel.cpp)
- Data-channel playback-hint handling in [`VDONinjaOutput`](../src/vdoninja-output.cpp)

### 11. OBS log identifier redaction needs a policy decision

- [x] Review a normal INFO-level OBS log for session identifiers.
- [ ] Review an explicitly enabled debug log for SDP, ICE candidates, and local addresses.
- [ ] Decide which values support diagnostics actually require.
- [ ] Replace raw identifiers with short fingerprints where possible and reserve full signaling payloads for explicit debug logging.
- [ ] Update tests and validation scripts that currently match raw room IDs in log lines.

Status: **Logging behavior reproduced; whether it is excessive remains a policy decision.**

INFO-level messages log the raw room or stream ID alongside its resolved hash when joining, publishing, and viewing. These identifiers can be sensitive when users share OBS logs for support. DEBUG-level signaling logs include complete sent and received JSON, which can also contain SDP and ICE details. Passwords are not directly logged in the reviewed paths.

2026-08-09 runtime result: the portable OBS log contained the exact test identifiers in `Joining room: codexZeroViewerRoom20260809` and `Publishing stream: codexZeroViewerBytes20260809`. This proves the behavior but not that it is wrong; raw identifiers may be useful for support. Redaction should be a deliberate diagnostics/privacy decision, not an automatic code change.

Relevant code: [`VDONinjaSignaling::joinRoom()`, `publishStream()`, `viewStream()`, and signaling send/receive logging](../src/vdoninja-signaling.cpp).

### 12. The Studio `Sent` counter measures encoded output with no viewer

- [x] Measure the counter with zero connected viewers.
- [ ] Compare the counter with one and two connected viewers.
- [ ] Repeat while deliberately saturating the media queue.
- [ ] Decide whether the intended metric is encoded bytes produced, successfully queued media, or aggregate peer-network bytes.
- [ ] Rename the label or move accounting to the matching send path.

Status: **Zero-viewer behavior reproduced; desired metric semantics need a product decision.**

`VDONinjaOutput::data()` increments `totalBytes_` once for every encoded OBS packet after processing. It increments with no viewer connected, includes packets that may subsequently be dropped, and does not multiply for multiple peer sends or include transport overhead. Studio displays this value as `Sent`, so the current label implies a network measurement the implementation does not provide.

2026-08-09 runtime result on portable OBS 32.1.0: no viewer was opened. `GetStreamStatus` reported `outputBytes=213545` at 0.383 seconds and `outputBytes=3968045` at 5.388 seconds. The plugin telemetry simultaneously reported `0 viewers` and zero media frames/packets sent to peers. This conclusively establishes that the counter is encoded output bytes, not peer-network bytes. That may be intentional; prefer clarifying the label only after confirming the intended meaning, and do not move accounting into the transport path without a separate one/two-viewer comparison.

Relevant code: [`VDONinjaOutput::data()`](../src/vdoninja-output.cpp) and [`VDONinjaDock::updateStats()`](../src/vdoninja-dock.cpp).

### 13. Hide-on-disconnect can use a different scene from source creation

- [x] Leave the auto-inbound target scene empty and enable hide rather than remove on disconnect.
- [x] Add an inbound source, switch to another scene, and then disconnect it.
- [x] Reproduce that the item in the original scene remains enabled.
- [x] Track the actual scene association for each managed stream instead of resolving the current scene again during removal.
- [x] Add focused regression coverage for retaining, replacing, and consuming scene assignments.
- [x] Re-run the scene-switch/disconnect path in portable OBS with the rebuilt plugin.

Status: **Fixed and validated locally; pending commit and release.**

An empty target-scene setting means "current scene." Source creation resolves that scene when its UI task runs, but the later hide path independently resolves whatever scene is current at removal time. If the operator has changed scenes, the manager can search the wrong scene and leave the original item visible.

2026-08-09 runtime result on portable OBS 32.1.0: auto-inbound created and enabled the Browser Source in scene A. After switching to an empty scene B and disconnecting the remote publisher, scene A still reported the same item with `sceneItemEnabled=true`; scene B had no items. This exactly reproduced the failure against the advertised hide-on-disconnect setting and established the required scene-association behavior for the fix.

2026-08-09 fix validation: the manager now retains the UUID of the scene where the item was actually added and consumes that association during removal. A deterministic signaling add/remove test through portable OBS 32.1.0 confirmed that the item began enabled in scene A, remained associated with scene A after switching to empty scene B, and changed to `sceneItemEnabled=false` on removal while scene B remained empty. All 637 core tests and 17 module-lifecycle tests passed; the focused auto-inbound subset contains eight passing tests.

Relevant code: [`acquireTargetSceneSource()` and `VDOAutoSceneManager::queueStreamRemovalLocked()`](../src/vdoninja-auto-scene-manager.cpp).

### 14. Output settings loaded from profiles are not consistently bounded

- [ ] Test a manually edited profile with negative, zero, and very large bitrate, viewer-count, auto-inbound dimension, and layout values.
- [ ] Clamp all values at load time using the same limits presented by OBS properties.
- [ ] Use checked arithmetic when converting kbps to bps.
- [ ] Add settings-load tests that bypass the properties UI.

Status: **Static hardening concern only; malformed-profile behavior was not reproduced. Do not change limits yet.**

The output loader multiplies the configured bitrate by 1000 in an `int` without first clamping it, accepts viewer counts above the UI maximum, and accepts unbounded auto-inbound dimensions and enum values. OBS normally enforces property limits, but imported or manually edited profile data can bypass them.

No malformed-profile runtime test was completed in this pass. The UI limits may be the intended contract, and values such as viewer count may intentionally support larger imported configurations. Confirm an actual overflow, crash, invalid allocation, or clearly documented limit violation before changing these settings.

Relevant code: [`VDONinjaOutput::loadSettings()`](../src/vdoninja-output.cpp).

### 15. JSON utilities do not support all valid JSON string escapes

- [ ] Add round-trip tests for every C0 control byte, `\b`, `\f`, and `\uXXXX` sequences, including surrogate pairs.
- [x] Reproduce representative C0, `\b`/`\f`, and basic Unicode-escape behavior with temporary focused assertions.
- [ ] Confirm how browsers encode those values in chat, metadata, and control messages.
- [ ] Replace the simplified parser with a standards-compliant implementation or complete and fuzz the current one.

Status: **Standards behavior reproduced; no user-facing plugin failure reproduced.**

`JsonBuilder` escapes the common named controls but emits other bytes below `0x20` directly, which is invalid JSON. `JsonParser` does not decode Unicode escapes and does not decode `\b` or `\f` correctly. Normal ASCII and UTF-8 paths pass the current tests, but valid browser-generated strings containing these escapes can be corrupted.

2026-08-09 validation result: temporary assertions reproduced all three representative cases. The builder emitted raw `0x01`; the parser returned `abbfc` for an input containing `a\\bb\\fc`; and `prefix-\\u0041-suffix` became `prefix-u0041-suffix` instead of `prefix-A-suffix`. The temporary assertions were removed, the test binary was rebuilt, and all 40 existing JSON builder/parser tests passed. Do not replace the parser until an affected real message path is demonstrated or full JSON compliance is explicitly chosen as a requirement.

Relevant code: [`JsonBuilder` and `JsonParser`](../src/vdoninja-utils.cpp) and [`test-json.cpp`](../tests/test-json.cpp).

## Already Checked

- [x] The v1.1.65 Studio dock includes vertical scrolling and no longer imposes its full content height on the OBS window.
- [x] The v1.1.65 Linux and macOS release builds completed successfully.
- [x] Current package names and OBS compatibility tables agree on OBS 32.2.x, with separate Windows 32.0-32.1 artifacts.
- [x] The Linux v1.1.65 archive uses `lib/x86_64-linux-gnu/obs-plugins`.
- [x] Tracked Markdown relative links and local website anchors resolve.
- [x] Direct Windows test binaries passed during the 2026-08-09 review: 635 core tests and 17 module-lifecycle tests.
