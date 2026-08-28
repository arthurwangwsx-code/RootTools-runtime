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
guide.

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
