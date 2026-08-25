#!/usr/bin/env python3
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "package_rootless_deb", ROOT / "Scripts/package-rootless-deb.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        source = Path(temporary) / "CodeResources"
        source.write_bytes(b"signed fixture")
        payload = MODULE.tar_bytes(
            [
                (
                    source,
                    "./var/jb/Applications/RootTools.app/_CodeSignature/CodeResources",
                    0o644,
                )
            ]
        )
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
            members = {member.name: member for member in archive.getmembers()}
        required_directories = {
            "./var",
            "./var/jb",
            "./var/jb/Applications",
            "./var/jb/Applications/RootTools.app",
            "./var/jb/Applications/RootTools.app/_CodeSignature",
        }
        for directory in required_directories:
            assert directory in members, directory
            assert members[directory].isdir(), directory
        assert "./var/jb/Applications/RootTools.app/_CodeSignature/CodeResources" in members

    print("package_builder_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
