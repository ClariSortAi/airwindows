# Working with AirWindows

For a new installation, follow [FOR-AGENTS.md](FOR-AGENTS.md). It covers download, pairing, Home setup, frame capture and the final agent-operated test. Handle PC work and ask the user for one phone action at a time.

For an existing setup, read [the operating guide](app/CLAUDE-CONTROL.md). Reuse the intended receiver, observe the current screen, act once and verify the result. Follow your desktop tool's restrictions. Keep phone captures and device identifiers in a local, untracked folder.

The package includes `iosctl.exe`. Read [its guide](app/tools/iosctl/README.md) for commands, coordinates and exit codes. Use input commands only where local CLI-based desktop automation is permitted. Observation commands use the phone-only frame pipe and do not take focus. Python is optional.

Start troubleshooting with `iosctl --doctor`; it sends no input. `snapshot` is a standalone retained-image inspection command. Its output cannot verify an action or replace the fresh observation required for input. Check affected guides, both frame clients, tests and release contents when changing these contracts.

## Changing this repository

In a source checkout, run these before pushing (CI runs the same on Windows):

- `./scripts/check.ps1` (C++ policy checks; needs Visual Studio Build Tools)
- `python -m pytest app/tests/test_frames.py`
- `python scripts/check-repository.py`
- `./app/tools/iosctl/build.ps1`, then `./app/build/Release/bin/iosctl.exe --self-test`
- For receiver or backend changes: `./app/scripts/build.ps1 -Target AirWindowsNative` and `-Target FrameServerCheck`, then run `./app/build/Release/bin/FrameServerCheck.exe`

Rules:

- This file, `CLAUDE.md`, `FOR-AGENTS.md` and the `app/*.md` guides ship byte-for-byte in the Windows package. Keep links relative and valid in that layout; after packaging, `python scripts/check-package.py <outdir>` verifies them.
- `check-repository.py` rejects user-profile paths, local repository paths and device identifiers in docs and source. Use `$env:USERPROFILE` or placeholders.
- The version lives in `app/CMakeLists.txt` (`project(... VERSION)` and `AIRWINDOWS_VERSION_SUFFIX`). A release bump also needs the hardcoded URLs and paths in `FOR-AGENTS.md`, `README.md`, `app/README.md`, `app/tools/iosctl/README.md` and `app/CHANGELOG.md`; find them with `git grep <old-version>`.
- Package into a new directory: `./app/scripts/package.ps1 -Release -OutDir <new dir>`. It never overwrites existing output.
- Preserve concurrent work. Report software checks and physical phone results separately. Receiver source is under `app`; the Bluetooth backend lives beside it in `native-ble-prototype`.
