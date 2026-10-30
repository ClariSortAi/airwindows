# iosctl: AirWindows from a command line

`iosctl.exe` brings Claude Code's tap, drag, typing, Home and image-inspection workflow into the Windows package. It uses the existing receiver window for input and its decoded-frame pipe for images. It does not start another Bluetooth controller.

The helper runs on Windows x64 with .NET Framework 4.8. Python is optional. Start AirWindows with **Start-AirWindows.cmd**, pair the intended phone and map Button 2 to Home. Follow [the operating guide](../../CLAUDE-CONTROL.md) for those steps.

Use input commands only where your agent environment permits desktop automation through a local CLI. This helper does not override tool restrictions. Observation commands read the phone-only pipe and leave focus alone. Input commands bring AirWindows forward, check focus and restore the previous foreground window when finished; `--keep-focus` leaves AirWindows active.

## First use

Run from the extracted `AirWindows` folder:

```powershell
.\iosctl.exe --list
# Set $receiverPid to the PID at the intended receiver's executable path.
.\iosctl.exe --pid $receiverPid 'preflight'
.\iosctl.exe --pid $receiverPid 'shot ".local\phone.png"'
```

Open the PNG with your image tool. Choose a visible target, then supply its coordinates in **native phone-image pixels**. The helper reads the current image dimensions and maps them to the displayed picture, including letterboxing and Windows DPI scaling. It rejects a changed receiver, binding or image geometry within a batch.

```powershell
# Replace X and Y with coordinates measured from the current native-size PNG.
.\iosctl.exe --pid $receiverPid 'tap X Y; wait 600; shot ".local\after.png"'
# After a visible text field is focused:
.\iosctl.exe --pid $receiverPid 'type "Bluetooth"; wait 600; shot ".local\text.png"'
```

Output is one JSON object per result. `submitted` means Windows accepted the input sequence; verify the phone's visible result. A screenshot or pixel difference cannot establish that the intended action succeeded.

## Commands

| Command | Behavior |
|---|---|
| `status` | Current frame and receiver control metadata |
| `preflight` | Three consistent, fresh observations with the intended binding ready; no input |
| `shot PATH [scale]` | Save a PNG and JSON sidecar; scale defaults to 1 |
| `crop PATH X Y W H [zoom]` | Save a region in native source pixels |
| `grid [threshold]` | Brightness-based row/column bands; does not identify apps |
| `tap X Y` | One left-button press and release |
| `go NAME` | One tap using an explicitly supplied target map |
| `home` | One right-button pulse; requires Button 2 mapped to Home |
| `type "text"` | US ASCII, tabs and newlines; up to 1,024 characters |
| `key enter\|tab\|backspace\|left\|right\|up\|down\|space` | One supported key |
| `swipe X1 Y1 X2 Y2 [ms]` | Paced list drag; default 400 ms |
| `flick X1 Y1 X2 Y2 [ms]` | Linear drag and immediate release; default 180 ms |
| `page left\|right` | One horizontal flick toward the preceding/following page |
| `hold MS` | Set flick's initial dwell, 0–350 ms; default 300 |
| `down X Y; move X Y; up X Y` | Bounded drag primitives; down/up must share a batch |
| `wait MS` | Fixed wait, at most 5 seconds per command |
| `changed` | Compare pixels with the frame before the last input |
| `settle [ms]` | Wait for a visible change followed by stability; default 2.5 seconds |
| `targets` | Print the supplied target map |

Pass one quoted command batch. Semicolons inside quoted text remain text. For example, PowerShell can pass `'type "first; second"'`. Quote paths containing spaces. The full batch is parsed before input begins; unknown commands and invalid coordinates stop it. `tapv` and `gov` are deliberately rejected: there is no automatic input replay.

Prefer short batches ending with an observation. Capture and inspect a new image before choosing an action on an unfamiliar screen. `settle` ignores the animated status area; a live viewfinder may never settle. Use `wait` and `shot` for Camera. `changed` reports pixels, not task success.

## Target maps

Copy `targets.example.json` into a local, untracked folder. Set `width` and `height` from a fresh native-size image, then define targets you have actually observed. Supply the file explicitly:

```powershell
.\iosctl.exe --pid $receiverPid --targets '.local\targets.json' 'targets'
.\iosctl.exe --pid $receiverPid --targets '.local\targets.json' 'go search; wait 600; shot ".local\after.png"'
```

The example map contains no app locations. Matching dimensions do not establish the same layout: confirm the page, orientation, overlays and visible target before using a name. Recreate the map after rearranging icons. A scaled or cropped screenshot's JSON includes the source dimensions and crop offsets; convert back to native pixels before defining a target.

## Results and recovery

| Exit code | Meaning |
|---|---|
| `0` | Commands completed; input results still need visual verification |
| `1` | Invalid command, unavailable frame, focus/identity change or input failure; batch stopped |
| `2` | `settle` ended unchanged or unstable; later commands were skipped |
| `3` | Input finished, but cleanup or foreground restoration was not confirmed |

After any failure, observe again before issuing another action. The helper never retries an input command. It attempts to release held buttons and keys on exit. Presses cannot intentionally remain held across invocations. Input is rejected if the receiver is minimized, the target is covered, focus moves away or the window changes geometry during a gesture.

One `iosctl` input batch may control a receiver at a time. A competing batch fails immediately. While a primitive `down` is active, only `move`, a short `wait` and `up` are allowed; release before capture or other blocking work. Held modifiers stop typing.

The frame reader rejects inactive and older-than-one-second images. A static phone screen can age out even when the mirror remains connected. Use a permitted live desktop observation or wait for a visible update; do not reuse an older PNG after a failed capture. `preflight` establishes current connection readiness, not successful physical input.

## Build and checks

The normal receiver build also builds `iosctl`. To build it separately, run `app\tools\iosctl\build.ps1` from the source root. It uses the Windows .NET Framework C# compiler and takes its version from `app\CMakeLists.txt`.

```powershell
.\app\build\Release\bin\iosctl.exe --self-test
```

Self-tests exercise parsing, bounds, frame identity/freshness, target maps, image conversion, real local-pipe reads/timeouts and release-on-error logic. They send no desktop or Bluetooth input. The integrated helper and two-phone handling still need a live-device acceptance run for this release candidate.

See [the photo demo](DEMO-TAKE-A-PHOTO.md). The helper is distributed under the application's GPL-3.0-or-later license. Its navigation, sampling and batching approach was adapted from Claude Code's local prototype; personal target maps and session content are excluded.

Implementation references: [Windows foreground rules](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow) and [named-pipe connection behavior](https://learn.microsoft.com/en-us/dotnet/api/system.io.pipes.namedpipeclientstream.connect?view=netframework-4.8.1).
