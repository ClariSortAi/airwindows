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
- For receiver or backend changes: `./app/scripts/build.ps1 -Target AirWindowsNative`, `-Target FrameServerCheck` and `-Target MdnsRefreshCheck`, then run `./app/build/Release/bin/FrameServerCheck.exe` and `./app/build/Release/bin/MdnsRefreshCheck.exe`
- For changes to Microsoft Store licensing (`app/src/license_state.*`, `app/src/store_license.*`, the unlock panel, Help > Buy AirWindows): `./app/scripts/build.ps1 -Target LicenseCheck`, then run `./app/build/Release/bin/LicenseCheck.exe`. Check each UI state with `--selftest-license trial:<1-365>`, `expired` or `licensed`: it simulates the Store's answer for that run only, makes no Store call and saves nothing. Without it, only a packaged (MSIX) run asks the Store; source builds and the ZIP never do.
- For changes to `app/src/recorder.cpp`: `./app/scripts/build.ps1 -Target RecorderCheck`, then run `./app/build/Release/bin/RecorderCheck.exe` (local only; CI does not run it). It prints `SKIP` and exits 0 on a PC without an H.264 encoder.
- For window, capture or menu changes: run the receiver on a clip with `--selftest-video <clip.h264> --selftest-screenshot --selftest-record 4 --capture-dir <new folder> --exit-after 10` and check the `[CAPTURE]` and `[RECORD]` log lines and the two files. Make a clip with `ffmpeg -f lavfi -i testsrc2=size=1170x2532:rate=30 -t 2 -c:v libx264 -pix_fmt yuv420p -bsf:v h264_mp4toannexb -f h264 clip.h264`.
- When the icon drawing changes: edit `app/src/app-icon.svg` and `app/src/app-icon-small.svg`, run `python app/scripts/make-icon.py` (needs Inkscape 1.x), then touch `app/src/app.rc` so the next build embeds the new `app.ico`.
- When the icon drawing changes, also run `python app/scripts/make-store-assets.py` (needs Inkscape 1.x and Pillow) and commit `app/packaging/msix/Assets/`.
- Store package: `./app/scripts/package-msix.ps1 -OutDir <new dir> -IdentityName <Partner Center Name> -Publisher "<CN=...>" -PublisherDisplayName "<name>"`. It never overwrites output. The first version part must not be 0; `-AllowZeroMajor` is for local tests only. Runbook: `app/packaging/msix/README.md`.
- For manifest or packaging changes: run `./app/packaging/msix/probe/run-probe.ps1` (needs Developer Mode and Build Tools; add `-Bluetooth` only when no receiver is running). Then do a `-Register` dry run and `./app/scripts/package-msix.ps1 -Unregister`.
- For changes to the data folder, the setup guide, the network or Bluetooth checks: check them packaged too. Build `-Config RelWithDebInfo`, run `./app/scripts/package-msix.ps1 -ExePath app\build\RelWithDebInfo\bin\AirWindowsNative.exe -IosctlPath app\build\RelWithDebInfo\bin\iosctl.exe -AllowZeroMajor -OutDir app\dist\<new dir> -Register`, drive it with `driver.ps1 launch ... -Packaged` (see `.claude/skills/run-airwindows/SKILL.md`), then `./app/scripts/package-msix.ps1 -Unregister`.
- Self-test flags for these states (each makes the run a self-test; none is saved):
  - `--selftest-bluetooth none|no-peripheral|ready` simulates the launch check of the Bluetooth adapter; `probe` runs the real, read-only check. Without the flag a self-test skips the check.
  - `--selftest-network public|private|mixed|public-allowed|unknown` simulates the network types and the firewall answer: `public` every network Public and blocked (hard warning, step 1 unfinished), `private` every network Private and allowed, `mixed` one Private and allowed and the rest Public and blocked (conditional note), `public-allowed` Public but an allow rule covers it (no notice), `unknown` Public with the firewall unreadable (conditional note).
  - `--selftest-license store` asks the real Store, read-only, in a packaged self-test. Do not post Buy in that mode: it opens the real purchase dialog.
  - With `--selftest-license <trial:N>`, posting command 41201 (`driver.ps1 cmd expire`) ends the simulated trial while the receiver runs; `cmd buy` then unlocks it.
- Do not start a non-self-test receiver while a connected network is Public: it opens the network listener, and Windows shows a firewall prompt (unpackaged always, packaged on Public networks).

Rules:

- This file, `CLAUDE.md`, `FOR-AGENTS.md` and the `app/*.md` guides ship byte-for-byte in the Windows package. Keep links relative and valid in that layout; after packaging, `python scripts/check-package.py <outdir>` verifies them.
- `check-repository.py` rejects user-profile paths, local repository paths and device identifiers in docs and source. Use `$env:USERPROFILE` or placeholders.
- The version lives in `app/CMakeLists.txt` (`project(... VERSION)` and `AIRWINDOWS_VERSION_SUFFIX`). A release bump also needs the hardcoded URLs and paths in `FOR-AGENTS.md`, `README.md`, `app/README.md`, `app/tools/iosctl/README.md` and `app/CHANGELOG.md`; find them with `git grep <old-version>`.
- Package into a new directory: `./app/scripts/package.ps1 -Release -OutDir <new dir>`. It never overwrites existing output. It also writes `AirWindows-<version>-symbols.zip`; keep that file with the release to read crash reports. It is not part of the download or `SHA256SUMS.txt`.
- Close a running receiver before a manual receiver test. A second launch hands over to the running receiver and exits. Runs with `--selftest-video`, `--selftest-audio`, `--selftest-tone`, `--selftest-crash`, `--selftest-license`, `--selftest-bluetooth` or `--selftest-network` are exempt and do not advertise.
- Data folder (`settings.ini`, `debug.txt`, `crashes\`): unpackaged `%LOCALAPPDATA%\AirWindows`; a Store (MSIX) install uses `%LOCALAPPDATA%\Packages\<PackageFamilyName>\LocalState`, and its first run copies `settings.ini` from `%LOCALAPPDATA%\AirWindows`. `iosctl --doctor` prints `data_folder` for the running receiver; `python app/scripts/observe-phone.py --pid <pid> --data-folder` does the same.
- When a test receiver must not show the crash dialog, pass `--exit-after <seconds>`.
- Preserve concurrent work. Report software checks and physical phone results separately. Receiver source is under `app`; the Bluetooth backend lives beside it in `native-ble-prototype`.
