#!/usr/bin/env python3
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import json
from pathlib import Path
import stat
import sys
import tempfile
import uuid


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "Scripts"))
SPEC = importlib.util.spec_from_file_location(
    "qualify_remote_access", ROOT / "Scripts/qualify-remote-access.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def receipt(capability_id: str, output: str = "") -> dict:
    return {
        "capabilityId": capability_id,
        "ok": True,
        "executed": True,
        "result": "success",
        "output": output,
        "requestId": str(uuid.uuid4()),
        "auditId": str(uuid.uuid4()),
    }


class FakeAdmin:
    def __init__(self) -> None:
        self.enabled = False
        self.principal_id = ""
        self.other_principal_id = ""
        self.revoked = False
        self.grants: set[str] = set()
        self.host = "100.88.77.66"
        self.port = 45822

    def status(self) -> dict:
        return {
            "daemonVersion": "0.23.0",
            "packageVersion": "0.23.0-2",
            "uid": 0,
            "jailbreakRootless": True,
        }

    def principals(self) -> dict:
        return {"principals": []}

    def create_principal(self, principal_id: str, kind: str, display_name: str, confirmed: bool) -> dict:
        assert kind == "host" and confirmed and display_name
        if not self.principal_id:
            self.principal_id = principal_id
            token = "a" * 48
        else:
            self.other_principal_id = principal_id
            token = "d" * 48
        self.revoked = False
        return receipt("device.principal.create", token)

    def grant_principal(self, principal_id: str, capability_id: str, confirmed: bool, expires_at: int) -> dict:
        assert principal_id == self.principal_id and confirmed and expires_at > 0
        self.grants.add(capability_id)
        return receipt("device.principal.grant")

    def principal_grants(self, principal_id: str) -> dict:
        assert principal_id == self.principal_id
        return {"grants": [{"capabilityId": item, "active": True} for item in sorted(self.grants)]}

    def configure_remote_access(
        self, *, enabled: bool, principal_id: str, duration_minutes: int, confirmed: bool
    ) -> dict:
        assert confirmed
        if enabled:
            assert principal_id == self.principal_id and duration_minutes >= 5 and not self.revoked
            self.enabled = True
        else:
            self.enabled = False
        return receipt("device.remote-access.configure")

    def remote_access(self) -> dict:
        return {
            "enabled": self.enabled,
            "principalId": self.principal_id if self.enabled else "",
            "expiresAt": 4102444800,
            "transport": {
                "kind": "tailscale",
                "available": True,
                "bindAddress": self.host,
                "port": self.port,
                "listenerActive": self.enabled,
            },
            "policy": {
                "publicInternetListener": False,
                "ownerTokenAcceptedRemotely": False,
                "legacyAgentTokenAcceptedRemotely": False,
            },
        }

    def revoke_principal(self, principal_id: str, confirmed: bool) -> dict:
        assert principal_id in {self.principal_id, self.other_principal_id} and confirmed
        if principal_id == self.principal_id:
            self.revoked = True
            self.enabled = False
        return receipt("device.principal.revoke")


class FakePrincipal:
    def __init__(self, admin: FakeAdmin) -> None:
        self.admin = admin

    def status(self) -> dict:
        if not self.admin.enabled or self.admin.revoked:
            raise MODULE.DeviceServiceError("HTTP 403: closed", status_code=403)
        return self.admin.status()

    def performance(self) -> dict:
        return {"schemaVersion": 1, "loadAverage": [0.1, 0.2, 0.3]}

    def action(self, capability_id: str, parameters: dict) -> dict:
        assert capability_id == "device.app.launch"
        assert parameters == {"bundleID": "com.arthur.roottools.ios"}
        return receipt(capability_id)


class FakeRejected:
    def hello(self) -> dict:
        raise MODULE.DeviceServiceError("HTTP 403: remote_session_principal_required", status_code=403)


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary_name:
        temporary = Path(temporary_name)
        admin_token_file = temporary / "owner-token"
        agent_token_file = temporary / "agent-token"
        principal_token_file = temporary / "principal-token"
        other_principal_token_file = temporary / "other-principal-token"
        state_file = temporary / "state.json"
        admin_token_file.write_text("b" * 48 + "\n")
        agent_token_file.write_text("c" * 48 + "\n")
        admin_token_file.chmod(0o600)
        agent_token_file.chmod(0o600)

        admin = FakeAdmin()

        @contextlib.contextmanager
        def fake_usb_admin_client(udid: str | None, admin_token: str):
            assert admin_token == "b" * 48
            yield udid or "fixture-udid", admin

        def fake_direct_client(host: str, port: int, token: str, caller: str):
            assert host == admin.host and port == admin.port and caller
            if token == "a" * 48:
                return FakePrincipal(admin)
            return FakeRejected()

        MODULE.usb_admin_client = fake_usb_admin_client
        MODULE.direct_client = fake_direct_client
        MODULE.assert_no_usb_iphone = lambda: None

        prepare_args = argparse.Namespace(
            udid="fixture-udid",
            admin_token_file=admin_token_file,
            agent_token_file=agent_token_file,
            principal_id="host:qualification-test",
            other_principal_id="host:qualification-test-negative",
            principal_token_file=principal_token_file,
            other_principal_token_file=other_principal_token_file,
            duration_minutes=30,
            credential_profile="installed",
            expected_daemon_version="0.23.0",
            expected_package_version="0.23.0-2",
            state_file=state_file,
        )
        prepared = MODULE.prepare(prepare_args)
        assert prepared["phase"] == "prepared"
        assert len(prepared["receipts"]) == 6
        assert admin.grants == set(MODULE.MINIMUM_GRANTS)
        assert stat.S_IMODE(principal_token_file.stat().st_mode) == 0o600
        assert stat.S_IMODE(other_principal_token_file.stat().st_mode) == 0o600
        assert stat.S_IMODE(state_file.stat().st_mode) == 0o600
        state_text = state_file.read_text()
        assert json.loads(state_text)["principalId"] == "host:qualification-test"
        assert "a" * 48 not in state_text and "d" * 48 not in state_text

        verify_args = argparse.Namespace(
            state_file=state_file,
            admin_token_file=admin_token_file,
            agent_token_file=agent_token_file,
        )
        verified = MODULE.verify_remote(verify_args)
        assert verified["phase"] == "remote-verified"
        assert verified["r1Capability"] == "device.app.launch"
        assert len(verified["receipts"]) == 7

        cleanup_args = argparse.Namespace(
            state_file=state_file,
            admin_token_file=admin_token_file,
            udid="fixture-udid",
            verify_expiry=False,
        )
        completed = MODULE.cleanup(cleanup_args)
        assert completed["phase"] == "complete"
        assert completed["stopVerifiedAt"] > 0
        assert completed["revokeVerifiedAt"] > 0
        assert len(completed["receipts"]) == 11
        assert admin.revoked is True and admin.enabled is False

        try:
            MODULE.write_secret_new(principal_token_file, "d" * 48)
        except FileExistsError:
            pass
        else:
            raise AssertionError("principal token overwrite was not rejected")

    print("remote_qualification_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
