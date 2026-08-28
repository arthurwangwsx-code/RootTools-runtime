# RootTools Runtime Release Process

## Design

RootTools Runtime uses a local-first release model.

The maintainer Mac performs:

1. Contract tests
2. iOS Release build
3. Rootless DEB packaging
4. SHA256 generation
5. GitHub Release upload

GitHub Actions is only used for lightweight validation and release metadata.

## Local release

```bash
Scripts/release-local.sh 0.23.0-1
```

The script validates, builds, packages and prepares checksums.

## Upload

```bash
gh release create v0.23.0 \
  build/packages/*.deb \
  build/release/SHA256SUMS
```

## Rationale

Xcode/iOS builds consume macOS runner minutes heavily. A local build uses the
same physical development environment used for device validation and avoids
unnecessary CI cost.
