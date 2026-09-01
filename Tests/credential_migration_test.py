#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import plistlib
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "prepare_credential_migration", ROOT / "Scripts/prepare-credential-migration.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def main() -> int:
    source_owner = ("a" * 48).encode()
    source_agent = ("b" * 48).encode()
    target_owner = ("c" * 48).encode()
    target_agent = ("d" * 48).encode()

    scan = MODULE.scan_payload(
        {"app": b"prefix" + target_owner, "daemon": target_owner + target_agent},
        source_owner,
        source_agent,
        target_owner,
        target_agent,
    )
    assert scan["sourceCredentialAbsent"] is True
    assert scan["targetOwnerHitFiles"] == ["app", "daemon"]
    assert scan["targetAgentHitFiles"] == ["daemon"]

    with tempfile.TemporaryDirectory() as temporary_name:
        temporary = Path(temporary_name)
        ipa = temporary / "fixture.ipa"
        info = {
            "CFBundleIdentifier": "com.arthur.roottools.ios",
            "CFBundleShortVersionString": MODULE.VERSION.rsplit("-", 1)[0],
            "CFBundleVersion": MODULE.VERSION.rsplit("-", 1)[1],
        }
        with zipfile.ZipFile(ipa, "w") as archive:
            archive.writestr("Payload/RootTools.app/Info.plist", plistlib.dumps(info))
            archive.writestr("Payload/RootTools.app/RootTools", b"binary" + target_owner)
        inspected = MODULE.inspect_ipa(ipa, source_owner, source_agent, target_owner, target_agent)
        assert inspected["bundleIdentifier"] == "com.arthur.roottools.ios"
        assert inspected["credentialScan"]["sourceCredentialAbsent"] is True

        state = temporary / "state.json"
        payload = {"sourceFingerprint": "1" * 64, "targetFingerprint": "2" * 64}
        MODULE.write_state(state, payload)
        assert json.loads(state.read_text()) == payload
        assert state.stat().st_mode & 0o077 == 0
        state_text = state.read_text()
        assert source_owner.decode() not in state_text and target_owner.decode() not in state_text

    print("credential_migration_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
