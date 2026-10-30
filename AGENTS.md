# Working with AirWindows

For a new installation, follow [FOR-AGENTS.md](FOR-AGENTS.md). It covers download, pairing, Home setup, frame capture and the final agent-operated test. Handle PC work and ask the user for one phone action at a time.

For an existing setup, read [the operating guide](app/CLAUDE-CONTROL.md). Reuse the intended receiver, observe the current screen, act once and verify the result. Follow your desktop tool's restrictions. Keep phone captures and device identifiers in a local, untracked folder.

The package includes `iosctl.exe`. Read [its guide](app/tools/iosctl/README.md) for commands, coordinates and exit codes. Use input commands only where local CLI-based desktop automation is permitted. Observation commands use the phone-only frame pipe and do not take focus. Python is optional.

For code changes, preserve concurrent work and distinguish software checks from physical phone results. The receiver source and build instructions live under `app`; the Bluetooth backend lives beside it in `native-ble-prototype`.
