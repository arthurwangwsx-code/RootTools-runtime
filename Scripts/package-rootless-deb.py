#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import os
from pathlib import Path
import stat
import tarfile
import time


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_APP = ROOT / "build/DerivedData/Build/Products/Release-iphoneos/RootTools.app"
DEFAULT_DAEMON = ROOT / "build/daemon/roottools-execd"
DEFAULT_UPDATER = ROOT / "build/daemon/roottools-updater"
DEFAULT_PLIST = ROOT / "Daemon/com.arthur.roottools.execd.plist"
DEFAULT_UPDATER_PLIST = ROOT / "Daemon/com.arthur.roottools.updater.plist"


def tar_bytes(files: list[tuple[Path, str, int]], extra_text: dict[str, tuple[str, int]] | None = None) -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz", format=tarfile.GNU_FORMAT) as archive:
        for source, destination, mode in files:
            info = archive.gettarinfo(str(source), arcname=destination)
            info.uid = 0
            info.gid = 0
            info.uname = "root"
            info.gname = "wheel"
            info.mode = mode
            with source.open("rb") as handle:
                archive.addfile(info, handle)
        if extra_text:
            for destination, (text, mode) in extra_text.items():
                payload = text.encode()
                info = tarfile.TarInfo(destination)
                info.size = len(payload)
                info.mode = mode
                info.uid = 0
                info.gid = 0
                info.uname = "root"
                info.gname = "wheel"
                info.mtime = int(time.time())
                archive.addfile(info, io.BytesIO(payload))
    return buffer.getvalue()


def ar_member(name: str, payload: bytes) -> bytes:
    if len(name) > 15:
        raise ValueError(f"ar member name too long: {name}")
    header = (
        f"{name + '/':<16}"
        f"{int(time.time()):<12}"
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


def build_package(app: Path, daemon: Path, updater: Path, plist: Path, updater_plist: Path, output: Path, version: str) -> None:
    for path in (app, daemon, updater, plist, updater_plist):
        if not path.exists():
            raise SystemExit(f"missing build input: {path}")

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
LDID=/var/jb/usr/bin/ldid

[ -x "$LDID" ] || { echo "RootTools: ldid unavailable" >&2; exit 1; }
chmod 755 "$APP/RootTools" "$DAEMON" "$UPDATER"
chown 0:0 "$DAEMON" "$UPDATER" "$PLIST" "$UPDATER_PLIST"
"$LDID" -S "$APP/RootTools"
"$LDID" -S "$DAEMON"
"$LDID" -S "$UPDATER"
launchctl bootout system/com.arthur.roottools.execd >/dev/null 2>&1 || true
launchctl bootout system/com.arthur.roottools.updater >/dev/null 2>&1 || true
launchctl bootstrap system "$PLIST"
launchctl bootstrap system "$UPDATER_PLIST"
/var/jb/usr/bin/uicache -p "$APP"
exit 0
"""

    prerm = """#!/var/jb/bin/sh
launchctl bootout system/com.arthur.roottools.execd >/dev/null 2>&1 || true
launchctl bootout system/com.arthur.roottools.updater >/dev/null 2>&1 || true
exit 0
"""

    control_tar = tar_bytes(
        [],
        {
            "./control": (control, 0o644),
            "./postinst": (postinst, 0o755),
            "./prerm": (prerm, 0o755),
        },
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
    data_tar = tar_bytes(data_files)

    output.parent.mkdir(parents=True, exist_ok=True)
    package = b"!<arch>\n"
    package += ar_member("debian-binary", b"2.0\n")
    package += ar_member("control.tar.gz", control_tar)
    package += ar_member("data.tar.gz", data_tar)
    output.write_bytes(package)
    output.chmod(0o644)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a rootless RootTools .deb")
    parser.add_argument("--app", type=Path, default=DEFAULT_APP)
    parser.add_argument("--daemon", type=Path, default=DEFAULT_DAEMON)
    parser.add_argument("--updater", type=Path, default=DEFAULT_UPDATER)
    parser.add_argument("--plist", type=Path, default=DEFAULT_PLIST)
    parser.add_argument("--updater-plist", type=Path, default=DEFAULT_UPDATER_PLIST)
    parser.add_argument("--version", default="0.8.0-1")
    parser.add_argument("--output", type=Path, default=ROOT / "build/packages/roottools_0.8.0-1_iphoneos-arm64.deb")
    args = parser.parse_args()
    build_package(args.app, args.daemon, args.updater, args.plist, args.updater_plist, args.output, args.version)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
