# RootTools Runtime Release Process

## Design

RootTools Runtime uses a local-first release model.

The maintainer Mac performs:

1. Contract tests
2. iOS Release build
3. Rootless DEB packaging
4. SHA256 generation
5. Git tag creation
6. GitHub Release upload

GitHub Actions does not perform automatic builds. The remaining workflow is
manual-only and limited to lightweight repository hygiene so normal pushes do
not consume hosted runner minutes.

## Local release

```bash
Scripts/release-local.sh 0.23.0-1
```

The script requires a clean working tree, runs the full local validation/build,
packages the exact Debian version, generates checksums, creates/pushes the
annotated tag, and creates the GitHub Release with the DEB, checksum and install
guide. Before any build or push, it also verifies that `origin` is
`arthurwangwsx-code/RootTools-runtime` and that local `main` tracks
`origin/main`; the legacy mixed checkout is not a release source.

`VERSION` is the single release-version source in Debian form `X.Y.Z-N`.
The build derives daemon/App version `X.Y.Z` and App build `N` from it. Packaging
rejects a caller-supplied mismatch, stale build-version stamps, or an App plist
whose version does not match the candidate package.

DEB archive metadata uses `SOURCE_DATE_EPOCH`, defaulting to the timestamp of
the current Git commit. Repackaging the same committed inputs therefore
produces the same bytes and checksum. A caller may set `SOURCE_DATE_EPOCH`
explicitly for an equivalent controlled build environment.

When the exact artifact was already built and validated locally, publishing
can skip rebuilding:

```bash
Scripts/release-local.sh 0.23.0-1 --skip-build
```

## Result

The canonical tag/release format is `vX.Y.Z-N`, matching the Debian package
version. Release assets are:

- `roottools_X.Y.Z-N_iphoneos-arm64.deb`
- `SHA256SUMS`
- `INSTALL.md`
- `CHANGELOG.md`

The GitHub Releases page is the only public binary distribution source for
RootTools Runtime.

## Rationale

Xcode/iOS builds consume macOS runner minutes heavily. A local build uses the
same physical development environment used for device validation and avoids
unnecessary CI cost.
