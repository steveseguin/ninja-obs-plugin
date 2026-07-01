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

## What It Does Not Stress

- GitHub Actions runners.
- Installer packaging.
- Long real-user OBS sessions with many manually managed scenes.
- Native crash dump capture by default.
- OS/GPU-only display modes such as HDR, VRR, or multi-monitor color-space changes. Those still need manual Windows
  display changes or a dedicated machine profile, but the edge source stress covers plugin-side settings that can mimic
  the same oversized-canvas and reconfiguration pressure.

For crash-focused runs, enable Windows Error Reporting local dumps or run OBS under ProcDump before starting a longer `standard` or `soak` profile. The wrapper keeps OBS/plugin logs and step outputs organized, but it intentionally avoids changing machine-wide crash dump settings.
