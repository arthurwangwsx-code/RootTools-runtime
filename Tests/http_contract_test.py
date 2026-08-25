#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import plistlib
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import zipfile


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(port: int, token: str | None, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
    data = None if body is None else json.dumps(body, separators=(",", ":")).encode()
    headers: dict[str, str] = {}
    if token is not None:
        headers["X-RootTools-Token"] = token
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=2) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


def action(
    port: int,
    token: str,
    capability: str,
    *,
    confirmed: bool = False,
    parameters: dict | None = None,
    request_id: str | None = None,
    expected_revision: int | None = None,
) -> dict:
    body = {
        "requestId": request_id or str(uuid.uuid4()),
        "capabilityId": capability,
        "caller": "spoofed-caller",
        "confirmed": confirmed,
        "parameters": parameters or {},
    }
    if expected_revision is not None:
        body["expectedRevision"] = expected_revision
    status, payload = request(
        port,
        token,
        "POST",
        "/v1/commands/submit",
        body,
    )
    assert status == 200, (status, payload)
    return payload


def wait_ready(port: int, token: str) -> None:
    deadline = time.time() + 3
    while time.time() < deadline:
        try:
            status, _ = request(port, token, "GET", "/v1/status")
            if status == 200:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise AssertionError("test daemon did not become ready")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--daemon", type=Path, required=True)
    parser.add_argument("--admin-token", required=True)
    parser.add_argument("--agent-token", required=True)
    args = parser.parse_args()
    assert args.admin_token != args.agent_token

    with tempfile.TemporaryDirectory(prefix="roottools-http-") as temp:
        temp_path = Path(temp)
        policy_dir = temp_path / "policy"
        audit_path = temp_path / "audit.log"
        ledger_path = temp_path / "idempotency.sqlite3"
        runtime_agent_token_path = temp_path / "agent-token"
        tcc_path = temp_path / "TCC.db"
        mobile_scope = temp_path / "mobile-files"
        bootstrap_scope = temp_path / "bootstrap-files"
        package_root = temp_path / "packages"
        package_db = temp_path / "packages.sqlite3"
        update_db = temp_path / "self-update.sqlite3"
        port = free_port()
        tcc = sqlite3.connect(tcc_path)
        tcc.execute(
            "CREATE TABLE access(service TEXT, client TEXT, auth_value INTEGER, auth_reason INTEGER, last_modified INTEGER)"
        )
        tcc.execute(
            "INSERT INTO access(service,client,auth_value,auth_reason,last_modified) VALUES(?,?,?,?,?)",
            ("kTCCServiceCamera", "com.example.fixture", 2, 4, 123456),
        )
        tcc.commit()
        tcc.close()
        env = {
            **__import__("os").environ,
            "ROOTTOOLS_PORT": str(port),
            "ROOTTOOLS_POLICY_DIR": str(policy_dir),
            "ROOTTOOLS_AUDIT_PATH": str(audit_path),
            "ROOTTOOLS_LEDGER_PATH": str(ledger_path),
            "ROOTTOOLS_AGENT_TOKEN_PATH": str(runtime_agent_token_path),
            "ROOTTOOLS_TCC_DB": str(tcc_path),
            "ROOTTOOLS_TCC_FORCE_SNAPSHOT": "1",
            "ROOTTOOLS_TCC_SNAPSHOT_DIR": str(temp_path / "tcc-snapshot"),
            "ROOTTOOLS_MOBILE_SCOPE_ROOT": str(mobile_scope),
            "ROOTTOOLS_BOOTSTRAP_SCOPE_ROOT": str(bootstrap_scope),
            "ROOTTOOLS_PACKAGE_ROOT": str(package_root),
            "ROOTTOOLS_PACKAGE_DB": str(package_db),
            "ROOTTOOLS_UPDATE_DB": str(update_db),
            "ROOTTOOLS_TEST_LOCK_STATE": "locked",
            "ROOTTOOLS_TEST_SCREEN_BLANKED": "1",
        }
        daemon = subprocess.Popen([str(args.daemon)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            wait_ready(port, args.agent_token)

            status, _ = request(port, None, "GET", "/v1/status")
            assert status == 401
            status, _ = request(port, "incorrect", "GET", "/v1/status")
            assert status == 401

            status, hello = request(port, args.agent_token, "GET", "/v1/hello")
            assert status == 200
            assert hello["service"] == "roottools.device-service"
            assert hello["schemaVersion"] == 1
            assert hello["authenticatedRole"] == "agent"
            assert hello["features"]["commandGateway"] is True
            assert hello["features"]["durableIdempotency"] is True
            assert hello["features"]["expectedRevision"] is True
            assert hello["features"]["rawPrivilegedShell"] is False
            assert hello["features"]["providerRegistry"] is True
            assert hello["features"]["packageProviderPlanning"] is True
            assert hello["features"]["packageController"] is True
            assert hello["features"]["packageLifecycle"] is True
            assert hello["features"]["selfUpdater"] is True
            assert hello["features"]["runtimeSemanticObservation"] is True
            assert hello["features"]["packageChunkBytes"] == 262144
            assert hello["revisionAvailable"] is True
            initial_revision = hello["revision"]
            status, owner_hello = request(port, args.admin_token, "GET", "/v1/hello")
            assert status == 200
            assert owner_hello["authenticatedRole"] == "owner"

            status, catalog = request(port, args.agent_token, "GET", "/v1/capabilities/catalog")
            assert status == 200
            by_id = {item["id"]: item for item in catalog["capabilities"]}
            assert by_id["device.app.launch"]["hardEnabled"] is True
            assert by_id["device.app.launch"]["enabled"] is True
            assert by_id["device.raw-shell"]["hardEnabled"] is False
            assert by_id["device.raw-shell"]["enabled"] is False
            assert by_id["device.process.inspect"]["risk"] == "R0"

            status, runtime_catalog = request(port, args.agent_token, "GET", "/v1/runtime/catalog")
            assert status == 200
            assert runtime_catalog["schemaVersion"] == 1
            assert any(item["id"] == "roottools.execd" for item in runtime_catalog["adapters"])

            status, frida_status = request(port, args.agent_token, "GET", "/v1/runtime/frida")
            assert status == 200
            assert frida_status["providerId"] == "runtime.frida"
            assert frida_status["policy"]["scriptExecutionExposed"] is False
            assert frida_status["policy"]["arbitraryAttachExposed"] is False

            status, ellekit_status = request(port, args.agent_token, "GET", "/v1/runtime/ellekit")
            assert status == 200
            assert ellekit_status["providerId"] == "runtime.ellekit"
            assert ellekit_status["policy"]["rawHookAPIExposed"] is False
            assert ellekit_status["policy"]["arbitraryInjectionExposed"] is False

            status, provider_catalog = request(port, args.agent_token, "GET", "/v1/providers/catalog")
            assert status == 200
            assert provider_catalog["schemaVersion"] == 1
            provider_by_id = {item["id"]: item for item in provider_catalog["providers"]}
            assert provider_by_id["roottools.execd"]["state"] == "available"
            assert "package.trollstore" in provider_by_id
            binding_by_capability = {item["capabilityId"]: item for item in provider_catalog["bindings"]}
            assert binding_by_capability["device.app.launch"]["providerId"] == "ui.springboard"

            status, deb_plan = request(port, args.agent_token, "POST", "/v1/package/plan", {"format": "deb"})
            assert status == 200
            assert deb_plan["selectedProviderId"] == "bootstrap.procursus"
            assert deb_plan["policy"]["rawShell"] is False
            status, ipa_plan = request(port, args.agent_token, "POST", "/v1/package/plan", {"format": "ipa"})
            assert status == 200
            assert ipa_plan["selectedProviderId"] == "package.trollstore"

            package_payload = b"roottools-http-package-fixture"
            package_id = f"http-package-{uuid.uuid4().hex}"
            package_hash = hashlib.sha256(package_payload).hexdigest()
            package_begin = action(
                port,
                args.agent_token,
                "device.package.stage.begin",
                parameters={
                    "packageId": package_id,
                    "name": "fixture.deb",
                    "format": "deb",
                    "expectedIdentifier": "com.example.fixture",
                    "totalSize": len(package_payload),
                    "sha256": package_hash,
                },
            )
            assert package_begin["ok"] is True
            package_chunk = action(
                port,
                args.agent_token,
                "device.package.stage.chunk",
                parameters={
                    "packageId": package_id,
                    "offset": 0,
                    "data": base64.b64encode(package_payload).decode(),
                },
            )
            assert package_chunk["ok"] is True
            package_commit = action(
                port,
                args.agent_token,
                "device.package.stage.commit",
                parameters={"packageId": package_id},
            )
            assert package_commit["ok"] is True
            assert package_commit["result"] == "ready"
            status, package_catalog = request(port, args.agent_token, "GET", "/v1/packages/catalog")
            assert status == 200
            staged = next(item for item in package_catalog["packages"] if item["packageId"] == package_id)
            assert staged["state"] == "ready"
            install_agent = action(
                port,
                args.agent_token,
                "device.package.install-deb",
                confirmed=True,
                parameters={"packageId": package_id},
            )
            assert install_agent["result"] == "confirmation_required"
            install_owner = action(
                port,
                args.admin_token,
                "device.package.install-deb",
                confirmed=True,
                parameters={"packageId": package_id},
            )
            assert install_owner["result"] == "provider_unavailable"
            assert install_owner["executed"] is False
            uninstall_agent = action(
                port,
                args.agent_token,
                "device.package.uninstall-deb",
                confirmed=True,
                parameters={"packageId": package_id},
            )
            assert uninstall_agent["result"] == "confirmation_required"
            rollback_agent = action(
                port,
                args.agent_token,
                "device.package.rollback-deb",
                confirmed=True,
                parameters={"packageId": package_id},
            )
            assert rollback_agent["result"] == "confirmation_required"
            status, package_history = request(port, args.agent_token, "GET", "/v1/packages/history")
            assert status == 200
            assert package_history["events"] == []
            package_discard = action(
                port,
                args.agent_token,
                "device.package.discard",
                parameters={"packageId": package_id},
            )
            assert package_discard["ok"] is True
            assert package_discard["result"] == "discarded"

            ipa_path = temp_path / "fixture.ipa"
            with zipfile.ZipFile(ipa_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr(
                    "Payload/Fixture.app/Info.plist",
                    plistlib.dumps(
                        {
                            "CFBundleIdentifier": "com.example.fixture.ipa",
                            "CFBundleExecutable": "Fixture",
                            "CFBundleName": "Fixture",
                        },
                        fmt=plistlib.FMT_BINARY,
                    ),
                )
                archive.writestr("Payload/Fixture.app/Fixture", b"fixture executable bytes")
            ipa_payload = ipa_path.read_bytes()
            ipa_id = f"http-ipa-{uuid.uuid4().hex}"
            ipa_begin = action(
                port,
                args.agent_token,
                "device.package.stage.begin",
                parameters={
                    "packageId": ipa_id,
                    "name": "fixture.ipa",
                    "format": "ipa",
                    "expectedIdentifier": "",
                    "totalSize": len(ipa_payload),
                    "sha256": hashlib.sha256(ipa_payload).hexdigest(),
                },
            )
            assert ipa_begin["ok"] is True
            ipa_chunk = action(
                port,
                args.agent_token,
                "device.package.stage.chunk",
                parameters={
                    "packageId": ipa_id,
                    "offset": 0,
                    "data": base64.b64encode(ipa_payload).decode(),
                },
            )
            assert ipa_chunk["ok"] is True
            ipa_commit = action(
                port,
                args.agent_token,
                "device.package.stage.commit",
                parameters={"packageId": ipa_id},
            )
            assert ipa_commit["ok"] is True
            status, ipa_catalog = request(port, args.agent_token, "GET", "/v1/packages/catalog")
            assert status == 200
            ipa_row = next(item for item in ipa_catalog["packages"] if item["packageId"] == ipa_id)
            assert ipa_row["state"] == "ready"
            assert ipa_row["expectedIdentifier"] == "com.example.fixture.ipa"
            ipa_discard = action(
                port,
                args.agent_token,
                "device.package.discard",
                parameters={"packageId": ipa_id},
            )
            assert ipa_discard["ok"] is True

            self_payload = b"roottools-self-update-http-fixture"
            self_id = f"http-self-update-{uuid.uuid4().hex}"
            self_begin = action(
                port,
                args.agent_token,
                "device.package.stage.begin",
                parameters={
                    "packageId": self_id,
                    "name": "roottools.deb",
                    "format": "deb",
                    "expectedIdentifier": "com.arthur.roottools",
                    "totalSize": len(self_payload),
                    "sha256": hashlib.sha256(self_payload).hexdigest(),
                },
            )
            assert self_begin["ok"] is True
            assert action(
                port,
                args.agent_token,
                "device.package.stage.chunk",
                parameters={"packageId": self_id, "offset": 0, "data": base64.b64encode(self_payload).decode()},
            )["ok"] is True
            assert action(
                port,
                args.agent_token,
                "device.package.stage.commit",
                parameters={"packageId": self_id},
            )["ok"] is True
            self_agent = action(
                port,
                args.agent_token,
                "device.self-update.schedule",
                confirmed=True,
                parameters={"packageId": self_id},
            )
            assert self_agent["result"] == "confirmation_required"
            self_owner = action(
                port,
                args.admin_token,
                "device.self-update.schedule",
                confirmed=True,
                parameters={"packageId": self_id},
            )
            assert self_owner["result"] == "provider_unavailable"
            assert self_owner["executed"] is False
            status, self_status = request(port, args.agent_token, "GET", "/v1/self-update/status")
            assert status == 200
            assert self_status["updates"] == []
            assert action(
                port,
                args.agent_token,
                "device.package.discard",
                parameters={"packageId": self_id},
            )["ok"] is True

            status, lock_state = request(port, args.agent_token, "GET", "/v1/device/lock-state")
            assert status == 200
            assert lock_state["lockState"] == "locked"
            assert lock_state["locked"] is True
            assert lock_state["screenState"] == "blanked"
            assert lock_state["uiExecutionReady"] is False

            status, automation_state = request(port, args.agent_token, "GET", "/v1/automation/state")
            assert status == 200
            assert automation_state["mode"] == "lock-aware"
            assert automation_state["policy"]["bypassDevicePasscode"] is False
            assert automation_state["policy"]["uiJobsWaitForUnlock"] is True

            queued = action(
                port,
                args.agent_token,
                "device.automation.queue-app-launch",
                parameters={"bundleID": "com.apple.Preferences"},
            )
            assert queued["ok"] is True
            assert queued["executed"] is True
            assert queued["result"] == "queued"
            queued_job_id = queued["output"]
            status, queue_payload = request(port, args.agent_token, "GET", "/v1/automation/queue")
            assert status == 200
            queued_row = next(item for item in queue_payload["jobs"] if item["jobId"] == queued_job_id)
            assert queued_row["state"] == "pending"

            cancelled = action(
                port,
                args.agent_token,
                "device.automation.cancel",
                parameters={"jobID": queued_job_id},
            )
            assert cancelled["ok"] is True
            assert cancelled["result"] == "success"
            status, queue_after_cancel = request(port, args.agent_token, "GET", "/v1/automation/queue")
            assert status == 200
            cancelled_row = next(item for item in queue_after_cancel["jobs"] if item["jobId"] == queued_job_id)
            assert cancelled_row["state"] == "cancelled"

            status, fs_scopes = request(port, args.agent_token, "GET", "/v1/fs/scopes")
            assert status == 200
            assert {item["id"] for item in fs_scopes["scopes"]} == {"mobile", "bootstrap"}

            status, fs_list = request(port, args.agent_token, "POST", "/v1/fs/list", {"scope": "mobile"})
            assert status == 200
            assert fs_list["scope"] == "mobile"
            assert isinstance(fs_list["entries"], list)

            status, broad_files = request(port, args.agent_token, "GET", "/v1/files")
            assert status == 403
            assert "owner-only" in broad_files["error"]

            status, process_catalog = request(port, args.agent_token, "GET", "/v1/processes/catalog")
            assert status == 200
            assert any(item["pid"] == daemon.pid for item in process_catalog["processes"])

            status, network_catalog = request(port, args.agent_token, "GET", "/v1/network/catalog")
            assert status == 200
            assert isinstance(network_catalog["interfaces"], list)

            status, tcc_payload = request(port, args.agent_token, "GET", "/v1/permissions/tcc")
            assert status == 200
            assert tcc_payload["source"].startswith("snapshot:")
            assert tcc_payload["count"] == 1
            assert tcc_payload["records"][0]["service"] == "kTCCServiceCamera"
            assert tcc_payload["records"][0]["client"] == "com.example.fixture"

            status, screen_info = request(port, args.agent_token, "GET", "/v1/ui/screen-info")
            assert status == 503
            assert screen_info["error"] == "ZXTouch screen adapter unavailable"

            status, inspected = request(port, args.agent_token, "POST", "/v1/inspect/process", {"pid": daemon.pid})
            assert status == 200
            assert inspected["process"]["pid"] == daemon.pid

            status, _ = request(
                port,
                args.admin_token,
                "POST",
                "/v1/capabilities/set",
                {"capabilityId": "device.process.list", "enabled": False},
            )
            assert status == 200
            try:
                status, _ = request(port, args.agent_token, "GET", "/v1/processes")
                assert status == 403
            finally:
                status, _ = request(
                    port,
                    args.admin_token,
                    "POST",
                    "/v1/capabilities/set",
                    {"capabilityId": "device.process.list", "enabled": True},
                )
                assert status == 200
            status, after_process_policy = request(port, args.agent_token, "GET", "/v1/hello")
            assert status == 200
            assert after_process_policy["revision"] >= initial_revision + 2

            status, _ = request(
                port,
                args.agent_token,
                "POST",
                "/v1/capabilities/set",
                {"capabilityId": "device.app.launch", "enabled": False},
            )
            assert status == 403

            status, _ = request(
                port,
                args.admin_token,
                "POST",
                "/v1/capabilities/set",
                {"capabilityId": "device.raw-shell", "enabled": True},
            )
            assert status == 403

            status, _ = request(
                port,
                args.admin_token,
                "POST",
                "/v1/capabilities/set",
                {"capabilityId": "device.app.launch", "enabled": False},
            )
            assert status == 200
            try:
                denied = action(port, args.agent_token, "device.app.launch", parameters={"bundleID": "com.apple.Preferences"})
                assert denied["ok"] is False
                assert denied["executed"] is False
                assert denied["policy"] == "deny"
                assert denied["caller"] == "trusted-host-agent"
            finally:
                status, _ = request(
                    port,
                    args.admin_token,
                    "POST",
                    "/v1/capabilities/set",
                    {"capabilityId": "device.app.launch", "enabled": True},
                )
                assert status == 200

            status, revision_now = request(port, args.agent_token, "GET", "/v1/hello")
            assert status == 200
            stale_revision = revision_now["revision"]
            status, _ = request(
                port,
                args.admin_token,
                "POST",
                "/v1/capabilities/set",
                {"capabilityId": "device.network.observe", "enabled": False},
            )
            assert status == 200
            status, _ = request(
                port,
                args.admin_token,
                "POST",
                "/v1/capabilities/set",
                {"capabilityId": "device.network.observe", "enabled": True},
            )
            assert status == 200
            stale = action(
                port,
                args.agent_token,
                "device.app.launch",
                parameters={"bundleID": "com.apple.Preferences"},
                expected_revision=stale_revision,
            )
            assert stale["ok"] is False
            assert stale["executed"] is False
            assert stale["policy"] == "stale_revision"
            assert stale["result"] == "stale_revision"
            assert stale["revision"] > stale_revision

            provider_unavailable = action(
                port,
                args.agent_token,
                "device.app.launch",
                parameters={"bundleID": "com.apple.Preferences"},
            )
            assert provider_unavailable["ok"] is False
            assert provider_unavailable["executed"] is False
            assert provider_unavailable["result"] == "provider_unavailable"
            assert provider_unavailable["providerId"] == "ui.springboard"

            r2 = action(port, args.agent_token, "device.process.terminate", parameters={"pid": 123})
            assert r2["ok"] is False
            assert r2["executed"] is False
            assert r2["policy"] == "confirmation_required"
            assert r2["result"] == "confirmation_required"
            assert r2["caller"] == "trusted-host-agent"

            r2_spoof = action(
                port,
                args.agent_token,
                "device.process.terminate",
                confirmed=True,
                parameters={"pid": 123},
            )
            assert r2_spoof["ok"] is False
            assert r2_spoof["executed"] is False
            assert r2_spoof["policy"] == "confirmation_required"
            assert r2_spoof["caller"] == "trusted-host-agent"

            admin_r2 = action(
                port,
                args.admin_token,
                "device.process.terminate",
                confirmed=True,
                parameters={"pid": 999999},
            )
            assert admin_r2["policy"] == "allow"
            assert admin_r2["executed"] is False
            assert admin_r2["caller"] == "roottools-ui"

            r3 = action(port, args.agent_token, "device.raw-shell", confirmed=True)
            assert r3["ok"] is False
            assert r3["executed"] is False
            assert r3["policy"] == "deny"

            traversal = action(
                port,
                args.agent_token,
                "device.fs.write",
                parameters={"scope": "mobile", "name": "../escape", "content": "no"},
            )
            assert traversal["ok"] is False
            assert traversal["executed"] is False

            replay_id = f"replay-{uuid.uuid4().hex}"
            replay_first = action(
                port,
                args.agent_token,
                "device.raw-shell",
                confirmed=True,
                request_id=replay_id,
            )
            replay_second = action(
                port,
                args.agent_token,
                "device.raw-shell",
                confirmed=True,
                request_id=replay_id,
            )
            assert replay_first["replayed"] is False
            assert replay_second["replayed"] is True
            assert replay_second["auditId"] == replay_first["auditId"]
            assert replay_second["requestId"] == replay_id

            status, replay_events = request(
                port,
                args.agent_token,
                "POST",
                "/v1/events/replay",
                {"afterSequence": 0, "limit": 200},
            )
            assert status == 200
            replay_lifecycle = [item for item in replay_events["events"] if item["requestId"] == replay_id]
            assert [item["kind"] for item in replay_lifecycle] == ["accepted", "rejected"]
            assert all(item["caller"] == "trusted-host-agent" for item in replay_lifecycle)

            status, first_event_page = request(
                port,
                args.agent_token,
                "POST",
                "/v1/events/replay",
                {"afterSequence": 0, "limit": 1},
            )
            assert status == 200
            assert first_event_page["count"] == 1
            assert first_event_page["hasMore"] is True
            status, next_event_page = request(
                port,
                args.agent_token,
                "POST",
                "/v1/events/replay",
                {"afterSequence": first_event_page["lastSequence"], "limit": 200},
            )
            assert status == 200
            assert all(item["sequence"] > first_event_page["lastSequence"] for item in next_event_page["events"])

            conflict_body = {
                "requestId": replay_id,
                "capabilityId": "device.fs.write",
                "caller": "spoofed-caller",
                "confirmed": False,
                "parameters": {"scope": "mobile", "name": "different.txt", "content": "different"},
            }
            status, conflict = request(port, args.agent_token, "POST", "/v1/action", conflict_body)
            assert status == 409
            assert "different request" in conflict["error"]

            pending_id = f"pending-{uuid.uuid4().hex}"
            pending_body = {
                "requestId": pending_id,
                "capabilityId": "device.fs.write",
                "caller": "spoofed-caller",
                "confirmed": False,
                "parameters": {"scope": "mobile", "name": "pending.txt", "content": "never-replay"},
            }
            encoded_pending = json.dumps(pending_body, separators=(",", ":")).encode()
            fingerprint = hashlib.sha256(
                b"trusted-host-agent\0device.fs.write\0" + encoded_pending
            ).hexdigest()
            with sqlite3.connect(ledger_path) as ledger:
                ledger.execute(
                    "INSERT INTO action_requests(request_id,caller,capability_id,request_hash,state,created_at) "
                    "VALUES(?,?,?,?,0,?)",
                    (pending_id, "trusted-host-agent", "device.fs.write", fingerprint, int(time.time())),
                )
                ledger.commit()
            status, pending = request(port, args.agent_token, "POST", "/v1/action", pending_body)
            assert status == 409
            assert "indeterminate" in pending["error"]

            status, legacy = request(
                port,
                args.agent_token,
                "POST",
                "/v1/actions/file-read",
                {"scope": "mobile", "name": "missing-test-file"},
            )
            assert status == 200
            assert legacy["capabilityId"] == "device.fs.read"
            assert legacy["caller"] == "trusted-host-agent"

            status, audit = request(port, args.agent_token, "GET", "/v1/audit")
            assert status == 200
            for audit_id in (
                denied["auditId"], r2["auditId"], r2_spoof["auditId"], admin_r2["auditId"],
                r3["auditId"], traversal["auditId"], stale["auditId"], legacy["auditId"]
            ):
                assert audit_id in audit["output"]
            assert audit["output"].count(replay_first["auditId"]) == 1

            daemon.terminate()
            daemon.wait(timeout=1)
            daemon = subprocess.Popen([str(args.daemon)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            wait_ready(port, args.agent_token)
            replay_after_restart = action(
                port,
                args.agent_token,
                "device.raw-shell",
                confirmed=True,
                request_id=replay_id,
            )
            assert replay_after_restart["replayed"] is True
            assert replay_after_restart["auditId"] == replay_first["auditId"]
            status, audit_after_restart = request(port, args.agent_token, "GET", "/v1/audit")
            assert status == 200
            assert audit_after_restart["output"].count(replay_first["auditId"]) == 1

            agent_rotate_denied = action(
                port,
                args.agent_token,
                "device.agent.rotate",
                confirmed=True,
            )
            assert agent_rotate_denied["ok"] is False
            assert agent_rotate_denied["executed"] is False
            assert agent_rotate_denied["policy"] == "confirmation_required"

            rotate = action(
                port,
                args.admin_token,
                "device.agent.rotate",
                confirmed=True,
            )
            assert rotate["ok"] is True
            assert rotate["executed"] is True
            assert rotate["postCondition"]["passed"] is True
            new_agent_token = rotate["output"]
            assert len(new_agent_token) == 48
            assert new_agent_token != args.agent_token
            assert new_agent_token not in audit_path.read_text()

            status, _ = request(port, args.agent_token, "GET", "/v1/hello")
            assert status == 401
            status, new_agent_hello = request(port, new_agent_token, "GET", "/v1/hello")
            assert status == 200
            assert new_agent_hello["authenticatedRole"] == "agent"

            daemon.terminate()
            daemon.wait(timeout=1)
            daemon = subprocess.Popen([str(args.daemon)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            wait_ready(port, new_agent_token)
            status, _ = request(port, args.agent_token, "GET", "/v1/hello")
            assert status == 401
            status, persisted_agent_hello = request(port, new_agent_token, "GET", "/v1/hello")
            assert status == 200
            assert persisted_agent_hello["authenticatedRole"] == "agent"

            print("http_contract_test: PASS")
            return 0
        finally:
            daemon.terminate()
            try:
                daemon.wait(timeout=1)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()


if __name__ == "__main__":
    raise SystemExit(main())
