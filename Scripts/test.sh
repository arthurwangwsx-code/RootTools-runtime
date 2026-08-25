#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

mkdir -p build/tests

TOKEN_FILE="$ROOT/.roottools-token"
AGENT_TOKEN_FILE="$ROOT/.roottools-agent-token"
if [[ ! -f "$TOKEN_FILE" ]]; then openssl rand -hex 24 > "$TOKEN_FILE"; fi
if [[ ! -f "$AGENT_TOKEN_FILE" ]]; then openssl rand -hex 24 > "$AGENT_TOKEN_FILE"; fi
TOKEN="$(cat "$TOKEN_FILE")"
AGENT_TOKEN="$(cat "$AGENT_TOKEN_FILE")"

clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  Tests/control_plane_test.c Daemon/control_plane.c \
  -o build/tests/control_plane_test
build/tests/control_plane_test
clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  Tests/provider_registry_test.c Daemon/provider_registry.c \
  -o build/tests/provider_registry_test
build/tests/provider_registry_test
clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  Tests/package_controller_test.c Daemon/provider_registry.c Daemon/package_controller.c \
  -lsqlite3 -lz -framework CoreFoundation -o build/tests/package_controller_test
build/tests/package_controller_test
build/tests/control_plane_test --catalog | python3 -c '
import json, sys
catalog=json.load(sys.stdin)
assert catalog["schemaVersion"] == 1
by_id={item["id"]: item for item in catalog["capabilities"]}
assert by_id["device.app.launch"]["risk"] == "R1"
assert by_id["device.process.terminate"]["requiresConfirmation"] is True
assert by_id["device.raw-shell"]["enabled"] is False
assert catalog["invariants"] == {"r3Exposed": False, "rawPrivilegedShellExposed": False}
'

sed -e "s/__ROOTTOOLS_TOKEN__/$TOKEN/g" \
    -e "s/__ROOTTOOLS_AGENT_TOKEN__/$AGENT_TOKEN/g" \
    Daemon/roottools_execd.c > build/tests/roottools_execd_mac.c
clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  build/tests/roottools_execd_mac.c Daemon/control_plane.c Daemon/provider_registry.c Daemon/package_controller.c \
  -lsqlite3 -lz -framework CoreFoundation -o build/tests/roottools-execd-mac
python3 Tests/http_contract_test.py \
  --daemon build/tests/roottools-execd-mac \
  --admin-token "$TOKEN" \
  --agent-token "$AGENT_TOKEN"

python3 -m py_compile \
  Scripts/usbmux_proxy.py \
  Scripts/device_service.py \
  Scripts/device_doctor.py \
  Scripts/root_exec.py \
  Scripts/verify-device.py \
  Scripts/package-rootless-deb.py
bash -n Scripts/build.sh Scripts/install-jailbreak.sh

echo "RootTools contract tests: PASS"
