#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/Scripts/repository-boundary.sh"

TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/roottools-repository-boundary.XXXXXX")"
trap 'rm -rf "$TEST_ROOT"' EXIT

make_checkout() {
  local path="$1"
  local origin_url="$2"

  git init -q "$path"
  git -C "$path" symbolic-ref HEAD refs/heads/main
  git -C "$path" config user.name "RootTools Test"
  git -C "$path" config user.email "roottools-test@example.invalid"
  git -C "$path" commit -q --allow-empty -m "test: initialize boundary fixture"
  git -C "$path" remote add origin "$origin_url"
  git -C "$path" update-ref refs/remotes/origin/main HEAD
  git -C "$path" branch --set-upstream-to=origin/main main >/dev/null
}

CANONICAL="$TEST_ROOT/canonical"
make_checkout "$CANONICAL" "https://github.com/arthurwangwsx-code/RootTools-runtime.git"
roottools_require_canonical_source_checkout "$CANONICAL"
roottools_require_canonical_release_checkout "$CANONICAL" "$ROOTTOOLS_CANONICAL_SOURCE_REPO"
roottools_require_canonical_release_checkout "$CANONICAL" "$ROOTTOOLS_PRIVATE_RELEASE_REPO"
roottools_require_release_target "$ROOTTOOLS_CANONICAL_SOURCE_REPO" PUBLIC 1
roottools_require_release_target "$ROOTTOOLS_PRIVATE_RELEASE_REPO" PRIVATE 0
roottools_require_release_target "$ROOTTOOLS_PRIVATE_RELEASE_REPO" PRIVATE 1

if roottools_require_release_target "$ROOTTOOLS_CANONICAL_SOURCE_REPO" PUBLIC 0 >/dev/null 2>&1; then
  echo "public source releases containing personalized binaries must be draft-only" >&2
  exit 1
fi
if roottools_require_release_target "$ROOTTOOLS_PRIVATE_RELEASE_REPO" PUBLIC 1 >/dev/null 2>&1; then
  echo "binary release repository must remain private" >&2
  exit 1
fi

if roottools_require_canonical_release_checkout "$CANONICAL" "arthurwangwsx-code/RootTools" >/dev/null 2>&1; then
  echo "non-canonical release repositories must be rejected" >&2
  exit 1
fi

LEGACY="$TEST_ROOT/legacy"
make_checkout "$LEGACY" "https://github.com/arthurwangwsx-code/RootTools.git"
if roottools_require_canonical_source_checkout "$LEGACY" >/dev/null 2>&1; then
  echo "legacy origins must be rejected" >&2
  exit 1
fi

git -C "$CANONICAL" switch -q -c release-test
if roottools_require_canonical_source_checkout "$CANONICAL" >/dev/null 2>&1; then
  echo "non-main release branches must be rejected" >&2
  exit 1
fi

git -C "$CANONICAL" switch -q main
git -C "$CANONICAL" config --unset branch.main.remote
git -C "$CANONICAL" config --unset branch.main.merge
if roottools_require_canonical_source_checkout "$CANONICAL" >/dev/null 2>&1; then
  echo "main without origin/main upstream must be rejected" >&2
  exit 1
fi

echo "Repository boundary tests: PASS"
