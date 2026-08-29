#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/Scripts/credential-files.sh"

TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/roottools-credentials.XXXXXX")"
trap 'rm -rf "$TEST_ROOT"' EXIT

file_mode() {
  if stat -f '%Lp' "$1" >/dev/null 2>&1; then
    stat -f '%Lp' "$1"
  else
    stat -c '%a' "$1"
  fi
}

NEW_TOKEN="$TEST_ROOT/new-token"
NEW_VALUE="$(roottools_read_or_create_token "$NEW_TOKEN" "test token")"
[[ "$NEW_VALUE" =~ ^[[:xdigit:]]{48}$ ]]
[[ "$(file_mode "$NEW_TOKEN")" == "600" ]]

EXISTING_TOKEN="$TEST_ROOT/existing-token"
printf '%048d\n' 0 > "$EXISTING_TOKEN"
chmod 644 "$EXISTING_TOKEN"
EXISTING_VALUE="$(roottools_read_or_create_token "$EXISTING_TOKEN" "existing token")"
[[ "$EXISTING_VALUE" == "000000000000000000000000000000000000000000000000" ]]
[[ "$(file_mode "$EXISTING_TOKEN")" == "600" ]]

MALFORMED_TOKEN="$TEST_ROOT/malformed-token"
printf 'not-a-token\n' > "$MALFORMED_TOKEN"
if roottools_read_or_create_token "$MALFORMED_TOKEN" "malformed token" >/dev/null 2>&1; then
  echo "malformed credentials must be rejected" >&2
  exit 1
fi
[[ "$(file_mode "$MALFORMED_TOKEN")" == "600" ]]

EXTRA_CONTENT_TOKEN="$TEST_ROOT/extra-content-token"
printf '%048d\n\n' 0 > "$EXTRA_CONTENT_TOKEN"
if roottools_read_or_create_token "$EXTRA_CONTENT_TOKEN" "extra-content token" >/dev/null 2>&1; then
  echo "credentials with extra content must be rejected" >&2
  exit 1
fi

DIRECTORY_TOKEN="$TEST_ROOT/directory-token"
mkdir "$DIRECTORY_TOKEN"
if roottools_read_or_create_token "$DIRECTORY_TOKEN" "directory token" >/dev/null 2>&1; then
  echo "non-regular credential files must be rejected" >&2
  exit 1
fi

SYMLINK_TARGET="$TEST_ROOT/symlink-target"
SYMLINK_TOKEN="$TEST_ROOT/symlink-token"
printf '%048d\n' 0 > "$SYMLINK_TARGET"
ln -s "$SYMLINK_TARGET" "$SYMLINK_TOKEN"
if roottools_read_or_create_token "$SYMLINK_TOKEN" "symlink token" >/dev/null 2>&1; then
  echo "symbolic-link credentials must be rejected" >&2
  exit 1
fi

if command -v zsh >/dev/null 2>&1; then
  zsh -c 'source "$1"; roottools_read_or_create_token "$2" "zsh token" >/dev/null' \
    zsh-test "$ROOT/Scripts/credential-files.sh" "$NEW_TOKEN"
fi

echo "Credential file tests: PASS"
