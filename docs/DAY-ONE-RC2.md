# Three first-day fixes in 0.3.1-rc.2

| Problem | Change | Proof |
|---|---|---|
| A fine-resolution trackpad launches a full swipe for every small event | Accumulate 120 wheel units per swipe; keep multiple notches, bounded to four per event | Native checks cover fractional packets, reversal, multiple notches, zero input, focus reset, timeout and changed picture generation |
| An idle phone cannot be inspected through the agent helper | Standalone `snapshot` exports the retained picture with its original timestamps and age | Actual named-pipe tests cover retained pixels, pause/disconnect/rotation invalidation and PNG metadata; both clients still reject old pictures in normal capture |
| Setup fails without telling the user which step is missing | Read-only `--doctor` reports the next step and refuses ambiguous receiver selection | Helper checks cover missing/duplicate receivers, mirror state, missing image, control readiness, busy input and idle pictures; the running older receiver correctly reports that an upgrade is needed |

The implementation reuses the existing input queue, frame pipe, image writer and test runners. It adds no dependency, service, pairing procedure or permission toggle.

## Compatibility and affected paths

- **Input:** only wheel-event normalization changed. Pointer coordinates, BLE reports, selected-phone routing, drag handling and keyboard behavior use their existing paths. Hardware scroll feel still needs a phone check.
- **Frame protocol:** version 1 gains status and age fields. `ok:true` means a picture exists; consumers must continue checking timestamps. The included Python reader and normal helper commands retain the one-second limit. Retained images are available only while the mirror remains active and unpaused; static and stalled streams cannot be distinguished from pixels alone.
- **Agent commands:** `snapshot` must run alone and never creates a desktop input object. Doctor does not focus a window or change settings. Its exit code is `0` for software readiness and `2` for an issue. Use fresh observations for action verification.
- **Packaging and docs:** both executables take version 0.3.1-rc.2 from CMake. The Windows and source archives include the helper changes and current guides. README, setup runbook, operating guide, frame contract, native-control guide and bundled quick-start describe the same behavior.

Upgrade both executables together. Finish active work, close the old receiver, extract the new package to a new folder and open `Start-AirWindows.cmd`. Reconnect Screen Mirroring and run `iosctl --doctor`. Existing Bluetooth bonds and saved settings are reused.

## Verification

Local validation passed: **218 control/coordinate assertions, 19 discovery/battery assertions, 98 helper checks, 14 Python tests and the native frame-pipe lifecycle test**. The helper's real pipe-to-PNG test exports a 60-second-old synthetic image and checks its pixels, original age and inspection-only sidecar.

Run the checks documented in [the README](../README.md#development), then build and run `FrameServerCheck` and validate the release archives with `scripts/check-package.py`. These checks prove software behavior and package contents. They do not prove physical iPhone scrolling, Home mapping or other adapters. The earlier running development receiver was left intact.
