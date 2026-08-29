#!/usr/bin/env bash

# Read an existing RootTools credential or create one with owner-only access.
# The token is printed to stdout; diagnostics are written to stderr.
roottools_read_or_create_token() {
  if [[ "$#" -ne 2 ]]; then
    echo "usage: roottools_read_or_create_token <path> <label>" >&2
    return 64
  fi

  local credential_path="$1"
  local label="$2"
  local value
  local byte_count

  if [[ -L "$credential_path" ]]; then
    echo "$label must not be a symbolic link: $credential_path" >&2
    return 65
  fi
  if [[ -e "$credential_path" && ! -f "$credential_path" ]]; then
    echo "$label must be a regular file: $credential_path" >&2
    return 65
  fi

  if [[ ! -e "$credential_path" ]]; then
    (umask 077 && openssl rand -hex 24 > "$credential_path")
  fi

  chmod 600 "$credential_path"
  value="$(cat "$credential_path")"
  byte_count="$(wc -c < "$credential_path" | tr -d '[:space:]')"
  if [[ ! "$value" =~ ^[[:xdigit:]]{48}$ ]]; then
    echo "$label must contain exactly 48 hexadecimal characters: $credential_path" >&2
    return 65
  fi
  if [[ "$byte_count" != "48" && "$byte_count" != "49" ]]; then
    echo "$label must contain only the token and one optional newline: $credential_path" >&2
    return 65
  fi

  printf '%s' "$value"
}
