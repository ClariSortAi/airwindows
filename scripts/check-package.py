"""Verify a binary/source release pair without launching the receiver."""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import re
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
directory = args.directory
binary, = directory.glob("AirWindows-*-win-x64.zip")
source, = directory.glob("AirWindows-*-source.zip")
seen = set()
for line in (directory / "SHA256SUMS.txt").read_text().splitlines():
    expected, name = line.split()
    if name not in {binary.name, source.name} or name in seen:
        raise SystemExit(f"Unexpected or duplicate checksum entry: {name}")
    seen.add(name)
    if hashlib.sha256((directory / name).read_bytes()).hexdigest() != expected:
        raise SystemExit(f"Checksum mismatch: {name}")
if seen != {binary.name, source.name}:
    raise SystemExit("Both release ZIPs must have a checksum.")
with zipfile.ZipFile(binary) as archive:
    names = archive.namelist()
    for suffix in ("/LICENSE.txt", "/licenses/iphone-use-MIT.txt", "/licenses/FFmpeg-LGPL-2.1.txt"):
        if not any(name.endswith(suffix) for name in names):
            raise SystemExit(f"Binary package is missing {suffix}")
    if not any(name.endswith(("/AirWindowsNative.exe", "/AirWindows.exe")) for name in names):
        raise SystemExit("Binary package has no receiver.")
    if "AirWindows/iosctl.exe" not in names:
        raise SystemExit("Windows package has no iosctl helper.")
    packaged_files = {
        "Start-AirWindows.cmd": "app/packaging/Start-AirWindows.cmd",
        "README.txt": "app/packaging/README.txt",
        **{name: name for name in (
            "AGENTS.md", "CLAUDE.md", "FOR-AGENTS.md", "requirements-agent.txt", "app/README.md",
            "app/CLAUDE-CONTROL.md", "app/NATIVE-CONTROL.md", "app/FRAME-INTERFACE.md",
            "app/scripts/observe-phone.py", "app/scripts/airwindows_frames.py",
            "app/tools/iosctl/README.md", "app/tools/iosctl/preflight.ps1",
            "app/tools/iosctl/targets.example.json", "app/tools/iosctl/DEMO-TAKE-A-PHOTO.md",
        )},
    }
    for relative, original in packaged_files.items():
        entry = "AirWindows/" + relative
        if entry not in names or archive.read(entry) != (root / original).read_bytes():
            raise SystemExit(f"Windows package missing or mismatched: {relative}")
    for name in names:
        if not name.endswith(".md"):
            continue
        text = archive.read(name).decode("utf-8-sig")
        for target in re.findall(r"\]\(([^)]+)\)", text):
            if "://" in target or target.startswith(("#", "mailto:")):
                continue
            parts = []
            for part in (PurePosixPath(name).parent / target.split("#", 1)[0]).parts:
                if part == "..":
                    parts.pop()
                elif part != ".":
                    parts.append(part)
            if "/".join(parts) not in names:
                raise SystemExit(f"Broken link in Windows package: {name} -> {target}")
with zipfile.ZipFile(source) as archive:
    names = archive.namelist()
    for name in (
        "AGENTS.md", "CLAUDE.md", "FOR-AGENTS.md", "requirements-agent.txt", "app/packaging/Start-AirWindows.cmd",
        "app/CMakeLists.txt", "app/src/CMakeLists.txt", "app/src/main.cpp",
        "app/src/frame_server.cpp", "app/src/control_queue.h",
        "app/tests/frame_server_check.cpp", "app/tests/control_policy_check.cpp", "app/tests/test_frames.py",
        "app/scripts/bootstrap.ps1", "app/scripts/package.ps1", "app/scripts/observe-phone.py",
        "app/scripts/airwindows_frames.py", "native-ble-prototype/native_hid_backend.cpp",
        "native-ble-prototype/native_hid_backend.h", "native-ble-prototype/absolute_buttons.h",
        "native-ble-prototype/discovery_window.h", "native-ble-prototype/battery_level.h",
        "native-ble-prototype/subscriber_selection.h",
        "native-ble-prototype/LICENSE-iphone-use",
    ):
        entries = [item for item in names if item.endswith("/" + name)]
        if len(entries) != 1 or archive.read(entries[0]) != (root / name).read_bytes():
            raise SystemExit(f"Source package missing or mismatched: {name}")
    for path in (root / "app/tools/iosctl").iterdir():
        if path.suffix not in {".cs", ".ps1", ".md", ".json", ".manifest"}:
            continue
        name = path.relative_to(root).as_posix()
        entries = [item for item in names if item.endswith("/" + name)]
        if len(entries) != 1 or archive.read(entries[0]) != path.read_bytes():
            raise SystemExit(f"Source package missing or mismatched helper file: {name}")
    for suffix in ("/app/third_party/UxPlay/lib/raop.c", "/app/third_party/UxPlay/AIRWINDOWS-UPSTREAM-COMMIT.txt"):
        if not any(name.endswith(suffix) for name in names):
            raise SystemExit(f"Source package missing {suffix}")
    if sum("/upstream-sources/" in name and name.endswith(".tar.gz") for name in names) != 3:
        raise SystemExit("Expected FFmpeg, OpenSSL and libplist source archives.")
    if any(any(part in {".git", ".local", "analysis", "__pycache__"} for part in Path(name).parts) for name in names):
        raise SystemExit("Private or generated files in source package.")
print("Package verified: checksums, executable, launcher, agent guides/reader, links, matching source and notices.")
