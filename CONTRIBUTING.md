# Contributing

Build and run the checks in [the application guide](app/README.md). Keep changes to the AirPlay receiver and Bluetooth control separate from work in `airdrop-native` unless both are necessary.

For a bug, include the executable version, Windows version, Bluetooth adapter, iPhone/iOS version, steps to reproduce and the result you expected. A short excerpt from `%LOCALAPPDATA%\AirWindows\debug.txt` usually helps. Remove phone names, network addresses and other personal details before attaching it. Do not attach phone screenshots unless their contents are safe to share.

For input changes, test a click, text entry, a list drag, focus loss and reconnect. Report which checks used a real phone and which ran in software. A successful HID notification is not evidence that the intended control changed. Do not replay an uncertain action automatically.

Run `./scripts/check.ps1`, `python -m pytest app/tests/test_frames.py`, and `python scripts/check-repository.py` before submitting a pull request. Build `AirWindowsNative` and `FrameServerCheck` for changes to the application or backend. CI repeats these checks on Windows.

Keep captures, pairing settings, generated binaries, caches and credentials out of Git. Include the relevant upstream notice when adapting code. Run `app/scripts/package.ps1 -Release` into a new output directory when preparing a binary release, so the matching source and notices travel with it.
