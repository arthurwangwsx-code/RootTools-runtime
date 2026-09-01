#!/usr/bin/env bash

# Validate and read an existing RootTools credential. The token is printed to
# stdout; diagnostics are written to stderr.
roottools_read_token() {
  if [[ "$#" -ne 2 ]]; then
    echo "usage: roottools_read_token <path> <label>" >&2
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
    echo "$label is missing: $credential_path" >&2
    return 66
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

# Read an existing RootTools credential or create one with owner-only access.
roottools_read_or_create_token() {
  if [[ "$#" -ne 2 ]]; then
    echo "usage: roottools_read_or_create_token <path> <label>" >&2
    return 64
  fi

  local credential_path="$1"
  local label="$2"
  if [[ ! -e "$credential_path" && ! -L "$credential_path" ]]; then
    (umask 077 && openssl rand -hex 24 > "$credential_path")
  fi
  roottools_read_token "$credential_path" "$label"
}

roottools_validate_credential_profile() {
  if [[ "$#" -ne 1 ]]; then
    echo "usage: roottools_validate_credential_profile <profile>" >&2
    return 64
  fi
  local profile="$1"
  if [[ ! "$profile" =~ ^[a-z0-9][a-z0-9._-]{0,63}$ || "$profile" == *".."* ]]; then
    echo "credential profile must use 1-64 lowercase letters, digits, dot, underscore or hyphen: $profile" >&2
    return 65
  fi
}

roottools_prepare_credential_profile() {
  if [[ "$#" -ne 2 ]]; then
    echo "usage: roottools_prepare_credential_profile <root> <profile>" >&2
    return 64
  fi
  local root="$1"
  local profile="$2"
  roottools_validate_credential_profile "$profile" || return
  [[ "$profile" == "installed" ]] && return 0

  local base="$root/.roottools-credentials"
  local profile_dir="$base/$profile"
  for directory in "$base" "$profile_dir"; do
    if [[ -L "$directory" ]]; then
      echo "credential profile directory must not be a symbolic link: $directory" >&2
      return 65
    fi
    if [[ -e "$directory" && ! -d "$directory" ]]; then
      echo "credential profile path must be a directory: $directory" >&2
      return 65
    fi
    if [[ ! -e "$directory" ]]; then
      (umask 077 && mkdir "$directory")
    fi
    chmod 700 "$directory"
  done
}

roottools_owner_token_file() {
  if [[ "$#" -ne 2 ]]; then
    echo "usage: roottools_owner_token_file <root> <profile>" >&2
    return 64
  fi
  local root="$1"
  local profile="$2"
  roottools_validate_credential_profile "$profile" || return
  if [[ -n "${ROOTTOOLS_OWNER_TOKEN_FILE:-}" ]]; then
    printf '%s\n' "$ROOTTOOLS_OWNER_TOKEN_FILE"
  elif [[ "$profile" == "installed" ]]; then
    printf '%s/.roottools-token\n' "$root"
  else
    printf '%s/.roottools-credentials/%s/owner-token\n' "$root" "$profile"
  fi
}

roottools_agent_token_file() {
  if [[ "$#" -ne 2 ]]; then
    echo "usage: roottools_agent_token_file <root> <profile>" >&2
    return 64
  fi
  local root="$1"
  local profile="$2"
  roottools_validate_credential_profile "$profile" || return
  if [[ -n "${ROOTTOOLS_AGENT_TOKEN_FILE:-}" ]]; then
    printf '%s\n' "$ROOTTOOLS_AGENT_TOKEN_FILE"
  elif [[ "$profile" == "installed" ]]; then
    printf '%s/.roottools-agent-token\n' "$root"
  else
    printf '%s/.roottools-credentials/%s/agent-token\n' "$root" "$profile"
  fi
}

roottools_token_fingerprint() {
  if [[ "$#" -ne 2 ]]; then
    echo "usage: roottools_token_fingerprint <path> <label>" >&2
    return 64
  fi
  local credential_path="$1"
  local label="$2"
  local value
  value="$(roottools_read_token "$credential_path" "$label")" || return
  if command -v shasum >/dev/null 2>&1; then
    printf '%s' "$value" | shasum -a 256 | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    printf '%s' "$value" | sha256sum | awk '{print $1}'
  else
    echo "SHA-256 tool is required for credential fingerprints" >&2
    return 69
  fi
}
