# RootTools Runtime Release Process

## Design

RootTools Runtime uses a local-first release model.

The maintainer Mac performs:

1. Contract tests
2. iOS Release build
3. Rootless DEB and TrollStore-compatible IPA packaging
4. SHA256 and source/build manifest generation
5. Git tag creation in the public source repository
6. GitHub Release upload to the dedicated private artifact repository

GitHub Actions does not perform automatic builds. The remaining workflow is
manual-only and limited to lightweight repository hygiene so normal pushes do
not consume hosted runner minutes.

## Local release

```bash
Scripts/release-local.sh 0.23.0-2 \
  --credential-profile candidate-v0.23.0-2 \
  --repo arthurwangwsx-code/RootTools-runtime-releases \
  --draft
```

The script requires a clean working tree and an isolated non-`installed`
credential profile, runs the full local validation/build,
packages the exact Debian version and matching IPA, generates checksums and a
source/build manifest, creates/pushes the annotated tag, and creates the GitHub
Draft Release with both artifacts and the install guide. Before any build or
push, it also verifies that `origin` is
`arthurwangwsx-code/RootTools-runtime` and that local `main` tracks
`origin/main`. It then confirms that the artifact repository is exactly
`arthurwangwsx-code/RootTools-runtime-releases` and remains private; the legacy
mixed checkout is neither a release source nor an artifact target.

`VERSION` is the single release-version source in Debian form `X.Y.Z-N`.
The build derives daemon/App version `X.Y.Z` and App build `N` from it. Packaging
rejects a caller-supplied mismatch, stale build-version stamps, or an App plist
whose version does not match the candidate package.

DEB archive metadata uses `SOURCE_DATE_EPOCH`, defaulting to the timestamp of
the current Git commit. Repackaging the same committed inputs therefore
produces the same bytes and checksum. A caller may set `SOURCE_DATE_EPOCH`
explicitly for an equivalent controlled build environment.

When the exact artifacts were already built and validated locally, uploading
can skip rebuilding:

```bash
Scripts/release-local.sh 0.23.0-2 \
  --credential-profile candidate-v0.23.0-2 \
  --repo arthurwangwsx-code/RootTools-runtime-releases \
  --skip-build --draft
```

## Result

The canonical tag/release format is `vX.Y.Z-N`, matching the Debian package
version. Release assets are:

- `roottools_X.Y.Z-N_iphoneos-arm64.deb`
- `RootTools_X.Y.Z-N.ipa`
- `SHA256SUMS`
- `SOURCE_BUILD_MANIFEST.json`
- `INSTALL.md`
- `CHANGELOG.md`

The DEB is the complete Runtime: App, UID 0 daemon, updater and launchd jobs. The
IPA contains the foreground App only and requires the matching Runtime to be
installed separately for privileged features.

## Credential distribution boundary

Current RootTools App, daemon and updater binaries contain personalized
credentials. `release-local.sh` allows binary upload only to the dedicated
private artifact repository, or as a draft in the public source repository. A
private candidate also must use a named credential profile different from
`installed`. The manifest records profile name and fingerprints, not raw tokens.

Before upload, prepare the secret-free credential migration state from the exact
candidate and rollback artifacts:

```bash
python3 Scripts/prepare-credential-migration.py \
  --to-profile candidate-v0.23.0-2 \
  --deb build/packages/roottools_0.23.0-2_iphoneos-arm64.deb \
  --ipa build/packages/RootTools_0.23.0-2.ipa \
  --rollback-deb build/packages/roottools_0.23.0-1_iphoneos-arm64.deb
```

Run `Scripts/validate-simulator.py` for the repeatable offline App/fixture gate.
Neither receipt authorizes publishing a non-draft release while physical gates
remain pending.

## Rationale

Xcode/iOS builds consume macOS runner minutes heavily. A local build uses the
same physical development environment used for device validation and avoids
unnecessary CI cost.
