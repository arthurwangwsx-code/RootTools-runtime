#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  Scripts/release-local.sh <version> [--skip-build] [--draft]

Examples:
  Scripts/release-local.sh 0.23.0-1
  Scripts/release-local.sh 0.22.0-3 --skip-build

The version is both the Debian package version and the GitHub release tag
without the leading "v". Full tests/builds run on this Mac by default. GitHub
only hosts the already-built artifacts; no macOS Actions runner is required.
EOF
}

VERSION="${1:-}"
[[ -n "$VERSION" ]] || { usage; exit 64; }
shift || true

SKIP_BUILD=0
DRAFT=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1 ;;
    --draft) DRAFT=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage; exit 64 ;;
  esac
  shift
done

if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+-[0-9]+$ ]]; then
  echo "version must use Debian form X.Y.Z-N (example: 0.23.0-1)" >&2
  exit 64
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TAG="v$VERSION"
DEB="build/packages/roottools_${VERSION}_iphoneos-arm64.deb"
RELEASE_DIR="build/release/$TAG"
CHECKSUMS="$RELEASE_DIR/SHA256SUMS"
NOTES="$RELEASE_DIR/RELEASE_NOTES.md"
REPO="${ROOTTOOLS_RELEASE_REPO:-$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null || true)}"

echo "== RootTools Runtime local release $TAG =="

if [[ -n "$(git status --porcelain)" ]]; then
  echo "working tree must be clean before release" >&2
  git status --short >&2
  exit 2
fi

git diff --check
command -v gh >/dev/null || { echo "gh CLI is required" >&2; exit 3; }
gh auth status >/dev/null
[[ -n "$REPO" ]] || { echo "unable to resolve GitHub repository" >&2; exit 3; }
[[ "$(git branch --show-current)" == "main" ]] || {
  echo "releases must be created from main" >&2
  exit 3
}

if [[ "$SKIP_BUILD" -eq 0 ]]; then
  bash Scripts/test.sh
  bash Scripts/build.sh
  python3 Scripts/package-rootless-deb.py --version "$VERSION" --output "$DEB"
else
  [[ -f "$DEB" ]] || { echo "missing prebuilt artifact: $DEB" >&2; exit 4; }
fi

mkdir -p "$RELEASE_DIR"
if command -v shasum >/dev/null; then
  (cd "$(dirname "$DEB")" && shasum -a 256 "$(basename "$DEB")") > "$CHECKSUMS"
else
  (cd "$(dirname "$DEB")" && sha256sum "$(basename "$DEB")") > "$CHECKSUMS"
fi

cat > "$NOTES" <<EOF
# RootTools Runtime $TAG

RootTools Runtime for iOS 16+ Dopamine/rootless jailbreak devices.

## Assets

- \`$(basename "$DEB")\` — rootless Debian package
- \`SHA256SUMS\` — SHA-256 integrity checksum
- \`INSTALL.md\` — installation and upgrade guide
- \`CHANGELOG.md\` — milestone changes

## Important behavior

- RootTools remains a typed privileged runtime, not a raw root shell.
- Remote Access is opt-in and restricted to a selected Named Host Principal over a Tailscale address.
- Remote sessions expire automatically and do not expose the Owner token.
- UI automation waits for an unlocked visible device; passcode bypass is not implemented.

See \`CHANGELOG.md\` and \`docs/handoff/CURRENT_STATE.md\` for the current capability boundary.
EOF

if git rev-parse "$TAG" >/dev/null 2>&1; then
  CURRENT_TAG_COMMIT="$(git rev-list -n 1 "$TAG")"
  HEAD_COMMIT="$(git rev-parse HEAD)"
  [[ "$CURRENT_TAG_COMMIT" == "$HEAD_COMMIT" ]] || {
    echo "$TAG already exists on a different commit" >&2
    exit 5
  }
else
  git tag -a "$TAG" -m "RootTools Runtime $TAG"
fi

git push origin main
git push origin "$TAG"

RELEASE_ARGS=(
  "$TAG"
  "$DEB"
  "$CHECKSUMS"
  "INSTALL.md"
  "CHANGELOG.md"
  --repo "$REPO"
  --title "RootTools Runtime $TAG"
  --notes-file "$NOTES"
  --verify-tag
)
[[ "$DRAFT" -eq 1 ]] && RELEASE_ARGS+=(--draft)

if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  gh release upload "$TAG" "$DEB" "$CHECKSUMS" "INSTALL.md" "CHANGELOG.md" \
    --repo "$REPO" --clobber
  gh release edit "$TAG" --repo "$REPO" \
    --title "RootTools Runtime $TAG" --notes-file "$NOTES"
else
  gh release create "${RELEASE_ARGS[@]}"
fi

echo
echo "Release published:"
gh release view "$TAG" --repo "$REPO" --json url --jq .url
echo
cat "$CHECKSUMS"
