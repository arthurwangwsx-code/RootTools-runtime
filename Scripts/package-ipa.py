#!/usr/bin/env python3
from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import plistlib
import stat
import subprocess
import sys
import time
import zipfile


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_APP = ROOT / "build/DerivedData/Build/Products/Release-iphoneos/RootTools.app"
DEFAULT_DAEMON = ROOT / "build/daemon/roottools-execd"

DEB_SPEC = importlib.util.spec_from_file_location(
    "package_rootless_deb", ROOT / "Scripts/package-rootless-deb.py"
)
assert DEB_SPEC is not None and DEB_SPEC.loader is not None
DEB_PACKAGE = importlib.util.module_from_spec(DEB_SPEC)
DEB_SPEC.loader.exec_module(DEB_PACKAGE)

DEFAULT_VERSION = DEB_PACKAGE.DEFAULT_VERSION
DEFAULT_SOURCE_DATE_EPOCH = DEB_PACKAGE.DEFAULT_SOURCE_DATE_EPOCH
EXPECTED_BUNDLE_ID = "com.arthur.roottools.ios"


def zip_timestamp(source_date_epoch: int) -> tuple[int, int, int, int, int, int]:
    # ZIP cannot encode dates before 1980 or after 2107. Release timestamps are
    # expected to be ordinary Git commit times, but clamp explicitly so a
    # controlled SOURCE_DATE_EPOCH still produces a valid archive.
    minimum = 315_532_800
    maximum = 4_354_819_199
    return time.gmtime(min(max(source_date_epoch, minimum), maximum))[:6]


def write_ipa(app: Path, output: Path, source_date_epoch: int) -> None:
    if not app.is_dir():
        raise SystemExit(f"missing app bundle: {app}")

    entries = sorted(app.rglob("*"), key=lambda path: path.relative_to(app).as_posix())
    for source in entries:
        if source.is_symlink():
            raise SystemExit(f"IPA input must not contain symbolic links: {source}")

    output.parent.mkdir(parents=True, exist_ok=True)
    timestamp = zip_timestamp(source_date_epoch)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        root_info = zipfile.ZipInfo("Payload/RootTools.app/", timestamp)
        root_info.create_system = 3
        root_info.external_attr = (stat.S_IFDIR | 0o755) << 16
        archive.writestr(root_info, b"")

        for source in entries:
            relative = source.relative_to(app).as_posix()
            if source.is_dir():
                info = zipfile.ZipInfo(f"Payload/RootTools.app/{relative}/", timestamp)
                info.create_system = 3
                info.external_attr = (stat.S_IFDIR | 0o755) << 16
                archive.writestr(info, b"")
                continue
            if not source.is_file():
                raise SystemExit(f"unsupported IPA input: {source}")
            mode = source.stat().st_mode & 0o777
            info = zipfile.ZipInfo(f"Payload/RootTools.app/{relative}", timestamp)
            info.create_system = 3
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (stat.S_IFREG | mode) << 16
            archive.writestr(info, source.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    output.chmod(0o644)


def validate_inputs(app: Path, daemon: Path, version: str) -> None:
    DEB_PACKAGE.validate_version_inputs(app, daemon, version)

    info_path = app / "Info.plist"
    with info_path.open("rb") as stream:
        info = plistlib.load(stream)
    if info.get("CFBundleIdentifier") != EXPECTED_BUNDLE_ID:
        raise SystemExit(
            f"app bundle identifier mismatch: expected {EXPECTED_BUNDLE_ID}, "
            f"got {info.get('CFBundleIdentifier')}"
        )

    executable_name = info.get("CFBundleExecutable")
    if not isinstance(executable_name, str) or not executable_name:
        raise SystemExit("app Info.plist has no CFBundleExecutable")
    executable = app / executable_name
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise SystemExit(f"missing executable app binary: {executable}")

    if sys.platform == "darwin":
        subprocess.run(["codesign", "--verify", "--strict", str(executable)], check=True)
        architecture = subprocess.check_output(["lipo", "-archs", str(executable)], text=True).split()
        if architecture != ["arm64"]:
            raise SystemExit(f"app executable must contain only arm64, got: {' '.join(architecture)}")


def validate_archive(output: Path) -> None:
    with zipfile.ZipFile(output) as archive:
        names = archive.namelist()
        required = {
            "Payload/RootTools.app/Info.plist",
            "Payload/RootTools.app/RootTools",
        }
        missing = sorted(required.difference(names))
        if missing:
            raise SystemExit(f"IPA is missing required members: {', '.join(missing)}")
        for name in names:
            path = Path(name)
            if path.is_absolute() or ".." in path.parts or not name.startswith("Payload/RootTools.app/"):
                raise SystemExit(f"unsafe IPA member: {name}")
        bad = archive.testzip()
        if bad is not None:
            raise SystemExit(f"IPA CRC verification failed: {bad}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a deterministic TrollStore-compatible RootTools IPA")
    parser.add_argument("--app", type=Path, default=DEFAULT_APP)
    parser.add_argument("--daemon", type=Path, default=DEFAULT_DAEMON)
    parser.add_argument("--version", default=DEFAULT_VERSION)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / f"build/packages/RootTools_{DEFAULT_VERSION}.ipa",
    )
    parser.add_argument("--source-date-epoch", type=int, default=DEFAULT_SOURCE_DATE_EPOCH)
    args = parser.parse_args()
    if args.source_date_epoch < 0:
        parser.error("--source-date-epoch must be non-negative")

    validate_inputs(args.app, args.daemon, args.version)
    write_ipa(args.app, args.output, args.source_date_epoch)
    validate_archive(args.output)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
