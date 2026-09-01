#!/usr/bin/env python3
"""Prepare a secret-free credential migration ledger from verified artifacts.

This command is deliberately offline-only. It proves that candidate artifacts
contain the target profile, exclude the installed profile, and preserve an
exact rollback artifact. It never installs, rotates, deletes, or prints a
credential. Physical state is left pending for the dedicated runbook.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import stat
import subprocess
import sys
import tempfile
import time
import zipfile


ROOT = Path(__file__).resolve().parent.parent
VERSION = (ROOT / "VERSION").read_text().strip()
DEFAULT_OUTPUT = ROOT / "build/qualification/credential-migration-state.json"
TOKEN_PATTERN = re.compile(r"[0-9a-fA-F]{48}")
PROFILE_PATTERN = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")


class MigrationPreparationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise MigrationPreparationError(message)


def credential_paths(profile: str) -> tuple[Path, Path]:
    require(bool(PROFILE_PATTERN.fullmatch(profile)) and ".." not in profile, f"invalid credential profile: {profile}")
    if profile == "installed":
        return ROOT / ".roottools-token", ROOT / ".roottools-agent-token"
    directory = ROOT / ".roottools-credentials" / profile
    return directory / "owner-token", directory / "agent-token"


def load_token(path: Path, label: str) -> bytes:
    try:
        metadata = path.lstat()
    except OSError as error:
        raise MigrationPreparationError(f"{label} unavailable: {path}: {error}") from error
    require(stat.S_ISREG(metadata.st_mode) and not path.is_symlink(), f"{label} must be a regular non-symlink file: {path}")
    require(stat.S_IMODE(metadata.st_mode) & 0o077 == 0, f"{label} must be owner-only: {path}")
    value = path.read_text().strip()
    require(bool(TOKEN_PATTERN.fullmatch(value)), f"{label} must contain exactly 48 hexadecimal characters: {path}")
    return value.encode()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def profile_record(profile: str) -> tuple[dict, bytes, bytes]:
    owner_path, agent_path = credential_paths(profile)
    owner = load_token(owner_path, f"{profile} Owner token")
    agent = load_token(agent_path, f"{profile} Agent token")
    require(owner != agent, f"{profile} Owner and Agent credentials must differ")
    return (
        {
            "profile": profile,
            "ownerTokenFile": str(owner_path.resolve()),
            "agentTokenFile": str(agent_path.resolve()),
            "ownerFingerprint": hashlib.sha256(owner).hexdigest(),
            "agentFingerprint": hashlib.sha256(agent).hexdigest(),
        },
        owner,
        agent,
    )


def scan_payload(files: dict[str, bytes], source_owner: bytes, source_agent: bytes, target_owner: bytes, target_agent: bytes) -> dict:
    source_hits = sorted(name for name, data in files.items() if source_owner in data or source_agent in data)
    target_owner_hits = sorted(name for name, data in files.items() if target_owner in data)
    target_agent_hits = sorted(name for name, data in files.items() if target_agent in data)
    return {
        "sourceCredentialAbsent": not source_hits,
        "sourceCredentialHitFiles": source_hits,
        "targetOwnerHitFiles": target_owner_hits,
        "targetAgentHitFiles": target_agent_hits,
    }


def inspect_deb(path: Path, source_owner: bytes, source_agent: bytes, target_owner: bytes, target_agent: bytes) -> dict:
    require(path.is_file(), f"candidate DEB missing: {path}")
    package = subprocess.check_output(["dpkg-deb", "-f", str(path), "Package"], text=True).strip()
    version = subprocess.check_output(["dpkg-deb", "-f", str(path), "Version"], text=True).strip()
    architecture = subprocess.check_output(["dpkg-deb", "-f", str(path), "Architecture"], text=True).strip()
    require(package == "com.arthur.roottools", f"candidate DEB package mismatch: {package}")
    require(version == VERSION, f"candidate DEB version mismatch: expected {VERSION}, got {version}")
    require(architecture == "iphoneos-arm64", f"candidate DEB architecture mismatch: {architecture}")
    with tempfile.TemporaryDirectory(prefix="roottools-migration-deb-") as temporary:
        destination = Path(temporary)
        subprocess.run(["dpkg-deb", "-x", str(path), str(destination)], check=True)
        files = {
            file.relative_to(destination).as_posix(): file.read_bytes()
            for file in destination.rglob("*")
            if file.is_file() and not file.is_symlink()
        }
    scan = scan_payload(files, source_owner, source_agent, target_owner, target_agent)
    require(scan["sourceCredentialAbsent"], f"candidate DEB still contains installed credentials: {scan['sourceCredentialHitFiles']}")
    require(any(name.endswith("RootTools.app/RootTools") for name in scan["targetOwnerHitFiles"]), "candidate App does not contain target Owner credential")
    require(any(name.endswith("roottools-execd") for name in scan["targetOwnerHitFiles"]), "candidate daemon does not contain target Owner credential")
    require(any(name.endswith("roottools-updater") for name in scan["targetOwnerHitFiles"]), "candidate updater does not contain target Owner credential")
    require(any(name.endswith("roottools-execd") for name in scan["targetAgentHitFiles"]), "candidate daemon does not contain target Agent credential")
    return {
        "name": path.name,
        "path": str(path.resolve()),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
        "package": package,
        "version": version,
        "architecture": architecture,
        "credentialScan": scan,
    }


def inspect_ipa(path: Path, source_owner: bytes, source_agent: bytes, target_owner: bytes, target_agent: bytes) -> dict:
    require(path.is_file(), f"candidate IPA missing: {path}")
    with zipfile.ZipFile(path) as archive:
        require(archive.testzip() is None, "candidate IPA CRC verification failed")
        names = archive.namelist()
        require("Payload/RootTools.app/Info.plist" in names, "candidate IPA has no RootTools Info.plist")
        require("Payload/RootTools.app/RootTools" in names, "candidate IPA has no RootTools executable")
        for name in names:
            member = Path(name)
            require(not member.is_absolute() and ".." not in member.parts, f"unsafe IPA member: {name}")
        info = plistlib.loads(archive.read("Payload/RootTools.app/Info.plist"))
        files = {name: archive.read(name) for name in names if not name.endswith("/")}
    require(info.get("CFBundleIdentifier") == "com.arthur.roottools.ios", f"candidate IPA bundle mismatch: {info}")
    expected_core, expected_build = VERSION.rsplit("-", 1)
    require(info.get("CFBundleShortVersionString") == expected_core, f"candidate IPA version mismatch: {info}")
    require(str(info.get("CFBundleVersion")) == expected_build, f"candidate IPA build mismatch: {info}")
    scan = scan_payload(files, source_owner, source_agent, target_owner, target_agent)
    require(scan["sourceCredentialAbsent"], f"candidate IPA still contains installed credentials: {scan['sourceCredentialHitFiles']}")
    require("Payload/RootTools.app/RootTools" in scan["targetOwnerHitFiles"], "candidate IPA does not contain target Owner credential")
    require(not scan["targetAgentHitFiles"], "foreground IPA must not contain the target Agent credential")
    return {
        "name": path.name,
        "path": str(path.resolve()),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
        "bundleIdentifier": info["CFBundleIdentifier"],
        "shortVersion": info["CFBundleShortVersionString"],
        "buildVersion": str(info["CFBundleVersion"]),
        "credentialScan": scan,
    }


def inspect_rollback(path: Path, source_owner: bytes, source_agent: bytes) -> dict:
    require(path.is_file(), f"rollback DEB missing: {path}")
    with tempfile.TemporaryDirectory(prefix="roottools-rollback-deb-") as temporary:
        destination = Path(temporary)
        subprocess.run(["dpkg-deb", "-x", str(path), str(destination)], check=True)
        payload = b"".join(
            file.read_bytes()
            for file in destination.rglob("*")
            if file.is_file() and not file.is_symlink()
        )
    require(source_owner in payload, "rollback DEB does not contain the installed Owner credential")
    require(source_agent in payload, "rollback DEB does not contain the installed Agent credential")
    return {
        "name": path.name,
        "path": str(path.resolve()),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
        "package": subprocess.check_output(["dpkg-deb", "-f", str(path), "Package"], text=True).strip(),
        "version": subprocess.check_output(["dpkg-deb", "-f", str(path), "Version"], text=True).strip(),
        "credentialMatch": "installed-profile",
    }


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


def prepare(args: argparse.Namespace) -> dict:
    require(args.from_profile != args.to_profile, "source and target credential profiles must differ")
    source, source_owner, source_agent = profile_record(args.from_profile)
    target, target_owner, target_agent = profile_record(args.to_profile)
    require(source["ownerFingerprint"] != target["ownerFingerprint"], "target Owner credential matches installed credential")
    require(source["agentFingerprint"] != target["agentFingerprint"], "target Agent credential matches installed credential")
    candidate_deb = inspect_deb(args.deb, source_owner, source_agent, target_owner, target_agent)
    candidate_ipa = inspect_ipa(args.ipa, source_owner, source_agent, target_owner, target_agent)
    rollback = inspect_rollback(args.rollback_deb, source_owner, source_agent)
    commit = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    tree = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD^{tree}"], text=True).strip()
    state = {
        "schemaVersion": 1,
        "phase": "offline-prepared",
        "preparedAt": int(time.time()),
        "source": source,
        "target": target,
        "candidate": {"version": VERSION, "sourceCommit": commit, "sourceTree": tree, "deb": candidate_deb, "ipa": candidate_ipa},
        "rollback": rollback,
        "offlineEvidence": {
            "credentialsDiffer": True,
            "candidateContainsTargetCredentials": True,
            "candidateExcludesInstalledCredentials": True,
            "rollbackContainsInstalledCredentials": True,
            "artifactIntegrityVerified": True,
        },
        "realDevice": {
            "status": "pending",
            "deviceUdid": None,
            "requiredGates": [
                "installed-profile preflight authentication and version receipt",
                "candidate USB install from exact DEB or exact source/build manifest",
                "target Owner and Agent authentication",
                "installed Owner and Agent rejection",
                "packageVersion and daemonVersion identity",
                "full typed Device Service regression",
                "off-USB Tailnet prepare/verify/stop/expiry/revoke",
                "rollback rehearsal or explicit rollback waiver",
            ],
            "receipts": [],
        },
    }
    serialized = json.dumps(state, sort_keys=True)
    for secret in (source_owner, source_agent, target_owner, target_agent):
        require(secret.decode() not in serialized, "credential leaked into migration evidence")
    write_state(args.output, state)
    return state


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare an offline RootTools credential migration ledger")
    parser.add_argument("--from-profile", default="installed")
    parser.add_argument("--to-profile", required=True)
    parser.add_argument("--deb", type=Path, required=True)
    parser.add_argument("--ipa", type=Path, required=True)
    parser.add_argument("--rollback-deb", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    try:
        result = prepare(args)
    except (MigrationPreparationError, OSError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
