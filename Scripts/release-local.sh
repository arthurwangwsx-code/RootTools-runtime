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
source "$ROOT/Scripts/repository-boundary.sh"
source "$ROOT/Scripts/version.sh"

CANONICAL_VERSION="$(roottools_package_version "$ROOT/VERSION")"
if [[ "$VERSION" != "$CANONICAL_VERSION" ]]; then
  echo "release version must match VERSION ($CANONICAL_VERSION), got: $VERSION" >&2
  exit 64
fi

TAG="v$VERSION"
DEB="build/packages/roottools_${VERSION}_iphoneos-arm64.deb"
IPA="build/packages/RootTools_${VERSION}.ipa"
RELEASE_DIR="build/release/$TAG"
CHECKSUMS="$RELEASE_DIR/SHA256SUMS"
MANIFEST="$RELEASE_DIR/SOURCE_BUILD_MANIFEST.json"
NOTES="$RELEASE_DIR/RELEASE_NOTES.md"
REPO="${ROOTTOOLS_RELEASE_REPO:-$ROOTTOOLS_CANONICAL_REPO}"

echo "== RootTools Runtime local release $TAG =="

if [[ -n "$(git status --porcelain)" ]]; then
  echo "working tree must be clean before release" >&2
  git status --short >&2
  exit 2
fi

git diff --check
roottools_require_canonical_release_checkout "$ROOT" "$REPO"
command -v gh >/dev/null || { echo "gh CLI is required" >&2; exit 3; }
gh auth status >/dev/null

REPO_VISIBILITY="$(gh repo view "$REPO" --json visibility --jq .visibility)"
if [[ "$REPO_VISIBILITY" == "PUBLIC" && "$DRAFT" -ne 1 ]]; then
  echo "personalized RootTools binaries embed device credentials and must not be published from a public repository" >&2
  echo "use --draft for maintainer-only GitHub storage, or publish from an explicitly private release repository" >&2
  exit 6
fi

SOURCE_HEAD="$(git rev-parse HEAD)"
SOURCE_TREE="$(git rev-parse HEAD^{tree})"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git show -s --format=%ct HEAD)}"
export SOURCE_DATE_EPOCH

if [[ "$SKIP_BUILD" -eq 0 ]]; then
  bash Scripts/test.sh
  bash Scripts/build.sh
  python3 Scripts/package-rootless-deb.py --version "$VERSION" --output "$DEB"
  python3 Scripts/package-ipa.py --version "$VERSION" --output "$IPA"
else
  [[ -f "$DEB" ]] || { echo "missing prebuilt artifact: $DEB" >&2; exit 4; }
  [[ -f "$IPA" ]] || { echo "missing prebuilt artifact: $IPA" >&2; exit 4; }
fi

[[ "$(git rev-parse HEAD)" == "$SOURCE_HEAD" ]] || { echo "source HEAD changed during release build" >&2; exit 7; }
[[ "$(git rev-parse HEAD^{tree})" == "$SOURCE_TREE" ]] || { echo "source tree changed during release build" >&2; exit 7; }
if [[ -n "$(git status --porcelain)" ]]; then
  echo "tracked source changed during release build" >&2
  git status --short >&2
  exit 7
fi

mkdir -p "$RELEASE_DIR"
if command -v shasum >/dev/null; then
  (cd "$(dirname "$DEB")" && shasum -a 256 "$(basename "$DEB")" "$(basename "$IPA")") > "$CHECKSUMS"
else
  (cd "$(dirname "$DEB")" && sha256sum "$(basename "$DEB")" "$(basename "$IPA")") > "$CHECKSUMS"
fi

python3 - "$ROOT" "$REPO" "$VERSION" "$TAG" "$DEB" "$IPA" "$MANIFEST" "$SOURCE_HEAD" "$SOURCE_TREE" "$SOURCE_DATE_EPOCH" <<'PY'
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import sys

root, repository, version, tag, deb, ipa, manifest, commit, tree, source_date_epoch = sys.argv[1:]
root_path = Path(root)
app = root_path / "build/DerivedData/Build/Products/Release-iphoneos/RootTools.app"
with (app / "Info.plist").open("rb") as stream:
    info = plistlib.load(stream)

def artifact(path_value: str) -> dict:
    path = Path(path_value)
    return {
        "name": path.name,
        "bytes": path.stat().st_size,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }

payload = {
    "schemaVersion": 1,
    "product": "RootTools Runtime",
    "repository": repository,
    "version": version,
    "tag": tag,
    "source": {
        "commit": commit,
        "tree": tree,
        "sourceDateEpoch": int(source_date_epoch),
    },
    "build": {
        "xcode": subprocess.check_output(["xcodebuild", "-version"], text=True).strip().splitlines(),
        "iphoneosSdk": subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-version"], text=True).strip(),
        "configuration": "Release",
        "platform": "iphoneos",
        "architecture": "arm64",
    },
    "app": {
        "bundleIdentifier": info.get("CFBundleIdentifier"),
        "shortVersion": info.get("CFBundleShortVersionString"),
        "buildVersion": info.get("CFBundleVersion"),
    },
    "artifacts": [artifact(deb), artifact(ipa)],
    "distribution": {
        "credentialModel": "personalized build",
        "requiredGitHubVisibility": "draft or private",
    },
}
Path(manifest).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
PY

cat > "$NOTES" <<EOF
# RootTools Runtime $TAG

RootTools Runtime for iOS 16+ Dopamine/rootless jailbreak devices.

## Assets

- \`$(basename "$DEB")\` — rootless Debian package
- \`$(basename "$IPA")\` — TrollStore-compatible foreground App (requires the matching rootless runtime for full capability)
- \`SHA256SUMS\` — SHA-256 integrity checksums
- \`SOURCE_BUILD_MANIFEST.json\` — exact source, toolchain and artifact identity
- \`INSTALL.md\` — installation and upgrade guide
- \`CHANGELOG.md\` — milestone changes

## Important behavior

- RootTools remains a typed privileged runtime, not a raw root shell.
- Remote Access is opt-in and restricted to a selected Named Host Principal over a Tailscale address.
- Remote sessions expire automatically and do not expose the Owner token.
- UI automation waits for an unlocked visible device; passcode bypass is not implemented.
- This personalized build contains device credentials and is stored as a maintainer-only draft on the public source repository.

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
  "$IPA"
  "$CHECKSUMS"
  "$MANIFEST"
  "INSTALL.md"
  "CHANGELOG.md"
  --repo "$REPO"
  --title "RootTools Runtime $TAG"
  --notes-file "$NOTES"
  --verify-tag
)
[[ "$DRAFT" -eq 1 ]] && RELEASE_ARGS+=(--draft)

if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  gh release upload "$TAG" "$DEB" "$IPA" "$CHECKSUMS" "$MANIFEST" "INSTALL.md" "CHANGELOG.md" \
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
