#!/usr/bin/env python3
"""One-time trusted migration from the physical RootTools v0.9 bootstrap.

The physical v0.9 updater has two rootless-bootstrap defects: its launchd
environment cannot resolve Procursus `tar`, and it addresses the daemon in the
macOS-style `system` domain instead of Dopamine's foreground-user domain.

The migration deliberately keeps authorization and payload trust inside the
existing RootTools model:

1. v0.9 accepts the already-existing Owner R2 `device.self-update.schedule`
   request for a staged, verified `com.arthur.roottools` DEB.
2. The legacy updater is allowed to fail safely during metadata preflight;
   this establishes a durable, audited Owner-approved request without switching
   any target files.
3. The host proves that the local DEB is byte-for-byte the same SHA-256 as the
   staged device record, extracts *only that DEB's* new `roottools-updater`, and
   places the helper in the rootless bootstrap after device-side signing.
4. With the v0.9 daemon stopped, the exact approved request row is changed from
   its terminal legacy-preflight state back to `launching` using a guarded
   offline SQLite transition.
5. The candidate updater runs only `--request <approved-request-id>`. Normal
   package identity validation, extraction allowlist, target switching, health
   verification, rollback and final state recording remain updater-owned.

This is migration tooling for a personally owned development device. It does
not add a model-facing root shell, caller-provided executable path, or generic
update command to the RootTools protocol.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

from device_service import DeviceServiceClient, DeviceServiceError, device_proxy, load_token
from root_exec import push as root_push, run_root
from usbmux_proxy import discover_udid


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ADMIN_TOKEN = ROOT / ".roottools-token"
SERVICE_LABEL = "com.arthur.roottools.execd"
UPDATE_DB = "/var/mobile/Library/RootTools/self-update.sqlite3"
LEGACY_SAFE_PATH = "/usr/bin:/bin:/usr/sbin:/sbin"
ROOTTOOLS_IDENTIFIER = "com.arthur.roottools"


class MigrationError(RuntimeError):
    pass


def device_udid(explicit: str | None) -> str:
    if explicit:
        return explicit
    if shutil.which("idevice_id"):
        lines = subprocess.check_output(["idevice_id", "-l"], text=True).splitlines()
        if lines:
            return lines[0]
    return discover_udid()


def root_read(udid: str, command: str, timeout: float = 20) -> str:
    rc, out, err = run_root(udid, command, timeout=timeout)
    if rc:
        raise MigrationError((err or out or f"device command failed: {rc}").strip())
    return out.strip()


def mobile_uid(udid: str) -> int:
    text = root_read(udid, "id -u mobile 2>/dev/null || echo 501")
    try:
        value = int(text.splitlines()[-1])
    except (ValueError, IndexError) as error:
        raise MigrationError(f"unable to determine mobile uid: {text!r}") from error
    if value < 1 or value > 99999:
        raise MigrationError(f"unexpected mobile uid: {value}")
    return value


def updater_running(udid: str) -> bool:
    text = root_read(udid, "pgrep -x roottools-updater >/dev/null 2>&1 && echo yes || echo no")
    return text.splitlines()[-1:] == ["yes"]


def wait_client(udid: str, token: str, timeout: float = 25) -> tuple[object, DeviceServiceClient]:
    context = device_proxy(udid)
    port = context.__enter__()
    client = DeviceServiceClient(port, token, "v09-migration")
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            client.status()
            return context, client
        except DeviceServiceError:
            time.sleep(0.5)
    context.__exit__(None, None, None)
    raise MigrationError("RootTools daemon did not become reachable")


def latest_update_state(client: DeviceServiceClient, request_id: str) -> dict | None:
    try:
        payload = client.self_update_status()
    except DeviceServiceError:
        return None
    return next((row for row in payload.get("updates", []) if row.get("requestId") == request_id), None)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def daemon_version_from_debian(version: str) -> str:
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+){1,3})(?:-[0-9]+)?", version)
    if not match:
        raise MigrationError(f"unsupported RootTools Debian version: {version!r}")
    return match.group(1)


def verify_local_staged_deb(
    client: DeviceServiceClient,
    package_id: str,
    explicit_path: Path | None,
) -> tuple[Path, str, str]:
    rows = client.packages().get("packages", [])
    package = next((item for item in rows if item.get("packageId") == package_id), None)
    if not package:
        raise MigrationError(f"staged package not found: {package_id}")
    if package.get("format") != "deb" or package.get("expectedIdentifier") != ROOTTOOLS_IDENTIFIER:
        raise MigrationError("migration requires a staged verified com.arthur.roottools DEB")
    if package.get("state") not in {"ready", "installed"}:
        raise MigrationError(f"staged package is not reusable: {package.get('state')}")
    name = str(package.get("name") or "")
    local = (explicit_path or (ROOT / "build/packages" / name)).resolve()
    if not local.is_file():
        raise MigrationError(f"matching local DEB is unavailable: {local}")
    staged_sha = str(package.get("sha256") or "").lower()
    local_sha = sha256_file(local)
    if not staged_sha or local_sha != staged_sha:
        raise MigrationError(f"local DEB SHA-256 does not match staged record: local={local_sha} staged={staged_sha}")
    if not shutil.which("dpkg-deb"):
        raise MigrationError("host dpkg-deb is required to verify the staged migration DEB")
    package_name = subprocess.check_output(["dpkg-deb", "-f", str(local), "Package"], text=True).strip()
    package_version = subprocess.check_output(["dpkg-deb", "-f", str(local), "Version"], text=True).strip()
    if package_name != ROOTTOOLS_IDENTIFIER:
        raise MigrationError(f"local DEB Package mismatch: {package_name!r}")
    return local, daemon_version_from_debian(package_version), local_sha


def extract_candidate_updater(package_path: Path, temporary: Path) -> Path:
    extract_root = temporary / "payload"
    subprocess.run(["dpkg-deb", "-x", str(package_path), str(extract_root)], check=True)
    updater = extract_root / "var/jb/usr/local/bin/roottools-updater"
    if not updater.is_file():
        raise MigrationError("verified RootTools DEB does not contain roottools-updater")
    return updater


def stop_daemon(udid: str, uid: int) -> None:
    root_read(
        udid,
        f"launchctl bootout user/{uid}/{SERVICE_LABEL} 2>/dev/null || "
        f"launchctl bootout user/foreground/{SERVICE_LABEL}",
        timeout=25,
    )


def bootstrap_daemon(udid: str) -> None:
    root_read(
        udid,
        "launchctl bootstrap user/foreground /var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist",
        timeout=25,
    )


def offline_update_row(
    udid: str,
    request_id: str,
    *,
    expected_states: set[str],
    expected_results: set[str | None],
    new_state: str,
    new_result: str | None,
    new_error: str | None,
) -> tuple[str, str | None, str | None]:
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,120}", request_id):
        raise MigrationError("unsafe self-update request identifier")
    encoded = root_read(udid, f"/var/jb/usr/bin/base64 < {UPDATE_DB}", timeout=25)
    try:
        database_bytes = base64.b64decode(encoded, validate=False)
    except Exception as error:
        raise MigrationError("could not decode self-update database snapshot") from error
    if len(database_bytes) < 4096:
        raise MigrationError("self-update database snapshot is unexpectedly small")

    timestamp = int(time.time())
    remote_new = f"{UPDATE_DB}.migration-new-{request_id}"
    remote_backup = f"{UPDATE_DB}.migration-backup-{request_id}-{timestamp}"
    with tempfile.TemporaryDirectory() as temporary:
        local_db = Path(temporary) / "self-update.sqlite3"
        local_db.write_bytes(database_bytes)
        with sqlite3.connect(local_db) as database:
            row = database.execute(
                "SELECT state,result,error FROM self_updates WHERE request_id=?",
                (request_id,),
            ).fetchone()
            if not row:
                raise MigrationError("approved self-update request disappeared from offline database")
            state, result, error = row
            if state not in expected_states or result not in expected_results:
                raise MigrationError(f"self-update row changed unexpectedly: state={state} result={result} error={error}")
            cursor = database.execute(
                "UPDATE self_updates SET state=?,result=?,error=?,updated_at=? WHERE request_id=? AND state=?",
                (new_state, new_result, new_error, timestamp, request_id, state),
            )
            if cursor.rowcount != 1:
                raise MigrationError("guarded offline self-update transition did not update exactly one row")
            database.commit()
        root_push(udid, local_db, remote_new)

    root_read(
        udid,
        f"test ! -e '{remote_backup}' && cp -p {UPDATE_DB} '{remote_backup}' && "
        f"chown 0:mobile '{remote_new}' && chmod 644 '{remote_new}' && mv '{remote_new}' {UPDATE_DB}",
    )
    return str(state), result, error


def recover_known_preswap_stale_update(udid: str, token: str, uid: int) -> None:
    context, client = wait_client(udid, token)
    try:
        if str(client.status().get("daemonVersion", "")) != "0.9.0":
            return
        active = [
            row for row in client.self_update_status().get("updates", [])
            if row.get("state") in {"queued", "launching", "running"}
        ]
        if not active:
            return
        if len(active) != 1:
            raise MigrationError(f"multiple active self-updates require manual inspection: {active}")
        row = active[0]
        request_id = str(row.get("requestId") or "")
        if row.get("state") != "running" or row.get("result") != "switching":
            raise MigrationError(f"active v0.9 update is not the known pre-swap state: {row}")
        updated_at = int(row.get("updatedAt") or 0)
        if updated_at <= 0 or time.time() - updated_at < 30 or updater_running(udid):
            raise MigrationError("active v0.9 update is not safely classifiable as stale")
        current_paths = [
            "/var/jb/Applications/RootTools.app",
            "/var/jb/usr/local/bin/roottools-execd",
            "/var/jb/usr/local/bin/roottools-updater",
            "/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist",
            "/var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist",
        ]
        candidates = " && ".join(f"test -e '{path}.update-{request_id}'" for path in current_paths)
        no_rollbacks = " && ".join(f"test ! -e '{path}.rollback-{request_id}'" for path in current_paths)
        root_read(udid, f"{candidates} && {no_rollbacks}")
    finally:
        context.__exit__(None, None, None)

    stop_daemon(udid, uid)
    try:
        offline_update_row(
            udid,
            request_id,
            expected_states={"running"},
            expected_results={"switching"},
            new_state="failed",
            new_result="recovered_pre_switch",
            new_error="v0.9 bootstrap migration recovered before any target swap",
        )
    finally:
        bootstrap_daemon(udid)
    verify_context, verify_client = wait_client(udid, token)
    try:
        recovered = latest_update_state(verify_client, request_id)
        if not recovered or recovered.get("state") != "failed" or recovered.get("result") != "recovered_pre_switch":
            raise MigrationError(f"stale self-update recovery verification failed: {recovered}")
    finally:
        verify_context.__exit__(None, None, None)
    print(f"migration: recovered stale pre-swap request={request_id}")


def obtain_approved_terminal_request(
    udid: str,
    token: str,
    uid: int,
    package_id: str,
    timeout: float,
) -> str:
    context, client = wait_client(udid, token)
    previous_path = root_read(udid, "launchctl getenv PATH 2>/dev/null || true")
    try:
        if str(client.status().get("daemonVersion", "")) != "0.9.0":
            raise MigrationError("legacy approval step requires the v0.9.0 bootstrap daemon")
        reusable = next(
            (
                row for row in client.self_update_status().get("updates", [])
                if row.get("packageId") == package_id
                and row.get("state") in {"failed", "rolled_back", "rollback_failed"}
            ),
            None,
        )
        if reusable:
            request_id = str(reusable.get("requestId") or "")
            print(f"migration: reusing terminal Owner-approved request={request_id}")
            return request_id

        # Force the legacy updater to fail before extraction/switching. This is
        # safer than trying to teach the old binary enough PATH/domain behavior
        # to perform a replacement it was never qualified to perform.
        root_read(udid, f"launchctl setenv PATH '{LEGACY_SAFE_PATH}'")
        root_read(udid, f"launchctl kickstart -k user/{uid}/{SERVICE_LABEL}", timeout=25)
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                if str(client.status().get("daemonVersion", "")) == "0.9.0":
                    break
            except DeviceServiceError:
                pass
            time.sleep(0.5)
        else:
            raise MigrationError("v0.9 daemon did not return with bounded legacy PATH")

        receipt = client.schedule_self_update(package_id, confirmed=True)
        if not receipt.get("ok"):
            raise MigrationError(f"typed self-update scheduling failed: {receipt}")
        request_id = str(receipt.get("output") or receipt.get("requestId") or "")
        if not request_id:
            raise MigrationError("typed self-update receipt did not expose a request id")
        print(f"migration: typed Owner-approved request={request_id}")

        deadline = time.time() + timeout
        last_marker = ""
        while time.time() < deadline:
            state = latest_update_state(client, request_id)
            if state:
                marker = f"{state.get('state')}:{state.get('result')}:{state.get('error')}"
                if marker != last_marker:
                    print(f"migration: legacy updater {marker}")
                    last_marker = marker
                if state.get("state") in {"failed", "rolled_back", "rollback_failed"}:
                    return request_id
            time.sleep(0.5)
        raise MigrationError("legacy updater did not reach a terminal pre-migration state")
    finally:
        try:
            if previous_path:
                root_read(udid, f"launchctl setenv PATH '{previous_path}'")
            else:
                root_read(udid, "launchctl unsetenv PATH 2>/dev/null || true")
        finally:
            context.__exit__(None, None, None)


def run_candidate_updater(
    udid: str,
    token: str,
    uid: int,
    request_id: str,
    package_id: str,
    target_version: str,
    candidate: Path,
    package_sha: str,
    timeout: float,
) -> None:
    remote = f"/var/jb/usr/local/bin/roottools-migration-updater-{package_sha[:12]}"
    root_push(udid, candidate, remote)
    root_read(udid, f"chmod 755 '{remote}'; chown 0:0 '{remote}'; /var/jb/usr/bin/ldid -S '{remote}'")

    stop_daemon(udid, uid)
    try:
        state, result, error = offline_update_row(
            udid,
            request_id,
            expected_states={"failed", "rolled_back", "rollback_failed"},
            expected_results={None, "recovered_pre_switch", "previous daemon restored"},
            new_state="launching",
            new_result=None,
            new_error=None,
        )
        print(f"migration: offline reset {state}:{result}:{error} -> launching")
        rc, out, err = run_root(udid, f"'{remote}' --request '{request_id}'", timeout=timeout)
        if rc != 0:
            detail = (err or out or f"candidate updater exited {rc}").strip()
            raise MigrationError(detail)
    finally:
        try:
            root_read(udid, f"rm -f '{remote}'")
        except Exception:
            pass

    context, client = wait_client(udid, token, timeout=30)
    try:
        version = str(client.status().get("daemonVersion", ""))
        if version != target_version:
            raise MigrationError(f"candidate updater returned but daemon is {version}, expected {target_version}")
        state = latest_update_state(client, request_id)
        if not state or state.get("state") != "succeeded" or state.get("packageId") != package_id:
            raise MigrationError(f"candidate updater did not record successful terminal state: {state}")
    finally:
        context.__exit__(None, None, None)

    # Prove the newly installed runtime no longer depends on any migration
    # process environment or helper executable.
    root_read(udid, f"launchctl kickstart -k user/{uid}/{SERVICE_LABEL}", timeout=25)
    verify_context, verify_client = wait_client(udid, token, timeout=30)
    try:
        version = str(verify_client.status().get("daemonVersion", ""))
        if version != target_version:
            raise MigrationError(f"clean restart reported {version}, expected {target_version}")
    finally:
        verify_context.__exit__(None, None, None)
    print(f"migration: clean restart verified v{target_version}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Migrate a physical RootTools v0.9 bootstrap through a verified candidate updater")
    parser.add_argument("package_id", help="Staged verified com.arthur.roottools package ID")
    parser.add_argument("--package-file", type=Path, help="Local DEB; defaults to build/packages/<staged name>")
    parser.add_argument("--admin-token", type=Path, default=DEFAULT_ADMIN_TOKEN)
    parser.add_argument("--udid", default=os.environ.get("ROOTTOOLS_UDID"))
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9._-]{1,80}", args.package_id):
        raise SystemExit("invalid staged package id")
    udid = device_udid(args.udid)
    token = load_token(args.admin_token)
    uid = mobile_uid(udid)

    context, client = wait_client(udid, token)
    try:
        current = str(client.status().get("daemonVersion", ""))
        package_path, target_version, package_sha = verify_local_staged_deb(client, args.package_id, args.package_file)
    finally:
        context.__exit__(None, None, None)
    print(f"migration: daemon before migration={current}")
    print(f"migration: verified staged DEB target={target_version} sha256={package_sha}")
    if current == target_version:
        print("migration: target already active")
        return 0
    if current != "0.9.0":
        raise MigrationError(f"expected physical bootstrap daemon 0.9.0, got {current}")

    recover_known_preswap_stale_update(udid, token, uid)
    request_id = obtain_approved_terminal_request(udid, token, uid, args.package_id, min(args.timeout, 90))
    if updater_running(udid):
        raise MigrationError("legacy updater is still running; candidate handoff refused")

    with tempfile.TemporaryDirectory() as temporary:
        candidate = extract_candidate_updater(package_path, Path(temporary))
        run_candidate_updater(
            udid,
            token,
            uid,
            request_id,
            args.package_id,
            target_version,
            candidate,
            package_sha,
            args.timeout,
        )
    print(f"migration: v0.9 bootstrap upgraded successfully to v{target_version}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (MigrationError, subprocess.CalledProcessError) as error:
        print(f"migration failed: {error}", file=sys.stderr)
        raise SystemExit(2)
