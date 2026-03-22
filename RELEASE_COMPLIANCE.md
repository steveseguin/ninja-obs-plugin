# Release Compliance Checklist

Use this checklist before publishing a GitHub release.

## Required

1. Use `scripts/release.ps1` as the release entrypoint so version files, changelog, notes, tag, and push all come from one flow.
2. Tag from a committed state (`vX.Y.Z`) and keep the tag immutable.
3. Include a changelog entry (`CHANGELOG.md`) for the exact version.
4. Ensure source is publicly available for the released binaries:
- GitHub tag/release must point to the exact commit used for build.
- Keep release artifacts traceable to that commit (hash/tag in notes).
5. Include licensing files in release artifacts:
- `LICENSE`
- `THIRD_PARTY_LICENSES.md`
6. Verify license metadata is consistent:
- `AGPL-3.0-only` in repository metadata/docs (`README.md`, `CMakeLists.txt`, `package.json`).
7. Sync release-signing secrets before tagging:
- Run `scripts/sync-release-secrets-windows.ps1` to set `WIN_SIGN_CERT_B64` and `WIN_CSC_KEY_PASSWORD` from local `code-signing` repo.
- Confirm `gh secret list --repo steveseguin/ninja-obs-plugin` contains required secrets.
8. Sync version metadata before release:
- `CMakeLists.txt`
- `src/plugin-main.h`
- `package.json`
- `package-lock.json`
- `CHANGELOG.md`
- release notes/tag text
9. For Windows release validation, use the current portable OBS checklist and fallback note in:
- `docs/windows-obs32-build-and-validation.md`

## Recommended

1. Include build provenance in release notes:
- Build workflow run URL
- Commit SHA
- Build date
2. Include reproducible build instructions link:
- `README.md` build section
3. Include checksum files for release artifacts.
4. Run both a normal publish test and a forced-primary-failure fallback test in portable OBS before tagging a Windows release.
5. If release notes claim cellular / `srflx` success, capture phone-side WebRTC stats or equivalent candidate-pair evidence.

## Structured Flow

1. `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Action status`
2. `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Action prepare -Bump patch`
3. `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Action verify -Version X.Y.Z`
4. `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Action cut -Version X.Y.Z -Push`

`cut` is the one-command path; the staged flow is there when you want to inspect the generated version/changelog changes before tagging.

## Quick Commands

```bash
git rev-parse --short HEAD
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\sync-release-secrets-windows.ps1
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Action status
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\release.ps1 -Action cut -Bump patch -Push
```
