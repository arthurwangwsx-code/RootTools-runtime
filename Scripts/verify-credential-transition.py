#!/usr/bin/env python3
"""Verify candidate credentials and old-credential rejection on one USB iPhone."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import time

from device_service import DeviceServiceClient, DeviceServiceError, device_proxy, load_token
from usbmux_proxy import discover_udid


ROOT = Path(__file__).resolve().parent.parent
VERSION = (ROOT / "VERSION").read_text().strip()
CORE_VERSION = VERSION.rsplit("-", 1)[0]
DEFAULT_STATE = ROOT / "build/qualification/credential-migration-state.json"
PROFILE_PATTERN = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")


class TransitionFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TransitionFailure(message)


def credential_paths(profile: str) -> tuple[Path, Path]:
    require(bool(PROFILE_PATTERN.fullmatch(profile)) and ".." not in profile, f"invalid credential profile: {profile}")
    if profile == "installed":
        return ROOT / ".roottools-token", ROOT / ".roottools-agent-token"
    directory = ROOT / ".roottools-credentials" / profile
    return directory / "owner-token", directory / "agent-token"


def rejected(client: DeviceServiceClient, label: str) -> None:
    try:
        client.hello()
    except DeviceServiceError as error:
        require(error.status_code in {401, 403}, f"{label} failed for unexpected reason: {error}")
        return
    raise TransitionFailure(f"{label} remains accepted after credential transition")


def write_state(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(payload, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(0o600)
        os.replace(temporary, path)
        path.chmod(0o600)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify a RootTools physical credential transition")
    parser.add_argument("--active-profile", required=True)
    parser.add_argument("--retired-profile", default="installed")
    parser.add_argument("--udid")
    parser.add_argument("--state-file", type=Path, default=DEFAULT_STATE)
    args = parser.parse_args()
    require(args.active_profile != args.retired_profile, "active and retired profiles must differ")
    require(args.state_file.is_file(), f"offline migration state is missing: {args.state_file}")
    metadata = args.state_file.stat()
    require(stat.S_IMODE(metadata.st_mode) & 0o077 == 0, "migration state must be owner-only")
    state = json.loads(args.state_file.read_text())
    require(state.get("phase") == "offline-prepared", f"unexpected migration phase: {state.get('phase')}")
    require(state.get("candidate", {}).get("version") == VERSION, "migration candidate version does not match repository VERSION")
    require(state.get("target", {}).get("profile") == args.active_profile, "migration target profile mismatch")
    require(state.get("source", {}).get("profile") == args.retired_profile, "migration source profile mismatch")

    active_owner_path, active_agent_path = credential_paths(args.active_profile)
    retired_owner_path, retired_agent_path = credential_paths(args.retired_profile)
    active_owner = load_token(active_owner_path)
    active_agent = load_token(active_agent_path)
    retired_owner = load_token(retired_owner_path)
    retired_agent = load_token(retired_agent_path)
    require(len({active_owner, active_agent, retired_owner, retired_agent}) == 4, "transition credentials must all be distinct")
    udid = args.udid or discover_udid()

    with device_proxy(udid) as port:
        owner = DeviceServiceClient(port, active_owner, caller="credential-transition-owner")
        agent = DeviceServiceClient(port, active_agent, caller="credential-transition-agent")
        owner_hello = owner.hello()
        agent_hello = agent.hello()
        require(owner_hello.get("authenticatedRole") == "owner", f"candidate Owner authentication failed: {owner_hello}")
        require(agent_hello.get("authenticatedRole") == "agent", f"candidate Agent authentication failed: {agent_hello}")
        require(owner_hello.get("daemonVersion") == CORE_VERSION, f"candidate daemon version mismatch: {owner_hello}")
        require(owner_hello.get("packageVersion") == VERSION, f"candidate package version mismatch: {owner_hello}")
        status = owner.status()
        require(status.get("uid") == 0, f"candidate daemon is not UID 0: {status}")
        require(status.get("jailbreakRootless") is True, f"candidate rootless runtime unavailable: {status}")
        rejected(DeviceServiceClient(port, retired_owner, caller="credential-transition-old-owner"), "retired Owner credential")
        rejected(DeviceServiceClient(port, retired_agent, caller="credential-transition-old-agent"), "retired Agent credential")

    state["phase"] = "physical-credential-transition-verified"
    state["realDevice"] = {
        **state.get("realDevice", {}),
        "status": "credential-transition-verified",
        "deviceUdid": udid,
        "verifiedAt": int(time.time()),
        "candidateOwnerAccepted": True,
        "candidateAgentAccepted": True,
        "installedOwnerRejected": True,
        "installedAgentRejected": True,
        "daemonVersion": CORE_VERSION,
        "packageVersion": VERSION,
        "uid": 0,
        "rootless": True,
    }
    serialized = json.dumps(state, sort_keys=True)
    for secret in (active_owner, active_agent, retired_owner, retired_agent):
        require(secret not in serialized, "credential leaked into physical migration state")
    write_state(args.state_file, state)
    print(json.dumps({"status": "passed", "phase": state["phase"], "packageVersion": VERSION}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (DeviceServiceError, TransitionFailure, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        raise SystemExit(1)
