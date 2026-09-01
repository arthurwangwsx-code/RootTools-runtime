#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/Scripts/credential-files.sh"

usage() {
  cat <<'EOF'
Usage:
  Scripts/credential-profile.sh create <profile>
  Scripts/credential-profile.sh inspect <profile>
  Scripts/credential-profile.sh compare <from-profile> <to-profile>

The reserved profile `installed` resolves to the legacy-compatible flat token
files. Other profiles are isolated under ignored, owner-only directories in
.roottools-credentials/. Commands emit fingerprints and paths, never tokens.
EOF
}

profile_json() {
  local profile="$1"
  local owner_file="$2"
  local agent_file="$3"
  local owner_fingerprint="$4"
  local agent_fingerprint="$5"
  python3 - "$profile" "$owner_file" "$agent_file" "$owner_fingerprint" "$agent_fingerprint" <<'PY'
import json
import sys

profile, owner_file, agent_file, owner_fingerprint, agent_fingerprint = sys.argv[1:]
print(json.dumps({
    "schemaVersion": 1,
    "profile": profile,
    "ownerTokenFile": owner_file,
    "agentTokenFile": agent_file,
    "ownerFingerprint": owner_fingerprint,
    "agentFingerprint": agent_fingerprint,
}, indent=2, sort_keys=True))
PY
}

inspect_profile() {
  local profile="$1"
  local owner_file
  local agent_file
  local owner_fingerprint
  local agent_fingerprint
  roottools_validate_credential_profile "$profile"
  owner_file="$(roottools_owner_token_file "$ROOT" "$profile")"
  agent_file="$(roottools_agent_token_file "$ROOT" "$profile")"
  owner_fingerprint="$(roottools_token_fingerprint "$owner_file" "$profile Owner token")"
  agent_fingerprint="$(roottools_token_fingerprint "$agent_file" "$profile Agent token")"
  profile_json "$profile" "$owner_file" "$agent_file" "$owner_fingerprint" "$agent_fingerprint"
}

COMMAND="${1:-}"
case "$COMMAND" in
  create)
    [[ "$#" -eq 2 ]] || { usage; exit 64; }
    PROFILE="$2"
    roottools_prepare_credential_profile "$ROOT" "$PROFILE"
    OWNER_FILE="$(roottools_owner_token_file "$ROOT" "$PROFILE")"
    AGENT_FILE="$(roottools_agent_token_file "$ROOT" "$PROFILE")"
    roottools_read_or_create_token "$OWNER_FILE" "$PROFILE Owner token" >/dev/null
    roottools_read_or_create_token "$AGENT_FILE" "$PROFILE Agent token" >/dev/null
    inspect_profile "$PROFILE"
    ;;
  inspect)
    [[ "$#" -eq 2 ]] || { usage; exit 64; }
    inspect_profile "$2"
    ;;
  compare)
    [[ "$#" -eq 3 ]] || { usage; exit 64; }
    FROM_JSON="$(inspect_profile "$2")"
    TO_JSON="$(inspect_profile "$3")"
    python3 - "$FROM_JSON" "$TO_JSON" <<'PY'
import json
import sys

source = json.loads(sys.argv[1])
target = json.loads(sys.argv[2])
if source["profile"] == target["profile"]:
    raise SystemExit("credential migration profiles must differ")
if source["ownerFingerprint"] == target["ownerFingerprint"]:
    raise SystemExit("Owner credentials must differ between migration profiles")
if source["agentFingerprint"] == target["agentFingerprint"]:
    raise SystemExit("Agent credentials must differ between migration profiles")
print(json.dumps({
    "schemaVersion": 1,
    "from": source,
    "to": target,
    "credentialsDiffer": True,
}, indent=2, sort_keys=True))
PY
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 64
    ;;
esac
