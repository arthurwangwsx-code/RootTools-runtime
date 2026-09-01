#!/usr/bin/env python3
"""Run the repeatable RootTools offline simulator acceptance gate.

The validator builds the personalized App and Mac fixture daemon from one
credential profile, launches the real App in Simulator, proves the authenticated
HTTP identity, captures a screenshot, and writes a secret-free receipt. It is
not a substitute for a jailbroken physical-device qualification.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
VERSION = (ROOT / "VERSION").read_text().strip()
CORE_VERSION, BUILD_VERSION = VERSION.rsplit("-", 1)
PROFILE_PATTERN = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")
TOKEN_PATTERN = re.compile(rb"[0-9a-fA-F]{48}")
PORT = 45821


class SimulatorValidationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SimulatorValidationError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def credential_paths(profile: str) -> tuple[Path, Path]:
    require(bool(PROFILE_PATTERN.fullmatch(profile)) and ".." not in profile, f"invalid credential profile: {profile}")
    if profile == "installed":
        return ROOT / ".roottools-token", ROOT / ".roottools-agent-token"
    base = ROOT / ".roottools-credentials" / profile
    return base / "owner-token", base / "agent-token"


def load_token(path: Path, label: str) -> bytes:
    metadata = path.lstat()
    require(stat.S_ISREG(metadata.st_mode) and not path.is_symlink(), f"{label} must be a regular non-symlink file")
    require(stat.S_IMODE(metadata.st_mode) & 0o077 == 0, f"{label} must be owner-only")
    token = path.read_bytes().strip()
    require(bool(TOKEN_PATTERN.fullmatch(token)), f"{label} must contain exactly 48 hexadecimal characters")
    return token


def run_logged(label: str, command: list[str], log_path: Path, env: dict[str, str] | None = None) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a") as log:
        log.write(f"\n== {label} ==\n")
        log.flush()
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        tail = "\n".join(log_path.read_text(errors="replace").splitlines()[-80:])
        raise SimulatorValidationError(f"{label} failed with exit {result.returncode}\n{tail}")


def simulator(udid: str | None) -> dict:
    payload = json.loads(subprocess.check_output(["xcrun", "simctl", "list", "devices", "available", "-j"], text=True))
    candidates = []
    for runtime, devices in payload.get("devices", {}).items():
        for device in devices:
            if not device.get("isAvailable", True):
                continue
            candidates.append({**device, "runtime": runtime.rsplit(".", 1)[-1].replace("-", ".")})
    if udid:
        matches = [item for item in candidates if item.get("udid") == udid]
        require(bool(matches), f"requested Simulator is not available: {udid}")
        return matches[0]
    require(bool(candidates), "no available iOS Simulator device")
    candidates.sort(key=lambda item: (item.get("state") != "Booted", item.get("name", ""), item.get("runtime", "")))
    return candidates[0]


def request_json(path: str, owner_token: bytes) -> dict:
    request = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}")
    request.add_header("X-RootTools-Token", owner_token.decode())
    with urllib.request.urlopen(request, timeout=2) as response:
        require(response.status == 200, f"fixture daemon HTTP {response.status} for {path}")
        return json.load(response)


def wait_for_daemon(owner_token: bytes) -> dict:
    deadline = time.monotonic() + 15
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            return request_json("/v1/hello", owner_token)
        except (OSError, urllib.error.URLError, json.JSONDecodeError, SimulatorValidationError) as error:
            last_error = error
            time.sleep(0.2)
    raise SimulatorValidationError(f"fixture daemon did not become ready: {last_error}")


def ensure_port_available() -> None:
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.settimeout(0.2)
        require(probe.connect_ex(("127.0.0.1", PORT)) != 0, f"localhost port {PORT} is already in use")
    finally:
        probe.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate RootTools with an iOS Simulator and Mac fixture daemon")
    parser.add_argument("--credential-profile", required=True)
    parser.add_argument("--simulator-udid")
    parser.add_argument("--visual-inspection", choices=("pending", "completed"), default="pending")
    parser.add_argument("--skip-contract-tests", action="store_true")
    args = parser.parse_args()

    require(args.credential_profile != "installed", "offline candidate validation requires an isolated non-installed profile")
    owner_path, agent_path = credential_paths(args.credential_profile)
    owner_token = load_token(owner_path, "Owner token")
    agent_token = load_token(agent_path, "Agent token")
    require(owner_token != agent_token, "Owner and Agent credentials must differ")
    selected = simulator(args.simulator_udid)

    validation_dir = ROOT / "build" / "validation"
    build_log = validation_dir / f"roottools-{VERSION}-simulator-build.log"
    test_log = validation_dir / f"roottools-{VERSION}-simulator-contracts.log"
    daemon_log = validation_dir / f"roottools-{VERSION}-simulator-daemon.log"
    screenshot = validation_dir / f"roottools-{VERSION}-simulator.png"
    receipt_path = validation_dir / f"roottools-{VERSION}-simulator.json"
    validation_dir.mkdir(parents=True, exist_ok=True)
    build_log.write_text("")
    test_log.write_text("")
    daemon_log.write_text("")

    env = {**os.environ, "ROOTTOOLS_CREDENTIAL_PROFILE": args.credential_profile}
    run_logged("personalized device build", ["bash", "Scripts/build.sh"], build_log, env)
    if not args.skip_contract_tests:
        run_logged("contract tests", ["bash", "Scripts/test.sh"], test_log, env)
    app = ROOT / "build/DerivedData-simulator/Build/Products/Release-iphonesimulator/RootTools.app"
    run_logged(
        "Simulator Release build",
        [
            "xcodebuild", "-project", "RootTools.xcodeproj", "-scheme", "RootTools", "-configuration", "Release",
            "-sdk", "iphonesimulator", "-destination", f"platform=iOS Simulator,id={selected['udid']}",
            "-derivedDataPath", "build/DerivedData-simulator", f"MARKETING_VERSION={CORE_VERSION}",
            f"CURRENT_PROJECT_VERSION={BUILD_VERSION}", "CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO", "build",
        ],
        build_log,
        env,
    )
    require((app / "RootTools").is_file(), f"Simulator App executable missing: {app}")
    with (app / "Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    require(info.get("CFBundleIdentifier") == "com.arthur.roottools.ios", "Simulator App bundle identifier mismatch")
    require(info.get("CFBundleShortVersionString") == CORE_VERSION, "Simulator App version mismatch")
    require(str(info.get("CFBundleVersion")) == BUILD_VERSION, "Simulator App build mismatch")

    ensure_port_available()
    daemon: subprocess.Popen | None = None
    daemon_stream = None
    hello: dict = {}
    status: dict = {}
    launch_output = ""
    app_container = ""
    with tempfile.TemporaryDirectory(prefix="roottools-simulator-daemon-") as temporary:
        fixture = Path(temporary)
        runtime_agent = fixture / "agent-token"
        shutil.copyfile(agent_path, runtime_agent)
        runtime_agent.chmod(0o600)
        daemon_env = {
            **env,
            "ROOTTOOLS_PORT": str(PORT),
            "ROOTTOOLS_POLICY_DIR": str(fixture / "policy"),
            "ROOTTOOLS_AUDIT_PATH": str(fixture / "audit.log"),
            "ROOTTOOLS_LEDGER_PATH": str(fixture / "ledger.sqlite3"),
            "ROOTTOOLS_AGENT_TOKEN_PATH": str(runtime_agent),
            "ROOTTOOLS_MOBILE_SCOPE_ROOT": str(fixture / "mobile"),
            "ROOTTOOLS_BOOTSTRAP_SCOPE_ROOT": str(fixture / "bootstrap"),
            "ROOTTOOLS_PACKAGE_ROOT": str(fixture / "packages"),
            "ROOTTOOLS_PACKAGE_DB": str(fixture / "packages.sqlite3"),
            "ROOTTOOLS_UPDATE_DB": str(fixture / "update.sqlite3"),
            "ROOTTOOLS_PRINCIPAL_DB": str(fixture / "principal.sqlite3"),
            "ROOTTOOLS_REMOTE_WORKER_STATE_PATH": str(fixture / "remote-worker.conf"),
            "ROOTTOOLS_REMOTE_ACCESS_STATE_PATH": str(fixture / "remote-access.conf"),
            "ROOTTOOLS_TEST_TAILNET_IPV4": "127.0.0.1",
            "ROOTTOOLS_TEST_LOCK_STATE": "unlocked",
            "ROOTTOOLS_TEST_SCREEN_BLANKED": "0",
            "ROOTTOOLS_TEST_BATTERY_PERCENT": "82",
            "ROOTTOOLS_TEST_BATTERY_TEMPERATURE_CENTI_C": "3350",
            "ROOTTOOLS_TEST_EXTERNAL_POWER": "1",
            "ROOTTOOLS_TEST_IS_CHARGING": "0",
        }
        try:
            daemon_stream = daemon_log.open("w")
            daemon = subprocess.Popen(
                [str(ROOT / "build/tests/roottools-execd-mac")],
                cwd=ROOT,
                env=daemon_env,
                stdout=subprocess.DEVNULL,
                stderr=daemon_stream,
            )
            hello = wait_for_daemon(owner_token)
            require(hello.get("daemonVersion") == CORE_VERSION, f"fixture daemon core mismatch: {hello}")
            require(hello.get("packageVersion") == VERSION, f"fixture daemon package mismatch: {hello}")
            require(hello.get("authenticatedRole") == "owner", f"fixture Owner authentication failed: {hello}")

            if selected.get("state") != "Booted":
                subprocess.run(["xcrun", "simctl", "boot", selected["udid"]], check=True)
            run_logged("Simulator boot", ["xcrun", "simctl", "bootstatus", selected["udid"], "-b"], build_log)
            run_logged("Simulator install", ["xcrun", "simctl", "install", selected["udid"], str(app)], build_log)
            launched = subprocess.run(
                ["xcrun", "simctl", "launch", "--terminate-running-process", selected["udid"], "com.arthur.roottools.ios"],
                check=True,
                capture_output=True,
                text=True,
            )
            launch_output = launched.stdout.strip()
            require(bool(re.fullmatch(r"com\.arthur\.roottools\.ios: [0-9]+", launch_output)), f"unexpected launch receipt: {launch_output}")
            time.sleep(5)
            run_logged("Simulator screenshot", ["xcrun", "simctl", "io", selected["udid"], "screenshot", str(screenshot)], build_log)
            app_container = subprocess.check_output(
                ["xcrun", "simctl", "get_app_container", selected["udid"], "com.arthur.roottools.ios", "app"], text=True
            ).strip()
            require(Path(app_container).is_dir(), "installed Simulator App container is unavailable")
            status = request_json("/v1/status", owner_token)
            require(status.get("daemonVersion") == CORE_VERSION, f"status core mismatch: {status}")
            require(status.get("packageVersion") == VERSION, f"status package mismatch: {status}")
        finally:
            if daemon is not None and daemon.poll() is None:
                daemon.terminate()
                try:
                    daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait(timeout=5)
            if daemon_stream is not None:
                daemon_stream.close()

    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    tree = subprocess.check_output(["git", "rev-parse", "HEAD^{tree}"], cwd=ROOT, text=True).strip()
    receipt = {
        "schemaVersion": 1,
        "status": "passed",
        "validatedAt": int(time.time()),
        "scope": {
            "contractTests": "passed" if not args.skip_contract_tests else "not-run",
            "iphoneosReleaseBuild": "passed",
            "simulatorReleaseBuild": "passed",
            "simulatorInstallAndLaunch": "passed",
            "authenticatedFixtureDaemon": "passed",
            "visualInspection": args.visual_inspection,
            "physicalJailbrokenDevice": "pending-not-substituted",
        },
        "source": {"commit": commit, "tree": tree, "version": VERSION},
        "credentialProfile": {
            "name": args.credential_profile,
            "ownerFingerprint": hashlib.sha256(owner_token).hexdigest(),
            "agentFingerprint": hashlib.sha256(agent_token).hexdigest(),
        },
        "simulator": {
            "name": selected.get("name"),
            "udid": selected.get("udid"),
            "runtime": selected.get("runtime"),
            "launchReceipt": launch_output,
            "appContainer": app_container,
        },
        "app": {
            "bundleIdentifier": info.get("CFBundleIdentifier"),
            "shortVersion": info.get("CFBundleShortVersionString"),
            "buildVersion": str(info.get("CFBundleVersion")),
            "executableSha256": sha256(app / "RootTools"),
        },
        "daemon": {
            "daemonVersion": hello.get("daemonVersion"),
            "packageVersion": hello.get("packageVersion"),
            "authenticatedRole": hello.get("authenticatedRole"),
            "statusMachine": status.get("machine"),
            "statusUid": status.get("uid"),
        },
        "evidence": {
            "screenshot": str(screenshot.resolve()),
            "screenshotSha256": sha256(screenshot),
            "buildLog": str(build_log.resolve()),
            "contractLog": str(test_log.resolve()),
            "daemonLog": str(daemon_log.resolve()),
        },
        "boundary": "This receipt proves local contracts and a Simulator-to-Mac fixture path only; it does not qualify Dopamine, launchd, TrollStore, USB, Tailnet, or rollback on a physical iPhone.",
    }
    serialized = json.dumps(receipt, indent=2, sort_keys=True) + "\n"
    for secret in (owner_token, agent_token):
        require(secret.decode() not in serialized, "credential leaked into Simulator receipt")
        for evidence_path in (build_log, test_log, daemon_log):
            require(secret not in evidence_path.read_bytes(), f"credential leaked into validation log: {evidence_path.name}")
    receipt_path.write_text(serialized)
    receipt_path.chmod(0o600)
    print(json.dumps({"status": "passed", "receipt": str(receipt_path), "screenshot": str(screenshot), "simulator": selected["name"]}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError, urllib.error.URLError, json.JSONDecodeError, SimulatorValidationError) as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        raise SystemExit(1)
