#!/usr/bin/env python3
"""Physical-device regression for the RootTools v0.3 control plane.

The verifier intentionally drives RootTools through its typed Device Service.
The only exception is one fixed symlink fixture created through the existing
developer provisioning channel so O_NOFOLLOW can be verified end to end.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.error
import urllib.request
import uuid

from device_service import DeviceServiceClient, DeviceServiceError, device_proxy, load_token
from usbmux_proxy import discover_udid


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TOKEN = ROOT / ".roottools-agent-token"
DEFAULT_ADMIN_TOKEN = ROOT / ".roottools-token"
REFERENCE_APP = "com.apple.Preferences"
ROOTTOOLS_APP = "com.arthur.roottools.ios"


class VerificationFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationFailure(message)


def step(label: str, detail: str = "") -> None:
    suffix = f" — {detail}" if detail else ""
    print(f"[PASS] {label}{suffix}")


def process_row(output: str, command: str) -> tuple[int, int] | None:
    pattern = re.compile(r"^\s*(\d+)\s+(\d+)\s+(.+?)\s*$")
    for line in output.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        if match.group(3) == command:
            return int(match.group(1)), int(match.group(2))
    return None


def unauthorized_status(port: int) -> int:
    request = urllib.request.Request(f"http://127.0.0.1:{port}/v1/status")
    try:
        urllib.request.urlopen(request, timeout=4).read()
    except urllib.error.HTTPError as error:
        return error.code
    return 200


def assert_receipt(
    receipt: dict,
    capability_id: str,
    *,
    ok: bool | None = None,
    executed: bool | None = None,
    policy: str | None = None,
    result: str | None = None,
) -> None:
    require(receipt.get("capabilityId") == capability_id, f"unexpected capability receipt: {receipt}")
    require(bool(receipt.get("requestId")), f"missing requestId: {receipt}")
    require(bool(receipt.get("auditId")), f"missing auditId: {receipt}")
    if ok is not None:
        require(receipt.get("ok") is ok, f"unexpected ok for {capability_id}: {receipt}")
    if executed is not None:
        require(receipt.get("executed") is executed, f"unexpected executed for {capability_id}: {receipt}")
    if policy is not None:
        require(receipt.get("policy") == policy, f"unexpected policy for {capability_id}: {receipt}")
    if result is not None:
        require(receipt.get("result") == result, f"unexpected result for {capability_id}: {receipt}")


def install(udid: str) -> None:
    env = os.environ.copy()
    env["ROOTTOOLS_UDID"] = udid
    subprocess.run(["bash", str(ROOT / "Scripts/install-jailbreak.sh")], cwd=ROOT, env=env, check=True)


def create_symlink_fixture(udid: str) -> None:
    command = (
        "mkdir -p /var/mobile/Library/RootTools/files; "
        "rm -f /var/mobile/Library/RootTools/files/no-follow-test; "
        "ln -s /etc/passwd /var/mobile/Library/RootTools/files/no-follow-test"
    )
    subprocess.run(
        [sys.executable, str(ROOT / "Scripts/root_exec.py"), "--udid", udid, "exec", command],
        cwd=ROOT,
        check=True,
        stdout=subprocess.DEVNULL,
    )


def run_regression(
    client: DeviceServiceClient,
    admin_client: DeviceServiceClient,
    port: int,
    udid: str,
    full: bool,
) -> None:
    status = client.status()
    require(status.get("daemonVersion") == "0.10.0", f"expected daemon 0.10.0, got {status}")
    require(status.get("uid") == 0, f"daemon must be UID 0: {status}")
    require(status.get("jailbreakRootless") is True, f"rootless bootstrap is unavailable: {status}")
    step("daemon identity", f"v{status['daemonVersion']} uid={status['uid']}")

    lock_state = client.lock_state()
    require(lock_state.get("lockState") in {"locked", "unlocked", "unknown"}, f"invalid lock state: {lock_state}")
    require(lock_state.get("headlessExecutionReady") is True, f"headless execution unexpectedly unavailable: {lock_state}")
    step(
        "lock-aware state",
        f"lock={lock_state.get('lockState')} screen={lock_state.get('screenState')} uiReady={lock_state.get('uiExecutionReady')}",
    )

    automation_state = client.automation_state()
    require(automation_state.get("mode") == "lock-aware", f"unexpected automation mode: {automation_state}")
    require(automation_state.get("policy", {}).get("bypassDevicePasscode") is False, f"unsafe lock policy: {automation_state}")
    require(automation_state.get("policy", {}).get("uiJobsWaitForUnlock") is True, f"deferred UI policy missing: {automation_state}")
    step("headless automation policy", "locked execution allowed; UI jobs wait for unlock")

    if lock_state.get("locked") is True or lock_state.get("screenBlanked") is True:
        queued = client.action("device.automation.queue-app-launch", {"bundleID": REFERENCE_APP})
        assert_receipt(queued, "device.automation.queue-app-launch", ok=True, executed=True, policy="allow", result="queued")
        job_id = queued.get("output")
        require(bool(job_id), f"queued job ID missing: {queued}")
        queue_payload = client.automation_queue()
        pending = next((item for item in queue_payload.get("jobs", []) if item.get("jobId") == job_id), None)
        require(pending is not None and pending.get("state") == "pending", f"locked UI job did not remain pending: {queue_payload}")
        cancelled = client.action("device.automation.cancel", {"jobID": job_id})
        assert_receipt(cancelled, "device.automation.cancel", ok=True, executed=True, policy="allow", result="success")
        step("deferred UI queue", "locked app launch persisted then cancelled without launching")

    require(unauthorized_status(port) == 401, "missing token did not return HTTP 401")
    step("loopback authentication", "missing token -> 401")

    catalog = client.capabilities()
    by_id = {item["id"]: item for item in catalog.get("capabilities", [])}
    require(by_id.get("device.app.launch", {}).get("risk") == "R1", "R1 app capability missing")
    require(by_id.get("device.process.terminate", {}).get("requiresConfirmation") is True, "R2 confirmation metadata missing")
    require(by_id.get("device.raw-shell", {}).get("enabled") is False, "raw shell unexpectedly enabled")
    require(
        catalog.get("invariants") == {"r3Exposed": False, "rawPrivilegedShellExposed": False},
        f"unsafe capability invariants: {catalog.get('invariants')}",
    )
    step("capability truth", f"{len(by_id)} registry entries; R3/raw-shell blocked")

    providers = client.providers()
    provider_by_id = {item["id"]: item for item in providers.get("providers", [])}
    require(provider_by_id.get("jailbreak.dopamine", {}).get("state") == "available", f"Dopamine provider unavailable: {provider_by_id.get('jailbreak.dopamine')}")
    require(provider_by_id.get("roottools.updater", {}).get("state") == "available", f"independent updater unavailable: {provider_by_id.get('roottools.updater')}")
    require("package.trollstore" in provider_by_id, "TrollStore provider missing")
    require("runtime.frida" in provider_by_id and "ui.zxtouch" in provider_by_id, "runtime/UI providers missing")
    deb_plan = client.package_plan("deb")
    ipa_plan = client.package_plan("ipa")
    require(deb_plan.get("selectedProviderId") == "bootstrap.procursus", f"unexpected deb provider plan: {deb_plan}")
    require(ipa_plan.get("selectedProviderId") == "package.trollstore", f"unexpected ipa provider plan: {ipa_plan}")
    step("provider plane", f"{len(provider_by_id)} providers; deb={deb_plan['selectedProviderId']} ipa={ipa_plan['selectedProviderId']}")
    frida_runtime = client.frida_status()
    require(frida_runtime.get("providerId") == "runtime.frida", f"unexpected Frida runtime payload: {frida_runtime}")
    require(frida_runtime.get("policy", {}).get("scriptExecutionExposed") is False, "Frida script execution unexpectedly exposed")
    require(frida_runtime.get("policy", {}).get("arbitraryAttachExposed") is False, "Frida arbitrary attach unexpectedly exposed")
    if status.get("fridaReady"):
        require(frida_runtime.get("protocolReachable") is True, f"Frida status disagrees with device status: {frida_runtime}")
        require(frida_runtime.get("process", {}).get("running") is True, f"Frida server process not observed: {frida_runtime}")
    ellekit_runtime = client.ellekit_status()
    require(ellekit_runtime.get("providerId") == "runtime.ellekit", f"unexpected ElleKit runtime payload: {ellekit_runtime}")
    require(ellekit_runtime.get("policy", {}).get("rawHookAPIExposed") is False, "ElleKit raw hook API unexpectedly exposed")
    require(ellekit_runtime.get("policy", {}).get("arbitraryInjectionExposed") is False, "ElleKit arbitrary injection unexpectedly exposed")
    step(
        "runtime observation",
        f"frida={frida_runtime.get('state')} version={frida_runtime.get('package', {}).get('version')} "
        f"ellekit={ellekit_runtime.get('state')} version={ellekit_runtime.get('package', {}).get('version')}",
    )
    package_catalog = client.packages()
    require(package_catalog.get("schemaVersion") == 1, f"package catalog unavailable: {package_catalog}")
    step("package controller", f"{package_catalog.get('count', 0)} staged package records")
    update_status = client.self_update_status()
    require(update_status.get("schemaVersion") == 1, f"self-update status unavailable: {update_status}")
    step("self updater", f"{update_status.get('count', 0)} recorded update requests")

    if status.get("zxTouchReady"):
        screen = client.screen_info().get("screen", {})
        require(float(screen.get("width", 0)) > 0 and float(screen.get("height", 0)) > 0, f"invalid ZXTouch screen geometry: {screen}")
        require(screen.get("implementation") == "ios.zxtouch", f"unexpected screen adapter: {screen}")
        step("ZXTouch typed adapter", f"{screen['width']}×{screen['height']} scale={screen['scale']}")

    try:
        client.request(
            "POST",
            "/v1/capabilities/set",
            {"capabilityId": "device.app.launch", "enabled": False},
        )
        raise VerificationFailure("Agent credential unexpectedly changed owner capability policy")
    except DeviceServiceError as error:
        require("HTTP 403" in str(error), f"Agent policy mutation did not fail closed: {error}")
    step("Agent permission boundary", "Agent token cannot mutate capability policy")

    try:
        admin_client.request(
            "POST",
            "/v1/capabilities/set",
            {"capabilityId": "device.raw-shell", "enabled": True},
        )
        raise VerificationFailure("Admin unexpectedly enabled hard-blocked raw shell")
    except DeviceServiceError as error:
        require("HTTP 403" in str(error), f"R3 policy mutation did not fail closed: {error}")
    step("Owner hard-policy boundary", "Admin cannot enable R3/raw-shell")

    # Verify owner policy really gates Agent execution, then always restore it.
    admin_client.request(
        "POST",
        "/v1/capabilities/set",
        {"capabilityId": "device.app.launch", "enabled": False},
    )
    try:
        disabled_catalog = client.capabilities()
        disabled = {item["id"]: item for item in disabled_catalog["capabilities"]}["device.app.launch"]
        require(disabled.get("hardEnabled") is True and disabled.get("enabled") is False, f"owner policy not projected: {disabled}")
        denied_by_owner = client.action("device.app.launch", {"bundleID": REFERENCE_APP})
        assert_receipt(denied_by_owner, "device.app.launch", ok=False, executed=False, policy="deny", result="denied")
        step("Owner capability disable", "Agent execution denied before executor")
    finally:
        admin_client.request(
            "POST",
            "/v1/capabilities/set",
            {"capabilityId": "device.app.launch", "enabled": True},
        )
    restored = {item["id"]: item for item in client.capabilities()["capabilities"]}["device.app.launch"]
    require(restored.get("enabled") is True, f"app launch policy was not restored: {restored}")
    step("Owner capability restore", "Agent execution surface restored")

    no_confirm = client.action("device.process.terminate", {"pid": 101}, confirmed=False)
    assert_receipt(
        no_confirm,
        "device.process.terminate",
        ok=False,
        executed=False,
        policy="confirmation_required",
        result="confirmation_required",
    )
    step("R2 daemon confirmation", "unconfirmed request denied before execution")

    spoofed_confirmation = client.action("device.process.terminate", {"pid": 101}, confirmed=True)
    assert_receipt(
        spoofed_confirmation,
        "device.process.terminate",
        ok=False,
        executed=False,
        policy="confirmation_required",
        result="confirmation_required",
    )
    step("R2 Agent self-confirmation", "forged confirmed=true remains denied")

    r3 = client.action("device.raw-shell", {}, confirmed=True)
    assert_receipt(r3, "device.raw-shell", ok=False, executed=False, policy="deny", result="denied")
    step("R3 hard block", "raw privileged shell denied")

    invalid_path = client.action(
        "device.fs.write",
        {"scope": "mobile", "name": "../escape", "content": "must-not-write"},
    )
    assert_receipt(invalid_path, "device.fs.write", ok=False, executed=False, policy="allow")
    step("filesystem traversal guard", "invalid name rejected before open")

    replay_id = f"physical-replay-{uuid.uuid4().hex}"
    replay_first = client.action("device.raw-shell", {}, confirmed=True, request_id=replay_id)
    replay_second = client.action("device.raw-shell", {}, confirmed=True, request_id=replay_id)
    assert_receipt(replay_first, "device.raw-shell", ok=False, executed=False, policy="deny", result="denied")
    assert_receipt(replay_second, "device.raw-shell", ok=False, executed=False, policy="deny", result="denied")
    require(replay_first.get("replayed") is False, f"first request unexpectedly replayed: {replay_first}")
    require(replay_second.get("replayed") is True, f"retry did not replay durable receipt: {replay_second}")
    require(replay_second["auditId"] == replay_first["auditId"], "replay changed audit identity")
    step("durable idempotency", "same requestId returns original receipt without execution")

    if not full:
        return

    marker = f"roottools-v03-{uuid.uuid4().hex[:12]}"
    audit_ids: list[str] = [
        no_confirm["auditId"], spoofed_confirmation["auditId"], r3["auditId"], invalid_path["auditId"],
        replay_first["auditId"]
    ]
    for scope in ("mobile", "bootstrap"):
        name = "p1-validation.txt"
        write = client.action("device.fs.write", {"scope": scope, "name": name, "content": marker})
        assert_receipt(write, "device.fs.write", ok=True, executed=True, policy="allow", result="success")
        require(write.get("postCondition", {}).get("passed") is True, f"write post-condition failed: {write}")
        read = client.action("device.fs.read", {"scope": scope, "name": name})
        assert_receipt(read, "device.fs.read", ok=True, executed=True, policy="allow", result="success")
        require(read.get("output") == marker, f"file round-trip mismatch in {scope}: {read}")
        audit_ids.extend([write["auditId"], read["auditId"]])
        step("scoped file round-trip", scope)

    create_symlink_fixture(udid)
    no_follow = client.action("device.fs.read", {"scope": "mobile", "name": "no-follow-test"})
    assert_receipt(no_follow, "device.fs.read", ok=False, executed=True, policy="allow")
    audit_ids.append(no_follow["auditId"])
    step("filesystem symlink guard", "O_NOFOLLOW rejected fixture")

    launch = client.action("device.app.launch", {"bundleID": REFERENCE_APP})
    assert_receipt(launch, "device.app.launch", ok=True, executed=True, policy="allow", result="success")
    require(launch.get("postCondition", {}).get("passed") is True, f"app launch was not verified: {launch}")
    audit_ids.append(launch["auditId"])
    step("application launch", REFERENCE_APP)

    terminate = client.action("device.app.terminate", {"bundleID": REFERENCE_APP})
    assert_receipt(terminate, "device.app.terminate", ok=True, executed=True, policy="allow", result="success")
    require(terminate.get("postCondition", {}).get("passed") is True, f"app terminate was not verified: {terminate}")
    audit_ids.append(terminate["auditId"])
    step("application terminate", REFERENCE_APP)

    processes = client.text("/v1/processes").get("output", "")
    daemon_row = process_row(processes, "roottools-execd")
    require(daemon_row is not None, "roottools-execd missing from process list")
    daemon_pid, daemon_uid = daemon_row
    require(daemon_uid == 0, f"roottools-execd must be UID 0, got uid={daemon_uid}")
    daemon_denial = admin_client.action("device.process.terminate", {"pid": daemon_pid}, confirmed=True)
    assert_receipt(daemon_denial, "device.process.terminate", ok=False, executed=False, policy="allow", result="denied")
    audit_ids.append(daemon_denial["auditId"])
    step("privileged daemon self-protection", f"pid={daemon_pid} uid=0 denied")

    ui_launch = client.action("device.app.launch", {"bundleID": ROOTTOOLS_APP})
    assert_receipt(ui_launch, "device.app.launch", ok=True, executed=True, policy="allow", result="success")
    processes = client.text("/v1/processes").get("output", "")
    ui_row = process_row(processes, "RootTools")
    require(ui_row is not None, "RootTools UI process did not appear")
    ui_pid, ui_uid = ui_row
    require(ui_uid != 0, f"RootTools UI unexpectedly runs as root: uid={ui_uid}")
    ui_kill = admin_client.action("device.process.terminate", {"pid": ui_pid}, confirmed=True)
    assert_receipt(ui_kill, "device.process.terminate", ok=True, executed=True, policy="allow", result="success")
    require(ui_kill.get("postCondition", {}).get("passed") is True, f"UI process termination was not verified: {ui_kill}")
    audit_ids.extend([ui_launch["auditId"], ui_kill["auditId"]])
    step("UI / daemon isolation", f"terminated UI pid={ui_pid} uid={ui_uid}")

    still_alive = client.status()
    require(still_alive.get("uid") == 0 and still_alive.get("daemonVersion") == "0.10.0", "daemon died with UI")
    step("daemon survives UI exit", "Device Service still responds")

    legacy = client.request(
        "POST",
        "/v1/actions/file-read",
        {"scope": "mobile", "name": "p1-validation.txt"},
    )
    assert_receipt(legacy, "device.fs.read", ok=True, executed=True, policy="allow", result="success")
    require(legacy.get("output") == marker, f"legacy compatibility read mismatch: {legacy}")
    audit_ids.append(legacy["auditId"])
    step("v0.2 compatibility adapter", "/v1/actions/file-read -> same router")

    audit = client.text("/v1/audit").get("output", "")
    missing = [audit_id for audit_id in audit_ids if audit_id not in audit]
    require(not missing, f"audit log is missing receipt IDs: {missing}")
    step("append-only audit correlation", f"verified {len(audit_ids)} receipt IDs")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify RootTools on a physical jailbreak iPhone")
    parser.add_argument("--udid", default=os.environ.get("ROOTTOOLS_UDID"))
    parser.add_argument("--token-file", type=Path, default=DEFAULT_TOKEN)
    parser.add_argument("--admin-token-file", type=Path, default=DEFAULT_ADMIN_TOKEN)
    parser.add_argument("--install", action="store_true", help="Build and install RootTools before verification")
    parser.add_argument("--full", action="store_true", help="Run safe mutation/post-condition tests")
    args = parser.parse_args()

    udid = args.udid
    if not udid:
        try:
            udid = subprocess.check_output(["idevice_id", "-l"], text=True).splitlines()[0]
        except (FileNotFoundError, subprocess.CalledProcessError, IndexError):
            try:
                udid = discover_udid()
            except Exception:
                print("No USB iPhone found", file=sys.stderr)
                return 2

    try:
        if args.install:
            install(udid)
            step("install", udid)
        token = load_token(args.token_file)
        admin_token = load_token(args.admin_token_file)
        require(token != admin_token, "Agent and admin credentials must be distinct")
        with device_proxy(udid) as port:
            client = DeviceServiceClient(port, token, caller="physical-verifier")
            admin_client = DeviceServiceClient(port, admin_token, caller="physical-owner-verifier")
            run_regression(client, admin_client, port, udid, args.full)
        print("RootTools physical-device verification: PASS")
        return 0
    except (DeviceServiceError, VerificationFailure, subprocess.CalledProcessError, OSError) as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
