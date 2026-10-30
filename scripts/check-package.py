"""Verify a binary/source release pair without launching the receiver."""
import argparse
import hashlib
from pathlib import Path
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
directory = args.directory
binary, = directory.glob("AirWindows-*-win-x64.zip")
source, = directory.glob("AirWindows-*-source.zip")
for line in (directory / "SHA256SUMS.txt").read_text().splitlines():
    expected, name = line.split()
    if name not in {binary.name, source.name}:
        raise SystemExit(f"Unexpected checksum entry: {name}")
    if hashlib.sha256((directory / name).read_bytes()).hexdigest() != expected:
        raise SystemExit(f"Checksum mismatch: {name}")
with zipfile.ZipFile(binary) as archive:
    names = archive.namelist()
    for suffix in ("/LICENSE.txt", "/licenses/iphone-use-MIT.txt", "/licenses/FFmpeg-LGPL-2.1.txt"):
        if not any(name.endswith(suffix) for name in names):
            raise SystemExit(f"Binary package is missing {suffix}")
    if not any(name.endswith(("/AirWindowsNative.exe", "/AirWindows.exe")) for name in names):
        raise SystemExit("Binary package has no receiver.")
with zipfile.ZipFile(source) as archive:
    names = archive.namelist()
    for name in (
        "app/CMakeLists.txt", "app/src/CMakeLists.txt", "app/src/main.cpp",
        "app/scripts/bootstrap.ps1", "app/scripts/package.ps1", "app/scripts/observe-phone.py",
        "app/scripts/airwindows_frames.py", "native-ble-prototype/native_hid_backend.cpp",
        "native-ble-prototype/native_hid_backend.h", "native-ble-prototype/absolute_buttons.h",
        "native-ble-prototype/discovery_window.h", "native-ble-prototype/battery_level.h",
        "native-ble-prototype/LICENSE-iphone-use",
    ):
        entries = [item for item in names if item.endswith("/" + name)]
        if len(entries) != 1 or archive.read(entries[0]) != (root / name).read_bytes():
            raise SystemExit(f"Source package missing or mismatched: {name}")
    for suffix in ("/app/third_party/UxPlay/lib/raop.c", "/app/third_party/UxPlay/AIRWINDOWS-UPSTREAM-COMMIT.txt"):
        if not any(name.endswith(suffix) for name in names):
            raise SystemExit(f"Source package missing {suffix}")
    if sum("/upstream-sources/" in name and name.endswith(".tar.gz") for name in names) != 3:
        raise SystemExit("Expected FFmpeg, OpenSSL and libplist source archives.")
    if any(any(part in {".git", ".local", "analysis", "__pycache__"} for part in Path(name).parts) for name in names):
        raise SystemExit("Private or generated files in source package.")
print("Package verified: checksums, executable, notices, matching backend source and upstream sources.")
