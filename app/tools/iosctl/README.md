# iosctl: PhonePort from a command line

`iosctl` brings Claude Code's tap, drag, typing, Home and image-inspection workflow to PhonePort. It uses the existing receiver window for input and its decoded-frame pipe for images. It does not start another Bluetooth controller.

The helper runs on Windows x64 with .NET Framework 4.8. Python is optional. Start PhonePort (formerly AirWindows) from the Start menu or with `phoneport --frames`, pair the intended phone and map Button 2 to Home. Follow [the operating guide](../../CLAUDE-CONTROL.md) for those steps. A ZIP build from before 1.0.0 starts with `Start-AirWindows.cmd` instead.

Use input commands only where your agent environment permits desktop automation through a local CLI. This helper does not override tool restrictions. Observation commands read the phone-only pipe and leave focus alone.

## How input reaches the phone

From 1.0.2, input goes to the PhonePort window in the background. `iosctl` posts the window messages that a mouse and keyboard produce to PhonePort's picture area. The window does not have to be in front, other windows can cover it, and the PC's pointer and keyboard are not used. You can keep working in another program while an agent drives the phone. PhonePort still runs its own checks and sends the input over Bluetooth, as it does for a person's clicks.

- The PhonePort window must not be minimized or hidden. Windows gives no focus to such a window, and PhonePort turns phone control on only with it. A refused command says so and sends nothing.
- While a batch runs, phone control is on inside PhonePort and its status bar says so. When the batch ends, control goes off again, unless you have brought PhonePort to the front yourself in the meantime.
- The person at the PC comes first. To take the phone from an agent, click the PhonePort window. While PhonePort is the front window and you are using the mouse or keyboard, scripted input is refused, and it stays refused for a minute after you stop. A mouse button held down on the PhonePort window refuses it too. Click another window to hand the phone back. To stop an agent for good, stop it in its host or end Screen Mirroring.
- While a batch runs, a pointer moved across PhonePort's picture also moves the phone's pointer.

`--foreground` selects the behavior from before 1.0.2. `iosctl` then brings PhonePort to the front, sends real mouse and keyboard events and restores the previous foreground window when it finishes. Windows refuses to bring PhonePort forward while you work in another window; the command reports `Windows denied focus` and sends nothing. `--keep-focus` leaves PhonePort in front afterwards and implies `--foreground`.

## First use

The Microsoft Store install also adds a Start menu entry, "PhonePort command line (iosctl)". Opened from there with no arguments, iosctl prints what it is and the support address and waits for Enter; it does nothing else. The Store install puts `iosctl` on the command path: type `iosctl` in place of `.\iosctl.exe` in the examples below, from any folder. Run outputs such as `.local\phone.png` into a folder you can write to; the install folder is read-only. With a ZIP or source build, run `.\iosctl.exe` from the folder that holds it.

```powershell
.\iosctl.exe --doctor
.\iosctl.exe --list
# Set $receiverPid to the PID at the intended receiver's executable path.
.\iosctl.exe --pid $receiverPid 'preflight'
.\iosctl.exe --pid $receiverPid 'shot ".local\phone.png"'
```

`--list` and `--doctor` find receivers running as `PhonePort.exe`, and as `AirWindowsNative.exe` or `AirWindows.exe` from before the rename.

Open the PNG with your image tool. Choose a visible target, then supply its coordinates in **native phone-image pixels**. The helper reads the current image dimensions and maps them to the displayed picture, including letterboxing and Windows DPI scaling. It rejects a changed receiver, binding or image geometry within a batch.

```powershell
# Replace X and Y with coordinates measured from the current native-size PNG.
.\iosctl.exe --pid $receiverPid 'tap X Y; wait 600; shot ".local\after.png"'
# After a visible text field is focused:
.\iosctl.exe --pid $receiverPid 'type "Bluetooth"; wait 600; shot ".local\text.png"'
```

Output is one JSON object per result. `submitted` means Windows accepted the input sequence for PhonePort; verify the phone's visible result. A screenshot or pixel difference cannot establish that the intended action succeeded.

## Commands

| Command | Behavior |
|---|---|
| `status` | Current frame and receiver control metadata |
| `preflight` | Three consistent, fresh observations with the intended binding ready; no input |
| `shot PATH [scale]` | Save a PNG and JSON sidecar; scale defaults to 1 |
| `snapshot PATH [scale]` | Inspect the retained image while mirroring remains active; reports its age; must run alone |
| `crop PATH X Y W H [zoom]` | Save a region in native source pixels |
| `grid [threshold]` | Brightness-based row/column bands; does not identify apps |
| `wake` | First in a batch only. Moves the phone's pointer with no button so a still screen sends new frames; fails if none arrive |
| `tap X Y` | One left-button press and release |
| `go NAME` | One tap using an explicitly supplied target map |
| `home` | One right-button pulse; requires Button 2 mapped to Home |
| `type "text"` | US ASCII, tabs and newlines; up to 1,024 characters |
| `key enter\|tab\|backspace\|left\|right\|up\|down\|space` | One supported key |
| `swipe X1 Y1 X2 Y2 [ms]` | Paced list drag; default 400 ms |
| `flick X1 Y1 X2 Y2 [ms]` | Linear drag and immediate release; default 180 ms |
| `page left\|right` | One horizontal flick toward the preceding/following page |
| `hold MS` | Set the initial dwell for `flick` and `page`, 0–350 ms. Defaults: `flick` 40 ms (a longer still press on a list row opens the row), `page` 300 ms (Home Screen paging needs the dwell) |
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
| `2` | Doctor found a setup issue, or `settle` ended unchanged/unstable and skipped later commands |
| `3` | Input finished, but cleanup was not confirmed: a held button or key, phone control left on in PhonePort or, with `--foreground`, the previous foreground window |

After any failure, observe again before issuing another action. The helper never retries an input command. It attempts to release held buttons and keys on exit. Presses cannot intentionally remain held across invocations. Input is rejected if the receiver is minimized or hidden, or if its picture area changes size during a gesture. With `--foreground` it is also rejected if the target is covered, focus moves away or the window moves.

One `iosctl` input batch may control a receiver at a time. A competing batch fails immediately. While a primitive `down` is active, only `move`, a short `wait` and `up` are allowed; release before capture or other blocking work. With `--foreground`, held modifiers stop typing.

Normal capture, input and verification reject images older than one second. For an idle screen, run `snapshot` alone: it saves the last decoded picture with `frame_age_ms`, `frame_fresh` and `purpose:inspection_only` in its result. The adjacent JSON preserves the frame's original timestamps. It cannot distinguish a static screen from a stalled stream, so it cannot authorize input or verify an action. Pause, disconnect and screen-generation changes invalidate the retained picture. Never reuse an older PNG after a failed capture.

`iosctl --doctor` finds a single running receiver and reports the next setup step without focus changes or input. It identifies a missing receiver, duplicate receivers, unavailable frame interface, stopped/paused mirroring, missing picture, stale control status, incomplete control connection, busy input and idle image. With several receivers, use `--doctor --pid PID` to inspect one explicitly. It does not alter pairing, firewall settings or processes. Every result includes `data_folder`, where the receiver keeps `settings.ini`, `debug.txt` and `crashes`: `%LOCALAPPDATA%\AirWindows` for the ZIP and source builds, the package's `LocalState` folder for a Microsoft Store install (`data_folder_basis` says which). A `ready` result means software prerequisites passed; test actual click, text and Home behavior on the phone.

`wake` requires the helper from **0.3.1-rc.3 or later**. Doctor and idle snapshots require the receiver from **0.3.1-rc.2 or later**. Older receivers still support fresh captures; upgrade both executables together for the new commands. `preflight` remains the three-observation check before input.

## MCP server

From 1.0.2, `iosctl --mcp` offers the phone to any agent host that speaks the Model Context Protocol (MCP). The host starts it and talks to it over standard input and output. Nothing listens on the network. The server adds no input path of its own: each tool builds a command batch and runs it through the same identity and release checks as the command line, and delivers it the same way, [in the background](#how-input-reaches-the-phone) unless `--foreground` is given.

Start PhonePort and Screen Mirroring first. Then register the server with the host.

```powershell
# Claude Code
claude mcp add phoneport -- iosctl --mcp
```

Hosts that read a JSON file (Claude Desktop, Cursor and others) take the same command and argument:

```json
{
  "mcpServers": {
    "phoneport": { "command": "iosctl", "args": ["--mcp"] }
  }
}
```

With a ZIP or source build, give the full path to `iosctl.exe` as the command.

| Option | Effect |
|---|---|
| `--pid PID` | Use this receiver. Without it the server uses the one running receiver and refuses to choose between several |
| `--max-side N` | Longest side of a returned picture in pixels, 320 to 4096. Default 1280 |
| `--foreground` | Bring PhonePort to the front for each input and send real mouse and keyboard events, as before 1.0.2 |
| `--keep-focus` | With foreground input, leave PhonePort in front after an input instead of restoring the previous window. Implies `--foreground` |

| Tool | Behavior |
|---|---|
| `phoneport_status` | Setup check with the next step, as `--doctor`. No input, no focus change |
| `phoneport_screenshot` | The phone's picture as JPEG or PNG, with its width, height and age. `x`, `y`, `width` and `height` return one region enlarged for reading. No input, no focus change |
| `phoneport_tap` | One tap |
| `phoneport_swipe` | One drag; `flick` makes it fast and straight |
| `phoneport_type` | US ASCII text, 1 to 1,024 characters, into the focused field |
| `phoneport_key` | One of enter, tab, backspace, left, right, up, down, space |
| `phoneport_home` | Home; requires Button 2 mapped to Home |
| `phoneport_batch` | Several input and timing commands in one call. File commands (`shot`, `crop`, `snapshot`) are refused |

What a host should know:

- Coordinates are pixels of the picture the server returns, with 0,0 at the top left. The server shrinks a picture whose longer side exceeds the limit and converts coordinates back to phone pixels itself. Take coordinates from the newest full screenshot.
- Every input tool waits `wait_ms` (default 700, at most 5000) and then returns the picture, so one call both acts and observes. The result also gives `change_permille` and `screen_changed`. They report pixels, not task success.
- A still screen sends no frames. `phoneport_screenshot` then returns the kept picture and says how old it is. An input tool starts with `wake` whenever the newest frame is older than 300 ms.
- Input goes to the PhonePort window in the background, so the user can keep working in another window. A minimized or hidden PhonePort window refuses input. So does a person using the phone: PhonePort in front with the mouse or keyboard used in the last minute, or a mouse button held down on its window. Each refusal says what to ask the user.
- With `--foreground`, input brings the PhonePort window to the front and restores the previous window afterwards. If the user is working in another window, Windows refuses, the tool reports `Windows denied focus` and nothing is sent.
- A tool that fails returns a result marked `isError` with `error`, `input_may_have_been_submitted` and `next`. Nothing is retried. After a failure that may have sent input, look before acting again.
- Standard output carries protocol messages only. Diagnostics go to standard error.
- Pictures go to the host and from there to its model. They can hold private information.

The self-test covers the protocol answers, argument refusal, coordinate conversion and a screenshot read over a real local pipe. It sends no input.

Device run, 2026-11-20: all eight tools ran against an iPhone 14 Pro mirrored to the 1.0.1 receiver from the Microsoft Store, in the background, while another program had the keyboard and mouse. The run opened Spotlight, typed a search, pressed Enter, tapped Calculator keys one at a time and in a batch, went Home and paged the Home Screen both ways. A separate Claude Code session that was given the server opened Calculator and worked out 12 × 12 in 21 seconds with four tool calls. A minimized window refused input and sent nothing. During that run a scripted tap and Home landed while the owner was using the mirrored phone by hand, six seconds after his last click. The rule that a front PhonePort window with a person at the PC refuses input was added because of it and then seen to work: with PhonePort in front and the owner's last input 21 seconds old, a command-line `wake` and an MCP key press were both refused and sent nothing. The same phone then mirrored to the 1.0.2 build itself, with a second paired iPhone connected over Bluetooth: PhonePort let go of the other phone, the mirroring one was chosen, and search, typing, Enter, a batch of taps, Home and paging all worked through the MCP server. Not run that day: `--foreground` with PhonePort in front, the held-mouse-button refusal, and landscape.

## Build and checks

The normal receiver build also builds `iosctl`. To build it separately, run `app\tools\iosctl\build.ps1` from the source root. It uses the Windows .NET Framework C# compiler and takes its version from `app\CMakeLists.txt`.

```powershell
.\app\build\Release\bin\iosctl.exe --self-test
```

Self-tests exercise parsing, bounds, frame identity/freshness, target maps, image conversion, real local-pipe reads/timeouts and release-on-error logic. They send no desktop or Bluetooth input. Two-phone handling through the helper still needs a live-device run.

See [the photo demo](DEMO-TAKE-A-PHOTO.md). The helper is distributed under the application's GPL-3.0-or-later license. Its navigation, sampling and batching approach was adapted from Claude Code's local prototype; personal target maps and session content are excluded.

Implementation references: [Windows foreground rules](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow) and [named-pipe connection behavior](https://learn.microsoft.com/en-us/dotnet/api/system.io.pipes.namedpipeclientstream.connect?view=netframework-4.8.1).
