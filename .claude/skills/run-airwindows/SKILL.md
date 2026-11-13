---
name: run-airwindows
description: Build, run and drive the AirWindows receiver (AirWindowsNative.exe) without an iPhone. Use when asked to run, start, launch, build or screenshot AirWindows, check a window/menu/capture/record/frame-pipe change in the real app, or drive the receiver window from a script.
---

Win32 C++ receiver. Drive it with `.claude/skills/run-airwindows/driver.ps1`: it launches the real `AirWindowsNative.exe` in self-test mode (a looping H.264 clip stands in for the phone), reads the decoded picture through the frame pipe, screenshots the window and posts menu/toolbar commands, all without taking focus. All paths are relative to the repository root. Use PowerShell 7.

## Prerequisites

- Visual Studio Build Tools (C++ workload, Windows SDK). `cmake` and `ninja` need not be on PATH; `build.ps1` loads the MSVC environment and finds them.
- `ffmpeg` on PATH (makes the clip), Python with `pip install -r requirements-dev.txt` (Pillow for the frame reader).
- A clean clone first needs `./app/scripts/bootstrap.ps1` (fetches UxPlay and vcpkg; the first build compiles OpenSSL, libplist and FFmpeg and takes a long time). It was not re-run here because `app/.deps` already existed.

## Build

```powershell
./app/scripts/build.ps1 -Target AirWindowsNative   # also builds iosctl; ~30 s when nothing changed
```

## Run (agent path)

```powershell
$d = './.claude/skills/run-airwindows/driver.ps1'
& $d launch 300                       # makes the clip if missing; waits for a fresh frame; exits by itself after 300 s
& $d status                           # JSON: pid, title, width/height, sequence, frame_age_ms, source=selftest
& $d frame .local/run-airwindows/f.png   # decoded picture at native size (1170x2532) + .json sidecar
& $d ss .local/run-airwindows/w.png      # whole window incl. toolbar, status bar, guide panel
& $d cmd screenshot                   # also: record, fullscreen, fit, ontop, details, guide, guide-open, guide-close, dump-menu, or a numeric id
& $d log '\[CAPTURE\]|\[RECORD\]'     # log lines written since this launch
& $d stop                             # WM_CLOSE, restores settings.ini, prints video summary + capture/record lines
```

- Look at every PNG you take. `ss` showing the colour bars inside the toolbar and status bar means the D3D path renders.
- Screenshots and recordings from `cmd screenshot`/`cmd record` land in the `capture-HHMMSS` folder printed by `launch` (under `.local/run-airwindows/`, git-ignored).
- Verified flow: `cmd screenshot` -> `[CAPTURE] screenshot 1170x2532 saved`; `cmd record`, wait 3 s, `cmd record` -> `[RECORD] saved ... (3.0 s, 177 frames)`; `cmd dump-menu` -> one `[SELFTEST] menu:` line per item with `checked`/grey state; `cmd guide-open` -> window widens and the setup guide shows in `ss`.
- Always run `stop` before `launch` again; `launch` refuses while the previous PID lives.
- `launch 300 bt` keeps the saved phones (see Gotchas). Use it only when testing Bluetooth UI and the user agrees.

`AGENTS.md` also lists a one-shot `--selftest-screenshot --selftest-record 4 --exit-after 10` run for capture changes. Not used here: run raw, it binds the user's saved iPhone (see Gotchas). The driver flow above covers the same capture and record paths.

## Test

```powershell
python -m pytest app/tests/test_frames.py
python scripts/check-repository.py
./app/build/Release/bin/iosctl.exe --self-test
./app/build/Release/bin/FrameServerCheck.exe
```

Full pre-push list (C++ policy checks, MdnsRefreshCheck, RecorderCheck) is in `AGENTS.md#changing-this-repository`.

## Run (human path)

`./app/build/Release/bin/AirWindowsNative.exe --frames` starts the real receiver: it advertises AirPlay, publishes Bluetooth and waits for a phone. Not run by this skill. Do not start it unless the user asks; a live phone session belongs to the user.

## Gotchas

- **Self-test still starts Bluetooth control.** With saved phones in `settings.ini`, a `--selftest-video` receiver publishes the HID service, the user's bonded iPhone(s) subscribe within a second, it logs `[CONTROL] Reconnected saved phone` and sends a release report. The driver hides the `NativeControl*` keys during launch so this does not happen; then `log '\[CONTROL\]'` prints nothing. The raw `AGENTS.md` command above does NOT do this.
- **Self-test shares the user's `%LOCALAPPDATA%\AirWindows`** (`settings.ini`, `debug.txt`, `crashes`). `SHGetKnownFolderPath` ignores env overrides. Window placement is saved on close. The driver backs up `settings.ini` (UTF-16LE) at launch, restores it right after the window appears, and again at `stop`. If a run dies without `stop`, copy `.local/run-airwindows/settings.ini.bak` back.
- **Self-test runs skip the single-instance mutex**, so they run beside a real receiver. A non-self-test second launch would instead hand over to the running receiver and exit.
- **`iosctl` and `observe-phone.py` reject self-test frames** (`source=selftest`) by design. The driver calls `AirPlayReader(pid, allow_selftest=True)` from `app/scripts/airwindows_frames.py`.
- **Frames go stale after 1 s.** The self-test clip loops forever (re-announcing the codec each pass), so frames stay fresh; a log line `decoder opened` every 4 s is that loop, not a fault.
- **`cmd screenshot` overwrites the clipboard** (`... and copied to the clipboard`). Only `--selftest-screenshot` skips the clipboard.
- **Refused command ids:** Bluetooth pair/bind/disconnect (41001-41005, 41100-41131) and anything that restarts the AirPlay service (rename, restart, decoder, fps, resolution, H.265). Restarting the service from a self-test receiver can start the real network listener. Do not post these by number either.
- **`cmd` posts and returns immediately.** Wait before reading `log` or files; a recording only exists after the second `record` (or `stop`).
- **`ss` uses `PrintWindow(PW_RENDERFULLCONTENT)`.** It captures the D3D picture while the window is covered, but fails when minimized. The driver sets per-monitor DPI awareness, otherwise sizes come back scaled.
- **`--selftest-video` needs Annex-B H.264/H.265**, not MP4. The driver's `clip` uses `-bsf:v h264_mp4toannexb -f h264`.
- The window title shows the receiver name from `settings.ini`; it is not an AirPlay session.

## Troubleshooting

- `unknown command 'restart'` / `id 40002 ... refused` -> by design; see Gotchas.
- `Receiver PID ... is not running` -> `--exit-after` elapsed (default 600 s). Run `log`, then `stop` (restores settings), then `launch`.
- `Driver receiver PID ... still running. Run: stop` -> a previous launch was not stopped.
- `Get-Process` with `-ErrorAction SilentlyContinue` returning nothing still sets exit code 1 in a chained command; check the output, not the code.
