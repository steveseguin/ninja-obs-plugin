# Local Stress Testing

This repo has a local-only stress wrapper for crash hunting:

```powershell
cmake --build build-obs32 --config Release --target obs-vdoninja
cmake --install build-obs32 --config Release --prefix install-obs32
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-local-stress.ps1 -Profile quick -InstallPrefix .\install-obs32
```

The build/install commands keep the portable OBS payload aligned with the current source tree. The wrapper does not edit GitHub Actions and does not push anything. It runs existing smoke and stress scripts sequentially, then saves per-step stdout, stderr, copied artifacts, and a machine-readable summary under:

```text
artifacts\local-stress\<run-id>\summary.json
```

## Profiles

`quick` is the default. It runs browser multi-viewer churn, OBS publish smoke, OBS native source smoke, and two native source lifecycle iterations.

`standard` adds source fault recovery and raises lifecycle churn to eight iterations.

`soak` keeps the same flow but raises lifecycle churn to forty iterations with longer waits. Use it when OBS can be left alone for a while.

Useful variants:

```powershell
# Skip browser-only Playwright churn and focus on native OBS plugin paths.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-local-stress.ps1 -Profile quick -SkipBrowser -InstallPrefix .\install-obs32

# Longer local run without changing CI.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-local-stress.ps1 -Profile standard -InstallPrefix .\install-obs32

# Override lifecycle depth for a targeted crash hunt.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-local-stress.ps1 -Profile quick -LifecycleIterations 20 -LifecycleIterationWaitMs 10000 -InstallPrefix .\install-obs32

# Add stricter OBS source screenshots. This is useful for visual validation,
# but less stable than log/connection-based crash hunting.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-local-stress.ps1 -Profile quick -CaptureSourceScreenshots -InstallPrefix .\install-obs32

# Abuse source settings through obs-websocket while OBS is running against a live publisher.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeLocal1 -Password false -InstallPrefix .\install-obs32

# Add multiple simultaneous VDO source instances and OBS canvas/FPS churn.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeLocalMulti1 -Password false -InstallPrefix .\install-obs32 -ExtraSources 3 -VideoChurn -RapidIterations 50 -RapidWaitMs 75 -CheckTimeoutSeconds 300

# Push OBS video settings harder with portrait, ultrawide, odd-size, and 120 FPS cases.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeVideoAggressive1 -Password false -InstallPrefix .\install-obs32 -VideoChurn -VideoChurnProfile aggressive -SkipEdgeCases -RapidIterations 12 -RequestTimeoutMs 120000 -CheckTimeoutSeconds 300

# Add publisher-side churn while OBS is connected. This flips media tracks,
# replaces generated video/audio tracks, sends known data-channel messages,
# and can inject a second audio sender.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgePublisherChurn1 -Password false -PushUrl "https://vdo.ninja/?push=edgePublisherChurn1&password=false&webcam=1&autostart=1&codec=h264&stereo=2" -ViewUrl "https://vdo.ninja/?view=edgePublisherChurn1&password=false&codec=h264&stereo=2" -InstallPrefix .\install-obs32 -PublisherChurn aggressive -PublisherChurnIterations 90 -PublisherChurnIntervalMs 750 -PublisherWarmupSeconds 35 -RapidIterations 40 -RapidWaitMs 100 -RequestTimeoutMs 90000 -CheckTimeoutSeconds 420

# Add official-like data-channel fuzzing while publisher media is changing.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeDataFuzz1 -Password false -InstallPrefix .\install-obs32 -PublisherChurn aggressive -PublisherDataFuzz aggressive -PublisherChurnIterations 30 -PublisherChurnIntervalMs 600 -StableRapidSettings -RapidIterations 35 -RapidWaitMs 100 -RequestTimeoutMs 120000 -CheckTimeoutSeconds 360

# Hold a native source open while data-channel fuzzing continues, then print publisher counters quickly.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeFocusedFuzz1 -Password false -InstallPrefix .\install-obs32 -PublisherChurn aggressive -PublisherDataFuzz aggressive -PublisherChurnIterations 80 -PublisherChurnIntervalMs 500 -PublisherStartupWaitMs 3000 -PublisherViewProbeWaitMs 5000 -PublisherKeepAliveMs 10000 -PublisherWarmupSeconds 10 -SkipEdgeCases -RapidIterations 0 -FinalWaitMs 20000 -RequestTimeoutMs 120000 -CheckTimeoutSeconds 300

# Combine live publisher fuzz with aggressive OBS video-setting churn, without scene visibility churn.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeComboFuzzVideo1 -Password false -InstallPrefix .\install-obs32 -PublisherChurn aggressive -PublisherDataFuzz aggressive -PublisherChurnIterations 100 -PublisherChurnIntervalMs 500 -PublisherStartupWaitMs 3000 -PublisherViewProbeWaitMs 5000 -PublisherKeepAliveMs 15000 -PublisherWarmupSeconds 10 -VideoChurn -VideoChurnProfile aggressive -SkipEdgeCases -RapidIterations 0 -FinalWaitMs 10000 -RequestTimeoutMs 120000 -CheckTimeoutSeconds 420

# Terminal data-channel cleanup pressure. This intentionally sends bye/cleanup fields.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeTerminalFuzz1 -Password false -InstallPrefix .\install-obs32 -PublisherChurn aggressive -PublisherDataFuzz terminal -PublisherChurnIterations 35 -PublisherChurnIntervalMs 500 -PublisherStartupWaitMs 3000 -PublisherViewProbeWaitMs 5000 -PublisherKeepAliveMs 8000 -PublisherWarmupSeconds 10 -SkipEdgeCases -RapidIterations 0 -FinalWaitMs 12000 -RequestTimeoutMs 120000 -CheckTimeoutSeconds 240

# Same publisher-side churn, but keep stream identity and receiver mode stable
# while rapidly changing dimensions, visibility, and transforms.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeStableChurn1 -Password false -PushUrl "https://vdo.ninja/?push=edgeStableChurn1&password=false&webcam=1&autostart=1&codec=h264&stereo=2" -ViewUrl "https://vdo.ninja/?view=edgeStableChurn1&password=false&codec=h264&stereo=2" -InstallPrefix .\install-obs32 -PublisherChurn aggressive -PublisherChurnIterations 90 -PublisherChurnIntervalMs 750 -PublisherWarmupSeconds 35 -ExtraSources 2 -StableRapidSettings -RapidIterations 50 -RapidWaitMs 75 -RequestTimeoutMs 90000 -CheckTimeoutSeconds 420

# Duplicate publishers on the same stream ID with mixed official URL knobs.
# This stresses duplicate offers/presence, codec preference, stereo/mono/multi
# audio declarations, PCM-like URL behavior, extra audio senders, and active
# publisher replaceTrack/null churn while OBS mutates the native source.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeTriple1 -Password false -PushUrl "https://vdo.ninja/?push=edgeTriple1&password=false&webcam=1&autostart=1" -ViewUrl "https://vdo.ninja/?view=edgeTriple1&password=false&codec=h264&stereo=multi" -InstallPrefix .\install-obs32 -PublisherCount 3 -PublisherUrlVariants "codec=h264&stereo=2;codec=vp9&stereo=multi&pcm&preferaudiocodec=red;codec=h264&stereo=mono&preferaudiocodec=opus" -PublisherChurn aggressive -PublisherChurnIterations 100 -PublisherChurnIntervalMs 700 -PublisherWarmupSeconds 70 -ExtraSources 2 -StableRapidSettings -SkipEdgeCases -RapidIterations 80 -RapidWaitMs 75 -RequestTimeoutMs 90000 -CheckTimeoutSeconds 540

# Reconnection pressure: reload publisher pages while OBS remains connected.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeReload1 -Password false -PushUrl "https://vdo.ninja/?push=edgeReload1&password=false&webcam=1&autostart=1&codec=h264&stereo=2" -ViewUrl "https://vdo.ninja/?view=edgeReload1&password=false&codec=h264&stereo=multi" -InstallPrefix .\install-obs32 -PublisherChurn aggressive -PublisherChurnIterations 120 -PublisherChurnIntervalMs 700 -PublisherReloads 2 -PublisherReloadIntervalMs 10000 -PublisherReloadStartupWaitMs 4000 -PublisherWarmupSeconds 55 -StableRapidSettings -SkipEdgeCases -RapidIterations 80 -RapidWaitMs 75 -RequestTimeoutMs 90000 -CheckTimeoutSeconds 540

# Heavier local-only pass: allow slower obs-websocket replies while OBS is overloaded.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeHeavy1 -Password false -InstallPrefix .\install-obs32 -ExtraSources 5 -VideoChurn -RapidIterations 80 -RapidWaitMs 75 -RequestTimeoutMs 120000 -CheckTimeoutSeconds 600

# Aggressive UI/canvas/video churn with live publisher track/data fuzz.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeUiCanvasFuzz1 -Password false -PushUrl "https://vdo.ninja/?push=edgeUiCanvasFuzz1&password=false&webcam=1&autostart=1&codec=h264&stereo=2" -ViewUrl "https://vdo.ninja/?view=edgeUiCanvasFuzz1&password=false&codec=h264&stereo=multi" -InstallPrefix .\install-obs32 -PublisherChurn aggressive -PublisherDataFuzz aggressive -PublisherChurnIterations 120 -PublisherChurnIntervalMs 450 -PublisherStartupWaitMs 3000 -PublisherViewProbeWaitMs 5000 -PublisherKeepAliveMs 20000 -PublisherWarmupSeconds 10 -VideoChurn -VideoChurnProfile aggressive -ExtraSources 3 -SkipEdgeCases -RapidIterations 80 -RapidWaitMs 75 -RequestTimeoutMs 120000 -CheckTimeoutSeconds 600

# Broader source lifecycle pass: invalid settings, browser/native mode toggles,
# two publishers, reloads, mixed codecs, and extra audio while OBS video churns.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeLifecycleFuzz1 -Password false -PushUrl "https://vdo.ninja/?push=edgeLifecycleFuzz1&password=false&autostart=1" -ViewUrl "https://vdo.ninja/?view=edgeLifecycleFuzz1&password=false&codec=h264&stereo=multi" -InstallPrefix .\install-obs32 -PublisherChurn aggressive -PublisherDataFuzz aggressive -PublisherExtraAudio -PublisherChurnIterations 90 -PublisherChurnIntervalMs 500 -PublisherStartupWaitMs 3000 -PublisherViewProbeWaitMs 5000 -PublisherKeepAliveMs 25000 -PublisherWarmupSeconds 10 -PublisherCount 2 -PublisherStaggerMs 1000 -PublisherUrlVariants "codec=h264&stereo=2;codec=vp9&stereo=1" -PublisherReloads 1 -PublisherReloadIntervalMs 12000 -PublisherReloadStartupWaitMs 5000 -VideoChurn -VideoChurnProfile standard -ExtraSources 2 -RapidIterations 45 -RapidWaitMs 100 -RequestTimeoutMs 120000 -CheckTimeoutSeconds 480

# Isolate OBS video reset behavior without first abusing VDO source settings.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-vdoninja-source-edge-stress.ps1 -StreamId edgeVideoOnly1 -Password false -InstallPrefix .\install-obs32 -VideoChurn -SkipEdgeCases -RapidIterations 10 -CheckTimeoutSeconds 240
```

## What This Stresses

- Browser publish/view reload and multiple viewer behavior through Playwright.
- OBS publishing through the native plugin service path.
- OBS viewing through the native source path.
- Repeated source create/check/remove cycles against a live browser publisher.
- Optional source disconnect/reconnect recovery in `standard`, `soak`, or `-IncludeFaultRecovery`.
- OBS source screenshots only when `-CaptureSourceScreenshots` is set.
- Edge-case obs-websocket source mutation with illegal dimensions, native/browser toggles, empty stream IDs, bad ICE
  settings, force-TURN toggles, source visibility churn, and rapid transform changes.
- Optional multi-source pressure with `run-vdoninja-source-edge-stress.ps1 -ExtraSources N`. This creates several
  concurrent VDO source instances pointed at the same stream, mixes native/browser receiver settings and data-channel
  settings, and mutates their visibility, transforms, dimensions, and receiver mode while media callbacks are active.
- Optional OBS video pipeline churn with `-VideoChurn`. This changes OBS base/output resolution and FPS through QHD,
  NTSC-rate 720p, UHD, and small-canvas cases while the VDO source is live, then restores the original video settings.
  `-VideoChurnProfile aggressive` adds UHD 60 FPS, portrait 1080x1920, ultrawide 3440x1440 at 120 FPS, odd 853x481
  at 59.94 FPS, and tiny 160x90 at 15 FPS. Unsupported OBS video settings are recorded in the JSON report instead of
  automatically being treated as plugin crashes.
- Optional publisher churn with `-PublisherChurn basic|aggressive`. The Playwright publisher records peer connections
  and data channels through both constructor capture and VDO.Ninja's official `session.pcs` peer map, then flips
  `MediaStreamTrack.enabled`, uses `RTCRtpSender.replaceTrack()` with generated camera and mono/stereo/6-channel audio
  tracks where Chromium permits them,
  briefly replaces tracks with `null` in aggressive mode, sends known VDO.Ninja-style data-channel messages (`chat`,
  `audioMuted`/`videoMuted`, tally, keyframe, `requestResolution`, `screenShareState`, `getConnectionMap`), and can add
  an extra audio sender with `-PublisherExtraAudio` or the aggressive default. This mirrors VDO.Ninja's real camera
  refresh, director mute, resolution request, screen-share state, and data-channel traffic better than receiver-only
  source mutation.
- `-PublisherDataFuzz off|official|aggressive|terminal` widens publisher data-channel pressure. `official` sends
  VDO.Ninja-shaped ping/pong, stats, OBS state, media-control, screen-share, director-video, director-audio,
  transform, and mesh payloads. `aggressive` adds unsupported or privileged official control fields, malformed JSON,
  large JSON strings, and binary frames. `terminal` additionally sends peer-cleanup fields such as `bye` and
  `request:"cleanup"` and can intentionally tear down the active peer; use it when reconnect behavior is the test, not
  when the final connected sample must pass.
- `-PublisherStartupWaitMs`, `-PublisherViewProbeWaitMs`, and `-PublisherKeepAliveMs` are passed to the Playwright
  publisher. Lower them when the OBS wrapper would otherwise kill the publisher before its JSON counters are flushed.
- `-FinalWaitMs` keeps the OBS source alive after source mutations and video churn. Use it with `-RapidIterations 0`
  when the goal is to observe ongoing media/data-channel behavior rather than source setting churn.
- `-PublisherCount N` opens multiple publisher pages, normally against the same stream ID. `-PublisherUrlVariants`
  accepts semicolon-separated query fragments and applies them round-robin to those publishers, such as
  `"codec=h264&stereo=2;codec=vp9&stereo=multi&pcm&preferaudiocodec=red"`.
- `-PublisherReloads`, `-PublisherReloadIntervalMs`, and `-PublisherReloadStartupWaitMs` reload publisher pages during
  the run. Use these for reconnection/camera-refresh pressure. If the reload happens before the publisher report is
  collected, the current `publisherChurn` counters can reset; the OBS-side timeout/crash result still remains valid.
- `-SkipEdgeCases` can isolate OBS video reset and rapid mutation behavior from the earlier invalid/hostile source
  settings sequence.
- `-StableRapidSettings` keeps the stream ID, password, room, native/browser receiver mode, data-channel setting, and
  reconnect setting stable during the rapid loop. Use it for realistic media churn where the publisher is changing
  tracks/resolution and OBS is changing scene transforms, without also forcing constant signaling teardown.
- `-RequestTimeoutMs` controls the per-request obs-websocket timeout. Keep the default for normal checks; increase it
  for local overload tests so a slow OBS reply is not confused with an OBS process crash.
- `run-vdoninja-source-edge-stress.ps1` captures timeout evidence automatically when OBS exits, the checker process
  times out, or obs-websocket reports timeout/closed/not-connected text. Evidence is written to
  `artifacts\obs-edge-capture-<timestamp>-<reason>` and includes wrapper stdout/stderr, the latest portable OBS logs,
  process metadata, and a live OBS minidump when OBS is still running.

## Interpreting Failures

- A new WER dump under `%LOCALAPPDATA%\CrashDumps` or an early OBS process exit is a crash.
- A `SetVideoSettings` or `SetSceneItemEnabled` timeout now should have an `OBS_EDGE_CAPTURE=...` artifact path. Inspect
  `metadata.json`, the copied OBS log, and `minidump-status.txt` first. If a dump exists, run `cdb -z <dump> -c "~* k; q"`
  and look for plugin callbacks waiting inside OBS source/video locks.
- A previous `SetVideoSettings` timeout during aggressive publisher track/data churn plus aggressive OBS video churn was
  traced to wrapper width/height callbacks querying the private child source during OBS video reconfiguration. The source
  now reports cached wrapper dimensions instead, and the same aggressive UI/canvas/video command completed successfully
  afterward.
- Publisher churn failures are not automatically plugin crashes. Check the edge-stress JSON report and
  `artifacts\obs-source-edge-publisher.out.log`; if `publisherChurn.errors` contains browser `replaceTrack` or
  `addTrack` rejections but OBS stays alive and continues responding, the harness found a browser negotiation limit
  rather than a native plugin crash.
- Aggressive data fuzz can produce libdatachannel `Got unexpected message on stream` log noise when unsupported,
  malformed, or binary data is sent. Treat it as a harness pressure signal unless OBS exits, WER captures a new dump,
  or plugin logs show an uncaught exception.

## What It Does Not Stress

- GitHub Actions runners.
- Installer packaging.
- Long real-user OBS sessions with many manually managed scenes.
- Native crash dump capture from the top-level `run-vdoninja-local-stress.ps1` aggregator. The edge source wrapper
  captures a live OBS minidump only on timeout/OBS-exit evidence paths.
- OS/GPU-only display modes such as HDR, VRR, or multi-monitor color-space changes. Those still need manual Windows
  display changes or a dedicated machine profile, but the edge source stress covers plugin-side settings that can mimic
  the same oversized-canvas and reconfiguration pressure. `&stereo=2`, `&stereo=multi`, `&pcm`, `&codec=h264`, and
  `&codec=vp9` URL variants are useful local knobs when looking for browser/codec negotiation differences.

For crash-focused runs, enable Windows Error Reporting local dumps or run OBS under ProcDump before starting a longer `standard` or `soak` profile. The wrapper keeps OBS/plugin logs and step outputs organized, but it intentionally avoids changing machine-wide crash dump settings.
