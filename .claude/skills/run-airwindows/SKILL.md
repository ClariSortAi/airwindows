---
name: run-airwindows
description: Build, run and drive the PhonePort receiver (PhonePort.exe, formerly AirWindows / AirWindowsNative.exe) without an iPhone. Use when asked to run, start, launch, build or screenshot PhonePort or AirWindows, check a window/menu/capture/record/frame-pipe change in the real app, or drive the receiver window from a script.
---

Win32 C++ receiver. Drive it with `.claude/skills/run-airwindows/driver.ps1`: it launches the real `PhonePort.exe` in self-test mode (a looping H.264 clip stands in for the phone), reads the decoded picture through the frame pipe, screenshots the window and posts menu/toolbar commands, all without taking focus. All paths are relative to the repository root. Use PowerShell 7.

## Prerequisites

- Visual Studio Build Tools (C++ workload, Windows SDK). `cmake` and `ninja` need not be on PATH; `build.ps1` loads the MSVC environment and finds them.
- `ffmpeg` on PATH (makes the clip), Python with `pip install -r requirements-dev.txt` (Pillow for the frame reader).
- A clean clone first needs `./app/scripts/bootstrap.ps1` (fetches UxPlay and vcpkg; the first build compiles OpenSSL, libplist and FFmpeg and takes a long time). It was not re-run here because `app/.deps` already existed.

## Build

```powershell
./app/scripts/build.ps1 -Target AirWindowsNative   # writes bin/PhonePort.exe; also builds iosctl; ~30 s when nothing changed
```

- The skill folder, `.local/run-airwindows/` and `$env:AIRWINDOWS_DRIVER_EXE` keep the old name on purpose (see `AGENTS.md#kept-names-do-not-rename`).
- With no `-Exe`, the driver runs `app/build/Release/bin/PhonePort.exe`, or `AirWindowsNative.exe` there if only a build from before the rename exists.

## Run (agent path)

```powershell
$d = './.claude/skills/run-airwindows/driver.ps1'
& $d launch 300                       # makes the clip if missing; waits for a fresh frame; exits by itself after 300 s
& $d status                           # JSON: pid, title, width/height, sequence, frame_age_ms, source=selftest
& $d frame .local/run-airwindows/f.png   # decoded picture at native size (1170x2532) + .json sidecar
& $d ss .local/run-airwindows/w.png      # whole window incl. toolbar, status bar, guide panel
& $d cmd screenshot                   # also: record, fullscreen, fit, ontop, details, guide, guide-open, guide-close, dump-menu, buy, expire, open-log, open-screenshots, open-recordings, network, support, rate, or a numeric id
& $d log '\[CAPTURE\]|\[RECORD\]'     # log lines written since this launch
& $d stop                             # WM_CLOSE, restores settings.ini, prints video summary + capture/record lines
```

- Look at every PNG you take. `ss` showing the colour bars inside the toolbar and status bar means the D3D path renders.
- Screenshots and recordings from `cmd screenshot`/`cmd record` land in the `capture-HHMMSS` folder printed by `launch` (under `.local/run-airwindows/`, git-ignored).
- Verified flow: `cmd screenshot` -> `[CAPTURE] screenshot 1170x2532 saved`; `cmd record`, wait 3 s, `cmd record` -> `[RECORD] saved ... (3.0 s, 177 frames)`; `cmd dump-menu` -> one `[SELFTEST] menu:` line per item with `checked`/grey state; `cmd guide-open` -> window widens and the setup guide shows in `ss`.
- Always run `stop` before `launch` again; `launch` refuses while the previous PID lives.
- Another build: `& $d launch 300 -Exe app/build/RelWithDebInfo/bin/PhonePort.exe` (or set `$env:AIRWINDOWS_DRIVER_EXE`). Extra receiver arguments: `-ReceiverArgs '--selftest-license','trial:5'`. Use this while the user's receiver runs from `app/build/Release` (that exe is locked; build `-Config RelWithDebInfo`).
- Store licensing UI: `-ReceiverArgs '--selftest-license','<trial:N|expired|licensed>'` simulates the Store's answer (no Store call, nothing saved). `expired` shows the unlock panel and no clip; `launch` then waits for the window only (`-NoFrame` does the same for any run). `cmd buy` simulates a successful purchase: the panel goes and the clip starts without a restart. `cmd dump-menu` lists Help > Buy PhonePort… in `trial:N` and `expired` only, and Help > Rate PhonePort… in `trial:N` and `licensed` only.
- Rating: add `'--selftest-rating','3'` to a `trial:N` or `licensed` run. The status bar shows `Ratings help others find PhonePort: Menu › Help › Rate PhonePort…` for 15 s and the log says `[RATING] showing the one-time line`. `cmd rate` logs `[RATING] self-test: no Store call` and opens nothing. `0`-`2` or `shown`: no line.
- Other monitors: `& $d monitors` lists bounds and DPI; `& $d move <x> <y> <w> <h>` moves and sizes the window without activating it. Keep the whole window on that monitor: parts off screen are not repainted, so `ss` shows stale pixels there.
- `launch 300 bt` keeps the saved phones (see Gotchas). Use it only when testing Bluetooth UI and the user agrees.
- Trial running out while open: `-ReceiverArgs '--selftest-license','trial:5'`, then `cmd expire` (unlock panel, clip stops, `[LICENSE] the license is no longer active`), then `cmd buy` (clip back, `[CONTROL] license active again: restoring Bluetooth control as at launch`).
- Idle screen without the clip: `-NoClip -ReceiverArgs '--selftest-network','public'` (needs a `--selftest-*` flag; the driver refuses otherwise). Add `'--selftest-bluetooth','none'` or `'no-peripheral'` for the adapter notices, or `'probe'` for the real read-only adapter check.
- H.265: `ffmpeg -f lavfi -i testsrc2=size=1170x2532:rate=30 -t 2 -c:v libx265 -pix_fmt yuv420p -f hevc .local/run-airwindows/clip.h265`, then `launch 120 -Clip .local/run-airwindows/clip.h265`. Add `-ReceiverArgs '--decoder','software'` for the FFmpeg path. Verified 2026-11-13: `picture 1170x2532 H.265 (D3D11)` and `(FFmpeg)`.
- `cmd open-log`, `open-screenshots`, `open-recordings`, `network`, `support` write `[SELFTEST] would open the ...: <path, ms-settings URI or support URL>` instead of opening Explorer or Settings (`--selftest-menu`, which the driver always passes).
- `-KnownFolders` leaves out `--capture-dir`: captures land in the real `Pictures\PhonePort` and `Videos\PhonePort`, named `PhonePort <date> <time>`. Never touch `Pictures\AirWindows` or `Videos\AirWindows` (captures from before the rename). Delete exactly the files the `[CAPTURE]`/`[RECORD]` lines name; those folders hold the user's own files.
- Other displays: move first, then size, in two calls (`move 700 1450`, then `move 700 1450 760 1120`). One call that crosses a DPI change gets its size scaled by Windows and can end up partly off screen.

## Packaged (MSIX) runs

- Register: `./app/scripts/package-msix.ps1 -ExePath app\build\RelWithDebInfo\bin\PhonePort.exe -IosctlPath app\build\RelWithDebInfo\bin\iosctl.exe -AllowZeroMajor -OutDir app\dist\<new dir> -Register`. Re-register after every rebuild (unregister first). The dev identity is `GraniteAi.PhonePort.Dev`.
- `& $d launch 120 -Packaged` starts it through the `phoneport.exe` alias (package identity; running the layout's exe by path has none) and reads the log and `settings.ini` in the package's data folder (`& $d datadir -Packaged`). All other commands work unchanged.
- The first packaged launch copies `%LOCALAPPDATA%\AirWindows\settings.ini`, saved phones included. Without `bt` the driver seeds the package's copy without them first. Use `launch 30 bt -Packaged` only for an agreed Bluetooth check.
- `-ReceiverArgs '--selftest-license','store'` asks the real Store, read-only. A local registration answers `active=0 trial=1 sku=(none)`, which maps to `not-store`: no trial line, no Buy item. Never post `cmd buy` in this mode.
- Finish with `./app/scripts/package-msix.ps1 -Unregister`; it removes the package's data folder too.

`AGENTS.md` also lists a one-shot `--selftest-screenshot --selftest-record 4 --exit-after 10` run for capture changes. Not used here: run raw, it binds the user's saved iPhone (see Gotchas). The driver flow above covers the same capture and record paths.

## Test

```powershell
python -m pytest app/tests/test_frames.py
python scripts/check-repository.py
./app/build/Release/bin/iosctl.exe --self-test
./app/build/Release/bin/FrameServerCheck.exe
./app/build/Release/bin/LicenseCheck.exe   # after ./app/scripts/build.ps1 -Target LicenseCheck
```

Full pre-push list (C++ policy checks, MdnsRefreshCheck, RecorderCheck) is in `AGENTS.md#changing-this-repository`.

## Run (human path)

`./app/build/Release/bin/PhonePort.exe --frames` (`AirWindowsNative.exe` in a build from before the rename) starts the real receiver: it advertises AirPlay, publishes Bluetooth and waits for a phone. Not run by this skill. Do not start it unless the user asks; a live phone session belongs to the user.

## Gotchas

- **Self-test still starts Bluetooth control.** With saved phones in `settings.ini`, a `--selftest-video` receiver publishes the HID service, the user's bonded iPhone(s) subscribe within a second, it logs `[CONTROL] Reconnected saved phone` and sends a release report. The driver hides the `NativeControl*` keys during launch so this does not happen; then `log '\[CONTROL\]'` prints nothing. The raw `AGENTS.md` command above does NOT do this.
- **Self-test shares the user's `%LOCALAPPDATA%\AirWindows`** (kept name; `settings.ini`, `debug.txt`, `crashes`); a `-Packaged` run uses the package's `LocalState` instead. `SHGetKnownFolderPath` ignores env overrides. Window placement is saved on close. The driver backs up `settings.ini` (UTF-16LE) at launch, restores it right after the window appears, and again at `stop`. If a run dies without `stop`, copy `.local/run-airwindows/settings.ini.bak` back.
- **Self-test runs skip the single-instance mutex**, so they run beside a real receiver. A non-self-test second launch would instead hand over to the running receiver and exit.
- **`iosctl` and `observe-phone.py` reject self-test frames** (`source=selftest`) by design. The driver calls `AirPlayReader(pid, allow_selftest=True)` from `app/scripts/airwindows_frames.py`.
- **Frames go stale after 1 s.** The self-test clip loops forever (re-announcing the codec each pass), so frames stay fresh; a log line `decoder opened` every 4 s is that loop, not a fault.
- **`cmd screenshot` overwrites the clipboard** (`... and copied to the clipboard`). Only `--selftest-screenshot` skips the clipboard.
- **Refused command ids:** Bluetooth pair/bind/disconnect (41001-41005, 41100-41131) and anything that restarts the AirPlay service (rename, restart, decoder, fps, resolution, H.265). Restarting the service from a self-test receiver can start the real network listener. Do not post these by number either.
- **`cmd` posts and returns immediately.** Wait before reading `log` or files; a recording only exists after the second `record` (or `stop`).
- **`ss` uses `PrintWindow(PW_RENDERFULLCONTENT)`.** It captures the D3D picture while the window is covered, but fails when minimized. The driver sets per-monitor DPI awareness, otherwise sizes come back scaled.
- **`--selftest-video` needs Annex-B H.264/H.265**, not MP4. The driver's `clip` uses `-bsf:v h264_mp4toannexb -f h264`.
- **An `ss` taken within ~1 s of `launch` shows `0 fps · 0.0 Mbps · software decode`** in the status bar. The stats fill in on the 1 s tick; the log already says `picture ... (D3D11)`. Re-take `ss` after 2 s before judging decode or fps.
- The window title shows the receiver name from `settings.ini`; it is not an AirPlay session.

## Troubleshooting

- `unknown command 'restart'` / `id 40002 ... refused` -> by design; see Gotchas.
- `Receiver PID ... is not running` -> `--exit-after` elapsed (default 600 s). Run `log`, then `stop` (restores settings), then `launch`.
- `Driver receiver PID ... still running. Run: stop` -> a previous launch was not stopped.
- `Get-Process` with `-ErrorAction SilentlyContinue` returning nothing still sets exit code 1 in a chained command; check the output, not the code.
