#!/usr/bin/env python3
"""Three-phase physical qualification for bounded RootTools Remote Access."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import time
from typing import Iterator

from device_service import DeviceServiceClient, DeviceServiceError, device_proxy
from usbmux_proxy import discover_udid


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ADMIN_TOKEN = ROOT / ".roottools-token"
DEFAULT_AGENT_TOKEN = ROOT / ".roottools-agent-token"
DEFAULT_EVIDENCE_DIR = ROOT / "build/qualification"
DEFAULT_STATE = DEFAULT_EVIDENCE_DIR / "remote-access-state.json"
DEFAULT_PRINCIPAL_TOKEN = DEFAULT_EVIDENCE_DIR / "remote-host-token"
DEFAULT_OTHER_PRINCIPAL_TOKEN = DEFAULT_EVIDENCE_DIR / "remote-other-host-token"
ROOTTOOLS_APP = "com.arthur.roottools.ios"
MINIMUM_GRANTS = (
    "device.status.observe",
    "device.performance.observe",
    "device.app.launch",
)


def load_expected_daemon_version() -> str:
    package_version = (ROOT / "VERSION").read_text().strip()
    core, separator, revision = package_version.rpartition("-")
    components = core.split(".")
    if not separator or not revision.isdigit() or len(components) != 3 or not all(item.isdigit() for item in components):
        raise RuntimeError(f"invalid RootTools VERSION: {package_version}")
    return core


DEFAULT_EXPECTED_DAEMON_VERSION = load_expected_daemon_version()
DEFAULT_EXPECTED_PACKAGE_VERSION = (ROOT / "VERSION").read_text().strip()


class QualificationFailure(RuntimeError):
    pass


def credential_files(profile: str) -> tuple[Path, Path]:
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,63}", profile) or ".." in profile:
        raise QualificationFailure(f"invalid credential profile: {profile}")
    if profile == "installed":
        return DEFAULT_ADMIN_TOKEN, DEFAULT_AGENT_TOKEN
    directory = ROOT / ".roottools-credentials" / profile
    return directory / "owner-token", directory / "agent-token"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise QualificationFailure(message)


def receipt_ok(receipt: dict, capability_id: str) -> None:
    safe_receipt = {key: value for key, value in receipt.items() if key != "output"}
    require(receipt.get("capabilityId") == capability_id, f"unexpected receipt: {safe_receipt}")
    require(receipt.get("ok") is True, f"{capability_id} failed: {safe_receipt}")
    require(receipt.get("executed") is True, f"{capability_id} was not executed: {safe_receipt}")
    require(receipt.get("result") == "success", f"{capability_id} did not succeed: {safe_receipt}")
    require(bool(receipt.get("requestId")), f"{capability_id} receipt is missing requestId: {safe_receipt}")
    require(bool(receipt.get("auditId")), f"{capability_id} receipt is missing auditId: {safe_receipt}")


def receipt_evidence(receipt: dict) -> dict:
    return {
        "capabilityId": receipt["capabilityId"],
        "requestId": receipt["requestId"],
        "auditId": receipt["auditId"],
        "result": receipt["result"],
    }


def validate_device_status(status: dict, expected_daemon_version: str, expected_package_version: str) -> None:
    require(
        status.get("daemonVersion") == expected_daemon_version,
        f"expected daemon {expected_daemon_version}, got: {status}",
    )
    require(
        status.get("packageVersion") == expected_package_version,
        f"expected package {expected_package_version}, got: {status}",
    )
    require(status.get("uid") == 0, f"daemon must run as UID 0: {status}")
    require(status.get("jailbreakRootless") is True, f"rootless jailbreak is unavailable: {status}")


def validate_remote_state(state: dict, principal_id: str, *, enabled: bool) -> None:
    require(state.get("enabled") is enabled, f"unexpected Remote Access state: {state}")
    transport = state.get("transport", {})
    require(transport.get("kind") == "tailscale", f"unexpected transport: {transport}")
    policy = state.get("policy", {})
    require(policy.get("publicInternetListener") is False, f"public listener policy widened: {policy}")
    require(policy.get("ownerTokenAcceptedRemotely") is False, f"Owner token accepted remotely: {policy}")
    require(policy.get("legacyAgentTokenAcceptedRemotely") is False, f"Agent token accepted remotely: {policy}")
    if enabled:
        require(state.get("principalId") == principal_id, f"session principal mismatch: {state}")
        require(transport.get("available") is True, f"Tailscale transport unavailable: {transport}")
        require(transport.get("listenerActive") is True, f"remote listener inactive: {transport}")
        address = str(transport.get("bindAddress", ""))
        require(address.startswith("100."), f"listener is not bound to Tailnet IPv4: {transport}")


def expect_remote_rejected(client: DeviceServiceClient, label: str) -> None:
    try:
        client.hello()
    except DeviceServiceError as error:
        require(error.status_code in {401, 403}, f"{label} failed for the wrong reason: {error}")
        return
    raise QualificationFailure(f"{label} was accepted by the remote listener")


def wait_for_remote_state(
    admin_client: DeviceServiceClient,
    principal_id: str,
    *,
    enabled: bool,
    timeout: float = 10,
) -> dict:
    deadline = time.time() + timeout
    latest: dict = {}
    while time.time() < deadline:
        latest = admin_client.remote_access()
        if latest.get("enabled") is enabled:
            if not enabled or latest.get("transport", {}).get("listenerActive") is True:
                validate_remote_state(latest, principal_id, enabled=enabled)
                return latest
        time.sleep(0.2)
    raise QualificationFailure(f"Remote Access did not converge to enabled={enabled}: {latest}")


def write_secret_new(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags, 0o600)
    try:
        os.write(descriptor, (value + "\n").encode())
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    path.chmod(0o600)


def write_state(path: Path, state: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(state, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(0o600)
        os.replace(temporary, path)
        path.chmod(0o600)
    finally:
        if temporary.exists():
            temporary.unlink()


def require_owner_only_file(path: Path, label: str) -> None:
    try:
        metadata = path.lstat()
    except OSError as error:
        raise QualificationFailure(f"{label} is unavailable: {path}: {error}") from error
    require(stat.S_ISREG(metadata.st_mode) and not path.is_symlink(), f"{label} must be a regular non-symlink file: {path}")
    require(stat.S_IMODE(metadata.st_mode) & 0o077 == 0, f"{label} must not be group/other accessible: {path}")


def load_secure_token(path: Path, label: str) -> str:
    require_owner_only_file(path, label)
    token = path.read_text().strip()
    require(valid_token(token), f"{label} is malformed: {path}")
    return token


def valid_token(token: str) -> bool:
    return len(token) == 48 and all(character in "0123456789abcdefABCDEF" for character in token)


def load_state(path: Path) -> dict:
    require_owner_only_file(path, "qualification state")
    return json.loads(path.read_text())


def assert_no_usb_iphone() -> None:
    try:
        connected = discover_udid()
    except Exception:
        return
    raise QualificationFailure(f"USB iPhone is still connected ({connected}); unplug it before off-USB verification")


@contextlib.contextmanager
def usb_admin_client(udid: str | None, admin_token: str) -> Iterator[tuple[str, DeviceServiceClient]]:
    target = udid or discover_udid()
    with device_proxy(target) as port:
        yield target, DeviceServiceClient(port, admin_token, caller="remote-qualification-owner")


def direct_client(host: str, port: int, token: str, caller: str) -> DeviceServiceClient:
    return DeviceServiceClient(port, token, caller=caller, host=host)


def prepare(args: argparse.Namespace) -> dict:
    require(not args.principal_token_file.exists(), f"refusing to overwrite principal credential: {args.principal_token_file}")
    require(
        not args.other_principal_token_file.exists(),
        f"refusing to overwrite non-selected principal credential: {args.other_principal_token_file}",
    )
    admin_token = load_secure_token(args.admin_token_file, "Owner token")
    agent_token = load_secure_token(args.agent_token_file, "Agent token")
    require(admin_token != agent_token, "Owner and Agent tokens must be distinct")
    principal_id = args.principal_id or f"host:qualification-{int(time.time())}"
    other_principal_id = args.other_principal_id or f"{principal_id}-negative"
    require(other_principal_id != principal_id, "selected and non-selected Principal IDs must differ")
    created_principals: list[str] = []
    receipts: list[dict] = []

    with usb_admin_client(args.udid, admin_token) as (udid, admin):
        validate_device_status(admin.status(), args.expected_daemon_version, args.expected_package_version)
        existing = {item.get("principalId") for item in admin.principals().get("principals", [])}
        for candidate in (principal_id, other_principal_id):
            require(candidate not in existing, f"principal already exists and its one-time token cannot be recovered: {candidate}")
        try:
            create_receipt = admin.create_principal(principal_id, "host", "RootTools Qualification Host", True)
            receipt_ok(create_receipt, "device.principal.create")
            receipts.append(receipt_evidence(create_receipt))
            principal_token = str(create_receipt.get("output", ""))
            require(valid_token(principal_token), "principal credential is malformed")
            created_principals.append(principal_id)
            write_secret_new(args.principal_token_file, principal_token)

            other_receipt = admin.create_principal(
                other_principal_id,
                "host",
                "RootTools Non-selected Qualification Host",
                True,
            )
            receipt_ok(other_receipt, "device.principal.create")
            receipts.append(receipt_evidence(other_receipt))
            other_principal_token = str(other_receipt.get("output", ""))
            require(valid_token(other_principal_token), "non-selected principal credential is malformed")
            created_principals.append(other_principal_id)
            write_secret_new(args.other_principal_token_file, other_principal_token)

            grant_expiry = int(time.time()) + max(args.duration_minutes * 60 + 900, 7200)
            for capability_id in MINIMUM_GRANTS:
                grant_receipt = admin.grant_principal(principal_id, capability_id, True, grant_expiry)
                receipt_ok(grant_receipt, "device.principal.grant")
                receipts.append(receipt_evidence(grant_receipt))
            active_grants = {
                item.get("capabilityId")
                for item in admin.principal_grants(principal_id).get("grants", [])
                if item.get("active")
            }
            require(active_grants == set(MINIMUM_GRANTS), f"qualification principal has unexpected grants: {active_grants}")

            configure_receipt = admin.configure_remote_access(
                enabled=True,
                principal_id=principal_id,
                duration_minutes=args.duration_minutes,
                confirmed=True,
            )
            receipt_ok(configure_receipt, "device.remote-access.configure")
            receipts.append(receipt_evidence(configure_receipt))
            remote = wait_for_remote_state(admin, principal_id, enabled=True)
            host = str(remote["transport"]["bindAddress"])
            port = int(remote["transport"]["port"])

            principal = direct_client(host, port, principal_token, "remote-qualification-host")
            validate_device_status(principal.status(), args.expected_daemon_version, args.expected_package_version)
            require(bool(principal.performance()), "remote performance snapshot is empty")
            expect_remote_rejected(direct_client(host, port, admin_token, "remote-owner-negative"), "Owner credential")
            expect_remote_rejected(direct_client(host, port, agent_token, "remote-agent-negative"), "legacy Agent credential")
            expect_remote_rejected(
                direct_client(host, port, other_principal_token, "remote-other-principal-negative"),
                "non-selected Principal credential",
            )

            state = {
                "schemaVersion": 1,
                "phase": "prepared",
                "deviceUdid": udid,
                "expectedDaemonVersion": args.expected_daemon_version,
                "expectedPackageVersion": args.expected_package_version,
                "credentialProfile": args.credential_profile,
                "principalId": principal_id,
                "principalTokenFile": str(args.principal_token_file.resolve()),
                "otherPrincipalId": other_principal_id,
                "otherPrincipalTokenFile": str(args.other_principal_token_file.resolve()),
                "host": host,
                "port": port,
                "sessionExpiresAt": int(remote["expiresAt"]),
                "preparedAt": int(time.time()),
                "receipts": receipts,
            }
            write_state(args.state_file, state)
            return state
        except Exception:
            if created_principals:
                with contextlib.suppress(Exception):
                    admin.configure_remote_access(enabled=False, principal_id="", duration_minutes=0, confirmed=True)
                for created_principal in reversed(created_principals):
                    with contextlib.suppress(Exception):
                        admin.revoke_principal(created_principal, True)
            raise


def verify_remote(args: argparse.Namespace) -> dict:
    assert_no_usb_iphone()
    state = load_state(args.state_file)
    require(state.get("phase") == "prepared", f"qualification is not prepared: {state}")
    principal_token_file = Path(state["principalTokenFile"])
    principal_token = load_secure_token(principal_token_file, "selected Principal token")
    other_principal_token = load_secure_token(Path(state["otherPrincipalTokenFile"]), "non-selected Principal token")
    admin_token = load_secure_token(args.admin_token_file, "Owner token")
    agent_token = load_secure_token(args.agent_token_file, "Agent token")
    host, port = str(state["host"]), int(state["port"])

    principal = direct_client(host, port, principal_token, "remote-qualification-off-usb")
    validate_device_status(
        principal.status(),
        str(state["expectedDaemonVersion"]),
        str(state["expectedPackageVersion"]),
    )
    require(bool(principal.performance()), "off-USB performance snapshot is empty")
    launch = principal.action("device.app.launch", {"bundleID": ROOTTOOLS_APP})
    receipt_ok(launch, "device.app.launch")
    expect_remote_rejected(direct_client(host, port, admin_token, "remote-owner-negative"), "Owner credential")
    expect_remote_rejected(direct_client(host, port, agent_token, "remote-agent-negative"), "legacy Agent credential")
    expect_remote_rejected(
        direct_client(host, port, other_principal_token, "remote-other-principal-negative"),
        "non-selected Principal credential",
    )

    state["phase"] = "remote-verified"
    state["remoteVerifiedAt"] = int(time.time())
    state["r1Capability"] = "device.app.launch"
    state.setdefault("receipts", []).append(receipt_evidence(launch))
    write_state(args.state_file, state)
    return state


def require_remote_closed(host: str, port: int, token: str, label: str) -> None:
    client = direct_client(host, port, token, f"remote-qualification-{label}")
    try:
        client.status()
    except DeviceServiceError:
        return
    raise QualificationFailure(f"remote authority remained available after {label}")


def cleanup(args: argparse.Namespace) -> dict:
    state = load_state(args.state_file)
    allowed_phases = {
        "remote-verified",
        "stop-verified",
        "expiry-running",
        "expiry-verified",
        "expiry-skipped",
        "revoke-session-active",
        "revoke-verified",
    }
    require(state.get("phase") in allowed_phases, f"off-USB verification must pass before cleanup: {state}")
    principal_token = load_secure_token(Path(state["principalTokenFile"]), "selected Principal token")
    admin_token = load_secure_token(args.admin_token_file, "Owner token")
    principal_id = str(state["principalId"])
    other_principal_id = str(state["otherPrincipalId"])
    host, port = str(state["host"]), int(state["port"])

    with usb_admin_client(args.udid, admin_token) as (_, admin):
        if state["phase"] == "remote-verified":
            stop_receipt = admin.configure_remote_access(
                enabled=False,
                principal_id="",
                duration_minutes=0,
                confirmed=True,
            )
            receipt_ok(stop_receipt, "device.remote-access.configure")
            state.setdefault("receipts", []).append(receipt_evidence(stop_receipt))
            wait_for_remote_state(admin, principal_id, enabled=False)
            require_remote_closed(host, port, principal_token, "stop")
            state["stopVerifiedAt"] = int(time.time())
            state["phase"] = "stop-verified"
            write_state(args.state_file, state)

        if state["phase"] == "stop-verified":
            if not args.verify_expiry:
                state["phase"] = "expiry-skipped"
                write_state(args.state_file, state)
            else:
                expiry_receipt = admin.configure_remote_access(
                    enabled=True,
                    principal_id=principal_id,
                    duration_minutes=5,
                    confirmed=True,
                )
                receipt_ok(expiry_receipt, "device.remote-access.configure")
                state["receipts"].append(receipt_evidence(expiry_receipt))
                expiring = wait_for_remote_state(admin, principal_id, enabled=True)
                state["expiryExpectedAt"] = int(expiring["expiresAt"])
                state["phase"] = "expiry-running"
                write_state(args.state_file, state)

        if state["phase"] == "expiry-running":
            expiry_deadline = int(state["expiryExpectedAt"]) + 15
            next_progress = int(time.time())
            while int(time.time()) <= expiry_deadline:
                current = admin.remote_access()
                if current.get("enabled") is False:
                    break
                if int(time.time()) >= next_progress:
                    remaining = max(0, int(current.get("expiresAt", expiry_deadline)) - int(time.time()))
                    print(f"[WAIT] Remote Access expiry in approximately {remaining}s", flush=True)
                    next_progress = int(time.time()) + 30
                time.sleep(2)
            validate_remote_state(admin.remote_access(), principal_id, enabled=False)
            require_remote_closed(host, port, principal_token, "expiry")
            state["expiryVerifiedAt"] = int(time.time())
            state["phase"] = "expiry-verified"
            write_state(args.state_file, state)

        if state["phase"] in {"expiry-verified", "expiry-skipped"}:
            revoke_session_receipt = admin.configure_remote_access(
                enabled=True,
                principal_id=principal_id,
                duration_minutes=5,
                confirmed=True,
            )
            receipt_ok(revoke_session_receipt, "device.remote-access.configure")
            state["receipts"].append(receipt_evidence(revoke_session_receipt))
            wait_for_remote_state(admin, principal_id, enabled=True)
            state["phase"] = "revoke-session-active"
            write_state(args.state_file, state)

        if state["phase"] == "revoke-session-active":
            revoke_receipt = admin.revoke_principal(principal_id, True)
            receipt_ok(revoke_receipt, "device.principal.revoke")
            state["receipts"].append(receipt_evidence(revoke_receipt))
            wait_for_remote_state(admin, principal_id, enabled=False)
            require_remote_closed(host, port, principal_token, "revoke")
            state["revokeVerifiedAt"] = int(time.time())
            state["phase"] = "revoke-verified"
            write_state(args.state_file, state)

        if state["phase"] == "revoke-verified":
            other_revoke_receipt = admin.revoke_principal(other_principal_id, True)
            receipt_ok(other_revoke_receipt, "device.principal.revoke")
            state["receipts"].append(receipt_evidence(other_revoke_receipt))
            state["phase"] = "complete"
            state["completedAt"] = int(time.time())
            write_state(args.state_file, state)
    return state


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Qualify RootTools Remote Access on a physical device")
    parser.add_argument("--state-file", type=Path, default=DEFAULT_STATE)
    parser.add_argument("--credential-profile", default="installed")
    parser.add_argument("--admin-token-file", type=Path)
    parser.add_argument("--agent-token-file", type=Path)
    subparsers = parser.add_subparsers(dest="phase", required=True)

    prepare_parser = subparsers.add_parser("prepare", help="USB: create a least-privilege Host and start a session")
    prepare_parser.add_argument("--udid")
    prepare_parser.add_argument("--principal-id")
    prepare_parser.add_argument("--other-principal-id")
    prepare_parser.add_argument("--principal-token-file", type=Path, default=DEFAULT_PRINCIPAL_TOKEN)
    prepare_parser.add_argument("--other-principal-token-file", type=Path, default=DEFAULT_OTHER_PRINCIPAL_TOKEN)
    prepare_parser.add_argument("--duration-minutes", type=int, default=30, choices=range(5, 481), metavar="5..480")
    prepare_parser.add_argument("--expected-daemon-version", default=DEFAULT_EXPECTED_DAEMON_VERSION)
    prepare_parser.add_argument("--expected-package-version", default=DEFAULT_EXPECTED_PACKAGE_VERSION)

    subparsers.add_parser("verify", help="off-USB: require no USB device and run R0/R1 plus negative auth checks")

    cleanup_parser = subparsers.add_parser("cleanup", help="USB: verify stop/revoke and optionally five-minute expiry")
    cleanup_parser.add_argument("--udid")
    cleanup_parser.add_argument("--verify-expiry", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        default_admin_token, default_agent_token = credential_files(args.credential_profile)
        args.admin_token_file = args.admin_token_file or default_admin_token
        args.agent_token_file = args.agent_token_file or default_agent_token
        if args.phase == "prepare":
            result = prepare(args)
        elif args.phase == "verify":
            result = verify_remote(args)
        else:
            result = cleanup(args)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (DeviceServiceError, QualificationFailure, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
