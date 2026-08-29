#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/Scripts/version.sh"

PACKAGE_VERSION="$(roottools_package_version "$ROOT/VERSION")"
DAEMON_VERSION="$(roottools_daemon_version "$PACKAGE_VERSION")"
[[ "$PACKAGE_VERSION" == "0.23.0-1" ]]
[[ "$DAEMON_VERSION" == "0.23.0" ]]

INVALID_VERSION_FILE="$(mktemp "${TMPDIR:-/tmp}/roottools-invalid-version.XXXXXX")"
trap 'rm -f "$INVALID_VERSION_FILE"' EXIT
printf '0.23\n' > "$INVALID_VERSION_FILE"
if roottools_package_version "$INVALID_VERSION_FILE" >/dev/null 2>&1; then
  echo "invalid package versions must be rejected" >&2
  exit 1
fi

python3 - "$ROOT" "$PACKAGE_VERSION" "$DAEMON_VERSION" <<'PY'
import importlib.util
from pathlib import Path
import sys

root = Path(sys.argv[1])
package_version = sys.argv[2]
daemon_version = sys.argv[3]
spec = importlib.util.spec_from_file_location("package_rootless_deb", root / "Scripts/package-rootless-deb.py")
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.DEFAULT_VERSION == package_version
assert module.daemon_version_from_package(package_version) == daemon_version

verify_spec = importlib.util.spec_from_file_location("verify_device", root / "Scripts/verify-device.py")
assert verify_spec is not None and verify_spec.loader is not None
verify_module = importlib.util.module_from_spec(verify_spec)
sys.path.insert(0, str(root / "Scripts"))
verify_spec.loader.exec_module(verify_module)
assert verify_module.DEFAULT_EXPECTED_DAEMON_VERSION == daemon_version
PY

echo "Version consistency tests: PASS"
