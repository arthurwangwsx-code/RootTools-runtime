#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import plistlib
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("package_ipa", ROOT / "Scripts/package-ipa.py")
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        temporary_path = Path(temporary)
        app = temporary_path / "RootTools.app"
        app.mkdir()
        with (app / "Info.plist").open("wb") as stream:
            plistlib.dump(
                {
                    "CFBundleExecutable": "RootTools",
                    "CFBundleIdentifier": "com.arthur.roottools.ios",
                },
                stream,
            )
        executable = app / "RootTools"
        executable.write_bytes(b"arm64 fixture")
        executable.chmod(0o755)
        resources = app / "Resources"
        resources.mkdir()
        (resources / "fixture.txt").write_text("deterministic fixture\n")

        first = temporary_path / "first.ipa"
        second = temporary_path / "second.ipa"
        MODULE.write_ipa(app, first, 1_700_000_000)
        MODULE.write_ipa(app, second, 1_700_000_000)
        assert first.read_bytes() == second.read_bytes()

        MODULE.validate_archive(first)
        with zipfile.ZipFile(first) as archive:
            names = archive.namelist()
            assert names == sorted(names, key=lambda name: name.removeprefix("Payload/RootTools.app/").rstrip("/"))
            assert archive.read("Payload/RootTools.app/Resources/fixture.txt") == b"deterministic fixture\n"
            executable_info = archive.getinfo("Payload/RootTools.app/RootTools")
            assert (executable_info.external_attr >> 16) & 0o111

    print("package_ipa_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
