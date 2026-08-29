#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/Scripts/credential-files.sh"

mkdir -p build/tests

TOKEN_FILE="$ROOT/.roottools-token"
AGENT_TOKEN_FILE="$ROOT/.roottools-agent-token"
TOKEN="$(roottools_read_or_create_token "$TOKEN_FILE" "Owner token")"
AGENT_TOKEN="$(roottools_read_or_create_token "$AGENT_TOKEN_FILE" "Agent token")"

bash Tests/credential_files_test.sh
bash Tests/repository_boundary_test.sh

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
clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  Tests/update_controller_test.c Daemon/provider_registry.c Daemon/package_controller.c Daemon/update_controller.c \
  -lsqlite3 -framework CoreFoundation -lz -o build/tests/update_controller_test
build/tests/update_controller_test
clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  Tests/runtime_observer_test.c Daemon/provider_registry.c Daemon/runtime_observer.c \
  -o build/tests/runtime_observer_test
build/tests/runtime_observer_test
clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  Tests/remote_worker_controller_test.c Daemon/remote_worker_controller.c \
  -framework CoreFoundation -o build/tests/remote_worker_controller_test
build/tests/remote_worker_controller_test
clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  Tests/remote_access_controller_test.c Daemon/remote_access_controller.c Daemon/principal_store.c \
  -lsqlite3 -framework CoreFoundation -o build/tests/remote_access_controller_test
build/tests/remote_access_controller_test
clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  Tests/principal_store_test.c Daemon/principal_store.c \
  -lsqlite3 -framework CoreFoundation -o build/tests/principal_store_test
build/tests/principal_store_test
python3 Tests/package_builder_test.py
build/tests/control_plane_test --catalog | python3 -c '
import json, sys
catalog=json.load(sys.stdin)
assert catalog["schemaVersion"] == 1
by_id={item["id"]: item for item in catalog["capabilities"]}
assert by_id["device.app.launch"]["risk"] == "R1"
assert by_id["device.process.terminate"]["requiresConfirmation"] is True
assert by_id["device.remote-worker.observe"]["risk"] == "R0"
assert by_id["device.remote-worker.configure"]["risk"] == "R2"
assert by_id["device.remote-worker.configure"]["requiresConfirmation"] is True
assert by_id["device.raw-shell"]["enabled"] is False
assert catalog["invariants"] == {"r3Exposed": False, "rawPrivilegedShellExposed": False}
'

sed -e "s/__ROOTTOOLS_TOKEN__/$TOKEN/g" \
    -e "s/__ROOTTOOLS_AGENT_TOKEN__/$AGENT_TOKEN/g" \
    Daemon/roottools_execd.c > build/tests/roottools_execd_mac.c
sed -e "s/__ROOTTOOLS_TOKEN__/$TOKEN/g" \
    Daemon/roottools_updater.c > build/tests/roottools_updater_mac.c
clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  build/tests/roottools_execd_mac.c Daemon/control_plane.c Daemon/provider_registry.c Daemon/package_controller.c Daemon/update_controller.c Daemon/runtime_observer.c Daemon/remote_worker_controller.c Daemon/remote_access_controller.c Daemon/principal_store.c \
  -lsqlite3 -lz -framework CoreFoundation -o build/tests/roottools-execd-mac
python3 Tests/http_contract_test.py \
  --daemon build/tests/roottools-execd-mac \
  --admin-token "$TOKEN" \
  --agent-token "$AGENT_TOKEN"
clang -std=c11 -Wall -Wextra -Werror -I Daemon \
  build/tests/roottools_updater_mac.c Daemon/provider_registry.c Daemon/package_controller.c Daemon/update_controller.c \
  -lsqlite3 -lz -framework CoreFoundation -o build/tests/roottools-updater-mac
ROOTTOOLS_UPDATE_DB="$(mktemp /tmp/roottools-updater-test.XXXXXX)" build/tests/roottools-updater-mac

python3 -m py_compile \
  Scripts/usbmux_proxy.py \
  Scripts/device_service.py \
  Scripts/device_doctor.py \
  Scripts/root_exec.py \
  Scripts/verify-device.py \
  Scripts/migrate-v09-updater-path.py \
  Scripts/package-rootless-deb.py
bash -n Scripts/*.sh Tests/*.sh

echo "RootTools contract tests: PASS"
