# Release Compliance Checklist

Use this checklist before publishing a GitHub release.

## Required

1. Tag from a committed state (`vX.Y.Z`) and keep the tag immutable.
2. Include a changelog entry (`CHANGELOG.md`) for the exact version.
3. Ensure source is publicly available for the released binaries:
- GitHub tag/release must point to the exact commit used for build.
- Keep release artifacts traceable to that commit (hash/tag in notes).
4. Include licensing files in release artifacts:
- `LICENSE`
- `THIRD_PARTY_LICENSES.md`
5. Verify license metadata is consistent:
- `AGPL-3.0-only` in repository metadata/docs (`README.md`, `CMakeLists.txt`, `package.json`).

## Recommended

1. Include build provenance in release notes:
- Build workflow run URL
- Commit SHA
- Build date
2. Include reproducible build instructions link:
- `README.md` build section
3. Include checksum files for release artifacts.

## Quick Commands

```bash
git tag -a vX.Y.Z -m "Release vX.Y.Z"
git push origin vX.Y.Z
```

```bash
git rev-parse --short HEAD
```
