#!/usr/bin/env python3
"""Typed host client for the RootTools Device Service.

This intentionally exposes semantic capability calls only. It has no command,
shell, SSH, or Frida execution primitive.
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import hashlib
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

from usbmux_proxy import discover_udid, port_forward


DEVICE_PORT = 45821
ROOT = Path(__file__).resolve().parent.parent
DEFAULT_AGENT_TOKEN = ROOT / ".roottools-agent-token"
DEFAULT_ADMIN_TOKEN = ROOT / ".roottools-token"


class DeviceServiceError(RuntimeError):
    pass


def free_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


@contextlib.contextmanager
def device_proxy(udid: str | None):
    if not udid:
        yield DEVICE_PORT
        return

    if not shutil.which("iproxy"):
        with port_forward(udid, DEVICE_PORT) as port:
            yield port
        return

    port = free_local_port()
    process = subprocess.Popen(
        ["iproxy", "-u", udid, f"{port}:{DEVICE_PORT}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.3)
        if process.poll() is not None:
            error = process.stderr.read().strip() if process.stderr else ""
            raise DeviceServiceError(error or "iproxy exited before the Device Service became reachable")
        yield port
    finally:
        process.terminate()
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


class DeviceServiceClient:
    def __init__(self, port: int, token: str, caller: str):
        self.base_url = f"http://127.0.0.1:{port}"
        self.token = token
        self.caller = caller

    def request(self, method: str, path: str, body: dict | None = None, timeout: float = 4) -> dict:
        data = None if body is None else json.dumps(body, separators=(",", ":")).encode()
        request = urllib.request.Request(
            self.base_url + path,
            method=method,
            data=data,
            headers={
                "X-RootTools-Token": self.token,
                **({"Content-Type": "application/json"} if data is not None else {}),
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                payload = response.read()
        except urllib.error.HTTPError as error:
            payload = error.read().decode(errors="replace")
            raise DeviceServiceError(f"HTTP {error.code}: {payload}") from error
        except (urllib.error.URLError, ConnectionError, TimeoutError, OSError) as error:
            raise DeviceServiceError(f"Device Service unavailable: {error}") from error

        try:
            return json.loads(payload)
        except json.JSONDecodeError as error:
            raise DeviceServiceError("Device Service returned invalid JSON") from error

    def status(self) -> dict:
        return self.request("GET", "/v1/status")

    def hello(self) -> dict:
        return self.request("GET", "/v1/hello")

    def capabilities(self) -> dict:
        return self.request("GET", "/v1/capabilities/catalog")

    def providers(self) -> dict:
        return self.request("GET", "/v1/providers/catalog")

    def principals(self) -> dict:
        return self.request("GET", "/v1/principals/catalog")

    def package_plan(self, package_format: str) -> dict:
        return self.request("POST", "/v1/package/plan", {"format": package_format})

    def packages(self) -> dict:
        return self.request("GET", "/v1/packages/catalog")

    def package_history(self) -> dict:
        return self.request("GET", "/v1/packages/history")

    def self_update_status(self) -> dict:
        return self.request("GET", "/v1/self-update/status")

    def runtime_catalog(self) -> dict:
        return self.request("GET", "/v1/runtime/catalog")

    def frida_status(self) -> dict:
        return self.request("GET", "/v1/runtime/frida")

    def ellekit_status(self) -> dict:
        return self.request("GET", "/v1/runtime/ellekit")

    def lock_state(self) -> dict:
        return self.request("GET", "/v1/device/lock-state")

    def automation_state(self) -> dict:
        return self.request("GET", "/v1/automation/state")

    def automation_queue(self) -> dict:
        return self.request("GET", "/v1/automation/queue")

    def tasks(self) -> dict:
        return self.request("GET", "/v1/tasks/catalog")

    def app_inspect(self, bundle_id: str) -> dict:
        return self.request("POST", "/v1/inspect/app", {"bundleID": bundle_id})

    def app_catalog(self) -> dict:
        return self.request("GET", "/v1/apps/catalog")

    def process_inspect(self, pid: int) -> dict:
        return self.request("POST", "/v1/inspect/process", {"pid": pid})

    def process_catalog(self) -> dict:
        return self.request("GET", "/v1/processes/catalog")

    def fs_scopes(self) -> dict:
        return self.request("GET", "/v1/fs/scopes")

    def fs_list(self, scope: str) -> dict:
        return self.request("POST", "/v1/fs/list", {"scope": scope})

    def screen_info(self) -> dict:
        return self.request("GET", "/v1/ui/screen-info")

    def tcc_permissions(self) -> dict:
        return self.request("GET", "/v1/permissions/tcc")

    def network_catalog(self) -> dict:
        return self.request("GET", "/v1/network/catalog")

    def events(self, after_sequence: int = 0, limit: int = 100) -> dict:
        return self.request(
            "POST",
            "/v1/events/replay",
            {"afterSequence": after_sequence, "limit": limit},
        )

    def text(self, path: str) -> dict:
        return self.request("GET", path)

    def action(
        self,
        capability_id: str,
        parameters: dict,
        confirmed: bool = False,
        request_id: str | None = None,
        expected_revision: int | None = None,
        timeout: float = 4,
    ) -> dict:
        body = {
            "requestId": request_id or str(uuid.uuid4()),
            "capabilityId": capability_id,
            "caller": self.caller,
            "confirmed": confirmed,
            "parameters": parameters,
        }
        if expected_revision is not None:
            body["expectedRevision"] = expected_revision
        return self.request(
            "POST",
            "/v1/commands/submit",
            body,
            timeout=timeout,
        )

    def create_principal(self, principal_id: str, kind: str, display_name: str, confirmed: bool) -> dict:
        return self.action(
            "device.principal.create",
            {"principalId": principal_id, "kind": kind, "displayName": display_name},
            confirmed=confirmed,
        )

    def revoke_principal(self, principal_id: str, confirmed: bool) -> dict:
        return self.action("device.principal.revoke", {"principalId": principal_id}, confirmed=confirmed)

    def principal_grants(self, principal_id: str) -> dict:
        return self.request("POST", "/v1/principals/grants", {"principalId": principal_id})

    def grant_principal(self, principal_id: str, capability_id: str, confirmed: bool, expires_at: int | None = None) -> dict:
        parameters: dict[str, object] = {
            "principalId": principal_id,
            "grantedCapabilityId": capability_id,
        }
        if expires_at is not None:
            parameters["expiresAt"] = expires_at
        return self.action("device.principal.grant", parameters, confirmed=confirmed)

    def ungrant_principal(self, principal_id: str, capability_id: str, confirmed: bool) -> dict:
        return self.action(
            "device.principal.ungrant",
            {"principalId": principal_id, "grantedCapabilityId": capability_id},
            confirmed=confirmed,
        )

    def stage_package(self, package_path: Path, expected_identifier: str = "") -> dict:
        package_path = package_path.resolve()
        if not package_path.is_file():
            raise DeviceServiceError(f"Package file not found: {package_path}")
        package_format = package_path.suffix.lower().lstrip(".")
        if package_format not in {"deb", "ipa", "tipa"}:
            raise DeviceServiceError("Only .deb, .ipa, and .tipa packages are supported")
        package_id = f"pkg-{uuid.uuid4().hex}"
        digest = hashlib.sha256()
        total_size = 0
        with package_path.open("rb") as stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                total_size += len(chunk)
        begin = self.action(
            "device.package.stage.begin",
            {
                "packageId": package_id,
                "name": package_path.name,
                "format": package_format,
                "expectedIdentifier": expected_identifier,
                "totalSize": total_size,
                "sha256": digest.hexdigest(),
            },
        )
        if not begin.get("ok"):
            return begin
        offset = 0
        with package_path.open("rb") as stream:
            while True:
                chunk = stream.read(256 * 1024)
                if not chunk:
                    break
                receipt = self.action(
                    "device.package.stage.chunk",
                    {
                        "packageId": package_id,
                        "offset": offset,
                        "data": base64.b64encode(chunk).decode("ascii"),
                    },
                    timeout=15,
                )
                if not receipt.get("ok"):
                    return receipt
                offset += len(chunk)
        return self.action(
            "device.package.stage.commit",
            {"packageId": package_id},
            timeout=30,
        )

    def install_package(self, package_id: str, confirmed: bool) -> dict:
        rows = self.packages().get("packages", [])
        package = next((item for item in rows if item.get("packageId") == package_id), None)
        if not package:
            raise DeviceServiceError(f"Staged package not found: {package_id}")
        package_format = package.get("format")
        capability = "device.package.install-deb" if package_format == "deb" else "device.package.install-ipa"
        return self.action(
            capability,
            {"packageId": package_id},
            confirmed=confirmed,
            timeout=180,
        )

    def uninstall_package(self, package_id: str, confirmed: bool) -> dict:
        rows = self.packages().get("packages", [])
        package = next((item for item in rows if item.get("packageId") == package_id), None)
        if not package:
            raise DeviceServiceError(f"Managed package not found: {package_id}")
        capability = "device.package.uninstall-deb" if package.get("format") == "deb" else "device.package.uninstall-ipa"
        return self.action(capability, {"packageId": package_id}, confirmed=confirmed, timeout=180)

    def rollback_package(self, package_id: str, confirmed: bool) -> dict:
        rows = self.packages().get("packages", [])
        package = next((item for item in rows if item.get("packageId") == package_id), None)
        if not package:
            raise DeviceServiceError(f"Retained package not found: {package_id}")
        capability = "device.package.rollback-deb" if package.get("format") == "deb" else "device.package.rollback-ipa"
        return self.action(capability, {"packageId": package_id}, confirmed=confirmed, timeout=180)

    def schedule_self_update(self, package_id: str, confirmed: bool) -> dict:
        rows = self.packages().get("packages", [])
        package = next((item for item in rows if item.get("packageId") == package_id), None)
        if not package:
            raise DeviceServiceError(f"RootTools update package not found: {package_id}")
        if package.get("format") != "deb" or package.get("expectedIdentifier") != "com.arthur.roottools":
            raise DeviceServiceError("Self-update requires a verified com.arthur.roottools DEB")
        return self.action("device.self-update.schedule", {"packageId": package_id}, confirmed=confirmed)

    def discard_package(self, package_id: str) -> dict:
        return self.action("device.package.discard", {"packageId": package_id})


def load_token(path: Path) -> str:
    try:
        token = path.read_text().strip()
    except OSError as error:
        raise DeviceServiceError(f"Unable to read token file {path}: {error}") from error
    if not token:
        raise DeviceServiceError(f"Token file {path} is empty")
    return token


def parameters_json(value: str) -> dict:
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if not isinstance(parsed, dict):
        raise argparse.ArgumentTypeError("parameters must be a JSON object")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Typed RootTools Device Service client")
    parser.add_argument("--udid", default=os.environ.get("ROOTTOOLS_UDID"), help="USB iPhone UDID; omit for local daemon")
    parser.add_argument("--token-file", type=Path, default=DEFAULT_AGENT_TOKEN)
    parser.add_argument("--caller", default="host-cli")
    parser.add_argument("--request-id", help="Stable idempotency key for retrying one action byte-for-byte")
    parser.add_argument("--expected-revision", type=int, help="Reject the action if the device execution revision has changed")
    parser.add_argument("--compact", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("hello")
    sub.add_parser("status")
    sub.add_parser("capabilities")
    sub.add_parser("providers")
    sub.add_parser("principal-list", help="Owner/admin only: list named command principals")
    principal_create = sub.add_parser("principal-create", help="R2 owner action: create a named host/app/skill/automation principal")
    principal_create.add_argument("principal_id")
    principal_create.add_argument("kind", choices=("host", "app", "skill", "automation"))
    principal_create.add_argument("display_name")
    principal_create.add_argument("--confirm", action="store_true", required=True)
    principal_create.add_argument("--save-to", type=Path, help="Persist the one-time principal credential locally with mode 0600")
    principal_revoke = sub.add_parser("principal-revoke", help="R2 owner action: revoke a named command principal")
    principal_revoke.add_argument("principal_id")
    principal_revoke.add_argument("--confirm", action="store_true", required=True)
    principal_grants = sub.add_parser("principal-grants", help="Owner/admin only: list one principal's capability grants")
    principal_grants.add_argument("principal_id")
    principal_grant = sub.add_parser("principal-grant", help="R2 owner action: grant one R0/R1 capability")
    principal_grant.add_argument("principal_id")
    principal_grant.add_argument("capability_id")
    principal_grant.add_argument("--expires-at", type=int)
    principal_grant.add_argument("--confirm", action="store_true", required=True)
    principal_ungrant = sub.add_parser("principal-ungrant", help="R2 owner action: remove one capability grant")
    principal_ungrant.add_argument("principal_id")
    principal_ungrant.add_argument("capability_id")
    principal_ungrant.add_argument("--confirm", action="store_true", required=True)
    package_plan = sub.add_parser("package-plan")
    package_plan.add_argument("format", choices=("deb", "ipa", "tipa"))
    sub.add_parser("package-list")
    sub.add_parser("package-history")
    sub.add_parser("self-update-status")
    package_stage = sub.add_parser("package-stage")
    package_stage.add_argument("file", type=Path)
    package_stage.add_argument("--identifier", default="", help="Optional expected DEB Package ID or iOS bundle ID")
    package_install = sub.add_parser("package-install", help="R2 owner action; use the admin token file")
    package_install.add_argument("package_id")
    package_install.add_argument("--confirm", action="store_true", required=True)
    package_discard = sub.add_parser("package-discard")
    package_discard.add_argument("package_id")
    package_uninstall = sub.add_parser("package-uninstall", help="R2 owner action; uninstall one RootTools-managed package")
    package_uninstall.add_argument("package_id")
    package_uninstall.add_argument("--confirm", action="store_true", required=True)
    package_rollback = sub.add_parser("package-rollback", help="R2 owner action; restore a retained verified package")
    package_rollback.add_argument("package_id")
    package_rollback.add_argument("--confirm", action="store_true", required=True)
    self_update = sub.add_parser("self-update", help="R2 owner action; schedule an independent RootTools self-update")
    self_update.add_argument("package_id")
    self_update.add_argument("--confirm", action="store_true", required=True)
    sub.add_parser("audit")
    sub.add_parser("runtime")
    sub.add_parser("apps")
    sub.add_parser("app-catalog")
    sub.add_parser("processes")
    sub.add_parser("process-catalog")
    sub.add_parser("network")
    sub.add_parser("diagnostics")
    sub.add_parser("runtime-catalog")
    sub.add_parser("frida-status")
    sub.add_parser("ellekit-status")
    sub.add_parser("lock-state")
    sub.add_parser("automation-state")
    sub.add_parser("automation-queue")
    sub.add_parser("task-list")
    task_launch = sub.add_parser("task-app-launch", help="Submit a durable app launch task")
    task_launch.add_argument("bundle_id")
    task_cancel = sub.add_parser("task-cancel", help="Cancel one queued/waiting/retrying task")
    task_cancel.add_argument("task_id")
    sub.add_parser("fs-scopes")
    fs_list = sub.add_parser("fs-list")
    fs_list.add_argument("scope", choices=("mobile", "bootstrap"))
    sub.add_parser("screen-info")
    sub.add_parser("tcc")
    sub.add_parser("network-catalog")
    events = sub.add_parser("events")
    events.add_argument("--after", type=int, default=0)
    events.add_argument("--limit", type=int, default=100)

    app_inspect = sub.add_parser("app-inspect")
    app_inspect.add_argument("bundle_id")
    process_inspect = sub.add_parser("process-inspect")
    process_inspect.add_argument("pid", type=int)

    generic = sub.add_parser("action", help="Invoke one registered semantic capability")
    generic.add_argument("capability_id")
    generic.add_argument("--parameters", type=parameters_json, default={})
    generic.add_argument("--confirm", action="store_true")

    policy = sub.add_parser("capability-set", help="Owner/admin only: enable or disable one runtime capability")
    policy.add_argument("capability_id")
    policy.add_argument("state", choices=("on", "off"))

    launch = sub.add_parser("app-launch")
    launch.add_argument("bundle_id")
    queue_launch = sub.add_parser("queue-app-launch")
    queue_launch.add_argument("bundle_id")
    cancel_automation = sub.add_parser("automation-cancel")
    cancel_automation.add_argument("job_id")
    terminate = sub.add_parser("app-terminate")
    terminate.add_argument("bundle_id")
    process = sub.add_parser("process-terminate", help="R2 owner action; use the admin token file")
    process.add_argument("pid", type=int)
    process.add_argument("--confirm", action="store_true", required=True)

    rotate = sub.add_parser("agent-rotate", help="R2 owner action; use the admin token file")
    rotate.add_argument("--confirm", action="store_true", required=True)
    rotate.add_argument("--save-to", type=Path, help="Persist the returned Agent token locally with mode 0600")

    read = sub.add_parser("file-read")
    read.add_argument("scope", choices=("mobile", "bootstrap"))
    read.add_argument("name")
    write = sub.add_parser("file-write")
    write.add_argument("scope", choices=("mobile", "bootstrap"))
    write.add_argument("name")
    write_group = write.add_mutually_exclusive_group(required=True)
    write_group.add_argument("--content")
    write_group.add_argument("--content-file", type=Path)
    return parser


def execute(client: DeviceServiceClient, args: argparse.Namespace) -> dict:
    if args.command == "hello":
        return client.hello()
    if args.command == "status":
        return client.status()
    if args.command == "capabilities":
        return client.capabilities()
    if args.command == "providers":
        return client.providers()
    if args.command == "principal-list":
        return client.principals()
    if args.command == "principal-create":
        return client.create_principal(args.principal_id, args.kind, args.display_name, args.confirm)
    if args.command == "principal-revoke":
        return client.revoke_principal(args.principal_id, args.confirm)
    if args.command == "principal-grants":
        return client.principal_grants(args.principal_id)
    if args.command == "principal-grant":
        return client.grant_principal(args.principal_id, args.capability_id, args.confirm, args.expires_at)
    if args.command == "principal-ungrant":
        return client.ungrant_principal(args.principal_id, args.capability_id, args.confirm)
    if args.command == "package-plan":
        return client.package_plan(args.format)
    if args.command == "package-list":
        return client.packages()
    if args.command == "package-history":
        return client.package_history()
    if args.command == "self-update-status":
        return client.self_update_status()
    if args.command == "package-stage":
        return client.stage_package(args.file, args.identifier)
    if args.command == "package-install":
        return client.install_package(args.package_id, args.confirm)
    if args.command == "package-discard":
        return client.discard_package(args.package_id)
    if args.command == "package-uninstall":
        return client.uninstall_package(args.package_id, args.confirm)
    if args.command == "package-rollback":
        return client.rollback_package(args.package_id, args.confirm)
    if args.command == "self-update":
        return client.schedule_self_update(args.package_id, args.confirm)
    if args.command == "runtime-catalog":
        return client.runtime_catalog()
    if args.command == "frida-status":
        return client.frida_status()
    if args.command == "ellekit-status":
        return client.ellekit_status()
    if args.command == "lock-state":
        return client.lock_state()
    if args.command == "automation-state":
        return client.automation_state()
    if args.command == "automation-queue":
        return client.automation_queue()
    if args.command == "task-list":
        return client.tasks()
    if args.command == "task-app-launch":
        return client.action("device.task.submit-app-launch", {"bundleID": args.bundle_id}, request_id=args.request_id, expected_revision=args.expected_revision)
    if args.command == "task-cancel":
        return client.action("device.task.cancel", {"taskId": args.task_id}, request_id=args.request_id, expected_revision=args.expected_revision)
    if args.command == "fs-scopes":
        return client.fs_scopes()
    if args.command == "fs-list":
        return client.fs_list(args.scope)
    if args.command == "screen-info":
        return client.screen_info()
    if args.command == "tcc":
        return client.tcc_permissions()
    if args.command == "network-catalog":
        return client.network_catalog()
    if args.command == "events":
        return client.events(args.after, args.limit)
    if args.command == "app-inspect":
        return client.app_inspect(args.bundle_id)
    if args.command == "app-catalog":
        return client.app_catalog()
    if args.command == "process-inspect":
        return client.process_inspect(args.pid)
    if args.command == "process-catalog":
        return client.process_catalog()
    if args.command == "audit":
        return client.text("/v1/audit")
    if args.command in {"runtime", "apps", "processes", "network", "diagnostics"}:
        return client.text(f"/v1/{args.command}")
    if args.command == "action":
        return client.action(args.capability_id, args.parameters, args.confirm, args.request_id, args.expected_revision)
    if args.command == "capability-set":
        return client.request(
            "POST",
            "/v1/capabilities/set",
            {"capabilityId": args.capability_id, "enabled": args.state == "on"},
        )
    if args.command == "app-launch":
        return client.action("device.app.launch", {"bundleID": args.bundle_id}, request_id=args.request_id, expected_revision=args.expected_revision)
    if args.command == "queue-app-launch":
        return client.action("device.task.submit-app-launch", {"bundleID": args.bundle_id}, request_id=args.request_id, expected_revision=args.expected_revision)
    if args.command == "automation-cancel":
        return client.action("device.task.cancel", {"taskId": args.job_id}, request_id=args.request_id, expected_revision=args.expected_revision)
    if args.command == "app-terminate":
        return client.action("device.app.terminate", {"bundleID": args.bundle_id}, request_id=args.request_id, expected_revision=args.expected_revision)
    if args.command == "process-terminate":
        return client.action("device.process.terminate", {"pid": args.pid}, args.confirm, args.request_id, args.expected_revision)
    if args.command == "agent-rotate":
        return client.action("device.agent.rotate", {}, args.confirm, args.request_id, args.expected_revision)
    if args.command == "file-read":
        return client.action("device.fs.read", {"scope": args.scope, "name": args.name}, request_id=args.request_id, expected_revision=args.expected_revision)
    if args.command == "file-write":
        content = args.content
        if args.content_file:
            content = args.content_file.read_text()
        return client.action(
            "device.fs.write",
            {"scope": args.scope, "name": args.name, "content": content},
            request_id=args.request_id,
            expected_revision=args.expected_revision,
        )
    raise DeviceServiceError(f"Unsupported command: {args.command}")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        token = load_token(args.token_file)
        target_udid = args.udid
        if not target_udid:
            try:
                target_udid = discover_udid()
            except Exception:
                target_udid = None
        with device_proxy(target_udid) as port:
            result = execute(DeviceServiceClient(port, token, args.caller), args)
        if args.command in {"agent-rotate", "principal-create"} and getattr(args, "save_to", None) and result.get("ok") and result.get("output"):
            args.save_to.parent.mkdir(parents=True, exist_ok=True)
            args.save_to.write_text(result["output"] + "\n")
            args.save_to.chmod(0o600)
        print(json.dumps(result, ensure_ascii=False, indent=None if args.compact else 2, sort_keys=True))
        return 0
    except (DeviceServiceError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
