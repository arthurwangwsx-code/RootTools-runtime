#!/usr/bin/env python3
from __future__ import annotations

import argparse
import gzip
import io
import os
from pathlib import Path, PurePosixPath
import plistlib
import stat
import subprocess
import tarfile


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_APP = ROOT / "build/DerivedData/Build/Products/Release-iphoneos/RootTools.app"
DEFAULT_DAEMON = ROOT / "build/daemon/roottools-execd"
DEFAULT_UPDATER = ROOT / "build/daemon/roottools-updater"
DEFAULT_PLIST = ROOT / "Daemon/com.arthur.roottools.execd.plist"
DEFAULT_UPDATER_PLIST = ROOT / "Daemon/com.arthur.roottools.updater.plist"
VERSION_FILE = ROOT / "VERSION"


def load_version() -> str:
    version = VERSION_FILE.read_text().strip()
    parts = version.rsplit("-", 1)
    if len(parts) != 2 or not parts[1].isdigit():
        raise SystemExit(f"invalid RootTools package version in {VERSION_FILE}: {version}")
    core = parts[0].split(".")
    if len(core) != 3 or not all(part.isdigit() for part in core):
        raise SystemExit(f"invalid RootTools package version in {VERSION_FILE}: {version}")
    return version


DEFAULT_VERSION = load_version()


def load_source_date_epoch() -> int:
    configured = os.environ.get("SOURCE_DATE_EPOCH")
    if configured is not None:
        if not configured.isdigit():
            raise SystemExit("SOURCE_DATE_EPOCH must be a non-negative integer")
        return int(configured)
    try:
        value = subprocess.check_output(
            ["git", "-C", str(ROOT), "show", "-s", "--format=%ct", "HEAD"],
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit("unable to derive SOURCE_DATE_EPOCH from Git HEAD") from error
    if not value.isdigit():
        raise SystemExit(f"invalid Git commit timestamp: {value}")
    return int(value)


DEFAULT_SOURCE_DATE_EPOCH = load_source_date_epoch()


def daemon_version_from_package(package_version: str) -> str:
    core, separator, revision = package_version.rpartition("-")
    if not separator or not revision.isdigit():
        raise SystemExit(f"invalid RootTools package version: {package_version}")
    components = core.split(".")
    if len(components) != 3 or not all(component.isdigit() for component in components):
        raise SystemExit(f"invalid RootTools package version: {package_version}")
    return core


def validate_version_inputs(app: Path, daemon: Path, version: str) -> None:
    if version != DEFAULT_VERSION:
        raise SystemExit(f"package version must match {VERSION_FILE}: expected {DEFAULT_VERSION}, got {version}")

    daemon_version = daemon_version_from_package(version)
    package_stamp = ROOT / "build/generated/package-version"
    daemon_stamp = ROOT / "build/generated/daemon-version"
    for stamp, expected in ((package_stamp, version), (daemon_stamp, daemon_version)):
        if not stamp.is_file() or stamp.read_text().strip() != expected:
            raise SystemExit(f"stale or missing build version stamp: {stamp} (expected {expected})")

    profile_stamp = ROOT / "build/generated/credential-profile"
    fingerprint_stamps = (
        ROOT / "build/generated/owner-token-fingerprint",
        ROOT / "build/generated/agent-token-fingerprint",
    )
    if not profile_stamp.is_file() or not profile_stamp.read_text().strip():
        raise SystemExit(f"stale or missing credential profile stamp: {profile_stamp}")
    for stamp in fingerprint_stamps:
        value = stamp.read_text().strip() if stamp.is_file() else ""
        if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
            raise SystemExit(f"stale or malformed credential fingerprint stamp: {stamp}")

    generated_daemon = ROOT / "build/generated/roottools_execd.c"
    expected_define = f'#define VERSION "{daemon_version}"'
    expected_package_define = f'#define PACKAGE_VERSION "{version}"'
    generated_text = generated_daemon.read_text() if generated_daemon.is_file() else ""
    if expected_define not in generated_text:
        raise SystemExit(f"generated daemon version mismatch: expected {expected_define}")
    if expected_package_define not in generated_text:
        raise SystemExit(f"generated daemon package version mismatch: expected {expected_package_define}")
    if daemon.stat().st_mtime < generated_daemon.stat().st_mtime:
        raise SystemExit(f"daemon binary is older than generated version source: {daemon}")

    info_plist = app / "Info.plist"
    if not info_plist.is_file():
        raise SystemExit(f"missing build input: {info_plist}")
    with info_plist.open("rb") as stream:
        info = plistlib.load(stream)
    if info.get("CFBundleShortVersionString") != daemon_version:
        raise SystemExit(
            f"app version mismatch: expected {daemon_version}, got {info.get('CFBundleShortVersionString')}"
        )
    expected_build = version.rsplit("-", 1)[1]
    if str(info.get("CFBundleVersion")) != expected_build:
        raise SystemExit(f"app build mismatch: expected {expected_build}, got {info.get('CFBundleVersion')}")


def tar_bytes(
    files: list[tuple[Path, str, int]],
    extra_text: dict[str, tuple[str, int]] | None = None,
    source_date_epoch: int = DEFAULT_SOURCE_DATE_EPOCH,
) -> bytes:
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode="wb", mtime=source_date_epoch) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.GNU_FORMAT) as archive:
            added_directories: set[str] = set()

            def add_parent_directories(destination: str) -> None:
                normalized = destination.removeprefix("./")
                parent_chain = list(PurePosixPath(normalized).parents)
                for parent in reversed(parent_chain):
                    name = parent.as_posix()
                    if name in {".", "/"} or name in added_directories:
                        continue
                    info = tarfile.TarInfo(f"./{name}")
                    info.type = tarfile.DIRTYPE
                    info.mode = 0o755
                    info.uid = 0
                    info.gid = 0
                    info.uname = "root"
                    info.gname = "wheel"
                    info.mtime = source_date_epoch
                    archive.addfile(info)
                    added_directories.add(name)

            for source, destination, mode in files:
                add_parent_directories(destination)
                info = archive.gettarinfo(str(source), arcname=destination)
                info.uid = 0
                info.gid = 0
                info.uname = "root"
                info.gname = "wheel"
                info.mode = mode
                info.mtime = source_date_epoch
                with source.open("rb") as handle:
                    archive.addfile(info, handle)
            if extra_text:
                for destination, (text, mode) in extra_text.items():
                    add_parent_directories(destination)
                    payload = text.encode()
                    info = tarfile.TarInfo(destination)
                    info.size = len(payload)
                    info.mode = mode
                    info.uid = 0
                    info.gid = 0
                    info.uname = "root"
                    info.gname = "wheel"
                    info.mtime = source_date_epoch
                    archive.addfile(info, io.BytesIO(payload))
    return buffer.getvalue()


def ar_member(name: str, payload: bytes, source_date_epoch: int = DEFAULT_SOURCE_DATE_EPOCH) -> bytes:
    if len(name) > 15:
        raise ValueError(f"ar member name too long: {name}")
    header = (
        f"{name + '/':<16}"
        f"{source_date_epoch:<12}"
        f"{0:<6}"
        f"{0:<6}"
        f"{0o100644:<8o}"
        f"{len(payload):<10}"
        "`\n"
    ).encode("ascii")
    if len(header) != 60:
        raise AssertionError(len(header))
    return header + payload + (b"\n" if len(payload) % 2 else b"")


def app_files(app: Path) -> list[tuple[Path, str, int]]:
    result: list[tuple[Path, str, int]] = []
    for source in sorted(app.rglob("*")):
        if not source.is_file():
            continue
        relative = source.relative_to(app)
        destination = f"./var/jb/Applications/RootTools.app/{relative.as_posix()}"
        executable = source.name == "RootTools" or bool(source.stat().st_mode & stat.S_IXUSR)
        result.append((source, destination, 0o755 if executable else 0o644))
    return result


def build_package(
    app: Path,
    daemon: Path,
    updater: Path,
    plist: Path,
    updater_plist: Path,
    output: Path,
    version: str,
    source_date_epoch: int = DEFAULT_SOURCE_DATE_EPOCH,
) -> None:
    for path in (app, daemon, updater, plist, updater_plist):
        if not path.exists():
            raise SystemExit(f"missing build input: {path}")
    validate_version_inputs(app, daemon, version)

    control = f"""Package: com.arthur.roottools
Name: RootTools
Version: {version}
Architecture: iphoneos-arm64
Section: Utilities
Priority: optional
Maintainer: RootTools
Depends: firmware (>= 16.0)
Description: Policy-controlled privileged iOS device control plane
"""

    postinst = """#!/var/jb/bin/sh
set -e
APP=/var/jb/Applications/RootTools.app
DAEMON=/var/jb/usr/local/bin/roottools-execd
UPDATER=/var/jb/usr/local/bin/roottools-updater
PLIST=/var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist
UPDATER_PLIST=/var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist
LDID=""
for candidate in /var/jb/usr/bin/ldid /var/jb/bin/ldid /usr/bin/ldid; do
    if [ -x "$candidate" ]; then LDID="$candidate"; break; fi
done
chmod 755 "$APP/RootTools" "$DAEMON" "$UPDATER"
chown 0:0 "$DAEMON" "$UPDATER" "$PLIST" "$UPDATER_PLIST"
if [ -n "$LDID" ]; then
    if "$LDID" -S "$APP/RootTools" && \
       "$LDID" -S "$DAEMON" && \
       "$LDID" -S "$UPDATER"; then
        echo "RootTools: refreshed build-time signatures with device ldid"
    else
        echo "RootTools: device ldid is present but unusable; keeping build-time ad-hoc signatures"
    fi
else
    echo "RootTools: device ldid unavailable; using build-time ad-hoc signatures"
fi
LAUNCH_DOMAIN="user/foreground"
launchctl bootout "$LAUNCH_DOMAIN/com.arthur.roottools.execd" >/dev/null 2>&1 || true
launchctl bootout "$LAUNCH_DOMAIN/com.arthur.roottools.updater" >/dev/null 2>&1 || true
launchctl bootstrap "$LAUNCH_DOMAIN" "$PLIST"
launchctl bootstrap "$LAUNCH_DOMAIN" "$UPDATER_PLIST"
/var/jb/usr/bin/uicache -p "$APP"
REGISTERED=0
for attempt in 1 2 3 4 5; do
    if /var/jb/usr/bin/uicache -i com.arthur.roottools.ios >/dev/null 2>&1; then REGISTERED=1; break; fi
    sleep 1
done
if [ "$REGISTERED" -ne 1 ]; then
    echo "RootTools: foreground App registration verification failed" >&2
    exit 1
fi
exit 0
"""

    prerm = """#!/var/jb/bin/sh
LAUNCH_DOMAIN="user/foreground"
launchctl bootout "$LAUNCH_DOMAIN/com.arthur.roottools.execd" >/dev/null 2>&1 || true
launchctl bootout "$LAUNCH_DOMAIN/com.arthur.roottools.updater" >/dev/null 2>&1 || true
exit 0
"""

    control_tar = tar_bytes(
        [],
        {
            "./control": (control, 0o644),
            "./postinst": (postinst, 0o755),
            "./prerm": (prerm, 0o755),
        },
        source_date_epoch,
    )

    data_files = app_files(app)
    data_files.extend(
        [
            (daemon, "./var/jb/usr/local/bin/roottools-execd", 0o755),
            (updater, "./var/jb/usr/local/bin/roottools-updater", 0o755),
            (plist, "./var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist", 0o644),
            (updater_plist, "./var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist", 0o644),
        ]
    )
    data_tar = tar_bytes(data_files, source_date_epoch=source_date_epoch)

    output.parent.mkdir(parents=True, exist_ok=True)
    package = b"!<arch>\n"
    package += ar_member("debian-binary", b"2.0\n", source_date_epoch)
    package += ar_member("control.tar.gz", control_tar, source_date_epoch)
    package += ar_member("data.tar.gz", data_tar, source_date_epoch)
    output.write_bytes(package)
    output.chmod(0o644)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a rootless RootTools .deb")
    parser.add_argument("--app", type=Path, default=DEFAULT_APP)
    parser.add_argument("--daemon", type=Path, default=DEFAULT_DAEMON)
    parser.add_argument("--updater", type=Path, default=DEFAULT_UPDATER)
    parser.add_argument("--plist", type=Path, default=DEFAULT_PLIST)
    parser.add_argument("--updater-plist", type=Path, default=DEFAULT_UPDATER_PLIST)
    parser.add_argument("--version", default=DEFAULT_VERSION)
    parser.add_argument("--output", type=Path, default=ROOT / f"build/packages/roottools_{DEFAULT_VERSION}_iphoneos-arm64.deb")
    parser.add_argument("--source-date-epoch", type=int, default=DEFAULT_SOURCE_DATE_EPOCH)
    args = parser.parse_args()
    if args.source_date_epoch < 0:
        parser.error("--source-date-epoch must be non-negative")
    build_package(
        args.app,
        args.daemon,
        args.updater,
        args.plist,
        args.updater_plist,
        args.output,
        args.version,
        args.source_date_epoch,
    )
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
