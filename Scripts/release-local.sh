#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-}"
if [[ -z "$VERSION" ]]; then
  echo "usage: $0 <version>"
  exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "== RootTools local release $VERSION =="

git diff --check
bash Scripts/test.sh
bash Scripts/build.sh
python3 Scripts/package-rootless-deb.py --version "$VERSION"

mkdir -p build/release
sha256sum build/packages/*.deb > build/release/SHA256SUMS

echo "Artifacts:"
ls -lh build/packages/*.deb build/release/SHA256SUMS

echo
echo "Create release:"
echo "gh release create v$VERSION build/packages/*.deb build/release/SHA256SUMS"
