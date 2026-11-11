# Claude Code: operate the mirrored iPhone

Use this guide for the native Windows Bluetooth build. The phone's image comes from AirPlay; mouse and keyboard input goes through the PC's BLE HID service. The ESP32 is not involved.

For a new user or a fresh PC, complete [the setup runbook](../FOR-AGENTS.md) first. This document covers operating an existing setup.

## Start with the correct interface

The iPhone controls are pixels inside a Windows video window. They are not browser DOM elements or Windows accessibility controls. Browser automation, UI Automation `SetValue`, and searching for an accessible iPhone “Settings” button will not operate them. Windows accessibility tools may expose the AirWindows toolbar itself.

Use the bundled [iosctl helper](tools/iosctl/README.md) where your environment permits local CLI-based desktop input, or a supported Windows desktop tool. The helper supplies taps, typing, paced drags, Home, fresh image capture and preflight. Follow your environment's tool restrictions; a bundled executable does not authorize a prohibited input mechanism. If neither input route is available, identify the missing capability before attempting phone operations.

The decoded-frame pipe is read-only. `iosctl` reads it for observation and sends checked desktop input to the same focused video window; it does not bypass the receiver's queue or Bluetooth identity checks. There is no phone-input HTTP/MCP endpoint. The separate `native_hid.exe` bench provider must not run alongside it. The older ESP `IPhone-Ble.py` and AI KVM MCP actions are not the native GUI control route.

## Locate the running receiver

Run these read-only checks in PowerShell from the package folder or repository root:

```powershell
# Package layout first, then a source build.
$receiverPath = @('.\AirWindowsNative.exe', '.\app\build\Release\bin\AirWindowsNative.exe') |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1 | Resolve-Path | ForEach-Object Path
if (-not $receiverPath) { throw 'Receiver executable not found; set $receiverPath to its location.' }
$iosctlExe = Join-Path (Split-Path -Parent $receiverPath) 'iosctl.exe'
$receivers = @(Get-CimInstance Win32_Process | Where-Object {
    $_.Name -eq 'AirWindowsNative.exe' -and $_.ExecutablePath -eq $receiverPath
})
if ($receivers.Count -ne 1) {
    throw 'Expected exactly one native receiver. Inspect processes before proceeding.'
}
$receiverPid = [int]$receivers[0].ProcessId
$receivers | Select-Object ProcessId, ExecutablePath, CommandLine
Get-Content -LiteralPath "$env:LOCALAPPDATA\AirWindows\debug.txt" -Tail 60
```

For another location, set `$receiverPath` and `$iosctlExe` to the actual files. Discover the PID again after a restart. Start the receiver with `--frames` to enable decoded capture. Its window title shows the AirPlay receiver name; the PC's Bluetooth name may differ.

Reuse the running receiver. Do not start a second copy, delete pairings or restart just to acquire focus. A restart interrupts Screen Mirroring. If disconnected, reconnect on the phone through Control Center > Screen Mirroring > the receiver name. Keep the phone unlocked with AssistiveTouch enabled. Use [Native control](NATIVE-CONTROL.md) for first-time pairing and select the intended phone explicitly.

## Observe, act once, observe again

1. With iosctl, run `preflight` and `shot`. Its coordinates are phone pixels, and its input commands bring AirWindows forward themselves. With another desktop tool, bring the existing window forward, obtain a fresh desktop screenshot and locate the actual phone picture, excluding the title bar, the button row above the picture, the status bar below it, the setup guide panel on the right when it is open, and letterbox bars.
2. Check that the intended phone is mirroring and the control status is ready. `[CONTROL] Ready=1` means the BLE and AirPlay identities matched at that log timestamp; an old entry does not establish current readiness.
3. Choose one visible target. A left click inside the picture both focuses the video and sends a phone click. It is not a harmless focus-only action. Choose the intended button or text field.
4. Complete one click, text entry or bounded drag. Keep focus in the picture until it finishes. Switching focus to a terminal, chat, toolbar or another window cancels pending input.
5. Allow about 600-700 ms as an initial observation delay, then inspect the result. Animations and longer text may need more time. Verify the expected screen or exact entered text before continuing.
6. If the result is uncertain, observe again and inspect status/logs. Do not automatically repeat the action. A submitted HID report does not prove the phone performed it.

For sharp phone-only images, use iosctl. This command sends no input and does not take focus:

```powershell
# Run the discovery block first in this session.
& $iosctlExe --pid $receiverPid 'shot ".local\phone.png"'
if ($LASTEXITCODE -ne 0) { throw 'No valid fresh phone frame was obtained.' }
```

The Python reader (`app\scripts\observe-phone.py`, after installing `requirements-agent.txt`) produces the same output and is optional.

Open the PNG with your image-reading tool. The adjacent JSON records PID, instance, generation, sequence, dimensions and age. A failed capture may leave an older file on disk: do not mistake it for a new image. Normal readers reject inactive or older-than-one-second frames and accept only real AirPlay frames. For setup diagnosis, run `iosctl --doctor` (or `--doctor --pid PID` when several receivers exist). For idle-image inspection, use `iosctl --pid PID 'snapshot ".local\idle.png"'` alone. Its reported age is meaningful: it cannot distinguish a static screen from a stalled stream and cannot verify an action. Use a fresh `shot` or a permitted live desktop observation before input.

## Act on a still screen

- A still phone screen sends no frames, and `iosctl` refuses input when the newest frame is older than one second.
- When the screen may be still, put `wake` first in the batch: `iosctl --pid PID 'wake; tap X Y; wait 800; shot ".local\after.png"'`.
- `wake` moves the phone pointer with no button. If no new frame arrives it fails, and no press is sent.
- `wake` is valid only as the first command in a batch.
- Put a whole known sequence in one batch to save round trips, and stop to inspect only before an action that cannot be undone, such as sending a message.

## Coordinates

A decoded PNG contains only the phone image. Its coordinates are not Windows desktop coordinates. Determine the picture rectangle anew after window movement, resize, rotation or display scaling changes.

If the source image is `W x H`, and the displayed phone picture has top-left `(L,T)` and size `Dw x Dh`, map a source pixel `(x,y)` to the desktop tool's coordinate space with:

```text
desktop_x = L + x * Dw / W
desktop_y = T + y * Dh / H
```

Use consistent units: if the desktop tool scales screenshots, follow its screenshot-to-input coordinate rules. Do not assume 100% Windows DPI. Do not pass phone pixels, a full-window rectangle or HID values `0..32767` directly to a desktop click tool. AirWindows performs its own absolute HID mapping after receiving the window input.

## Inputs that this build accepts

| Intent | Operation |
|---|---|
| Tap | Normal left-button down and up at a visible point inside the phone picture. |
| Type | Click the phone's text field and confirm a cursor is visible, then use ordinary keyboard text input. With no field focused, Space and Enter activate the focused control instead. Printable US ASCII is supported; verify the full result. |
| Paste | Explicit Ctrl+V while the video has focus. At most 1,024 ASCII characters including tabs/newlines; Unicode is rejected. A tool that pastes automatically depends on this path. |
| Editing/navigation keys | Enter, Backspace, Tab and arrow keys. Other shortcuts, including Ctrl+A and Cmd+H, are not established phone commands. |
| Scroll | A left-button drag within a list, then release. Start moving at once: a press that rests on a row before moving opens the row. Wheel input becomes short swipes; extra steps are dropped while Bluetooth is busy rather than queued. |
| Edge gesture | Start a drag at the picture edge, or up to 16 px into the bar beside it; the press snaps to the edge. Left edge goes back; bottom edge goes Home. |
| Home | AirWindows **Home** button in the button row, or right-click inside the phone picture, after mapping accessory Button 2 to Home. |
| Save the phone's picture | **Screenshot** button or F8: a native-size PNG in `Pictures\AirWindows`, also copied to the clipboard. For observation keep using `iosctl shot`, which does not touch the clipboard. |
| Record | **Record** button or F9 starts and stops an MP4 with the phone's sound in `Videos\AirWindows`. The status bar shows `REC` and a clock while it runs. The file has a constant frame rate (Video > Frame rate, default 60); a still screen is filled with repeats. |
| Return to PC | Click another application. A click far out in the bars sends nothing and leaves control on. No Enable step or Escape sequence is required. |

Prefer the Home button for Home; a bottom-edge drag also works. For list drags, send down, paced intermediate moves and up; a hover or single final move is not a complete drag. Always release. Avoid flooding mouse moves: the queue holds 32 actions, pointer actions expire after two seconds, and held input has idle/total time limits.

One-time Home setup on the iPhone: Settings > Accessibility > Touch > AssistiveTouch > Devices > your PC > Customize Additional Buttons. Trigger AirWindows Home or right-click inside the mirrored picture, then assign the detected Button 2 to **Home**. Test from an app. Right-click in the bars or chrome intentionally opens the PC menu. A keyboard context-menu command is not the same as right-button input inside the picture.

For a desktop tool that exposes message-based operations: input belongs to the **video child window**, not merely the top-level receiver. The implementation handles `WM_LBUTTONDOWN`, `WM_MOUSEMOVE`, `WM_LBUTTONUP`, right-button release, focused `WM_MOUSEWHEEL`, printable `WM_CHAR`, supported `WM_KEYDOWN` keys, and explicit `WM_PASTE`. Setting an accessibility value or injecting a background message without proper focus is not equivalent to this input sequence.

## Diagnose the failed layer

| Symptom | Check next |
|---|---|
| No phone elements in the accessibility tree | Expected for video. Use image coordinates and desktop input. |
| Picture is correct but clicks miss | Recalculate the displayed picture bounds, toolbar offset and screenshot/DPI scale. Inspect whether the AssistiveTouch floating menu covers the target. |
| Pointer moves but a tap fails | Ensure a complete left down/up sequence reaches the focused video. Do not mix the bench CLI's relative-button path with the GUI's absolute input. |
| Typing goes to Windows or stops partway | The video lost focus, or text/queue limits were reached. Check the field and status before submitting more text. |
| Right-click opens a PC menu | Check that the click lands inside the phone picture, the executable/hash is correct, and the tool sends mouse input rather than a context-menu command. |
| Right-click reaches the phone but does not go Home | Verify this accessory's Button 2 assignment. A pulse is not evidence of the mapping. |
| Repeated Ready=0/Ready=1 or reconnect entries | Treat as connection instability. Stop new input, wait for stable status, then re-observe. Do not spam Pair or replay clicks. |
| Frame reader reports stale/inactive or wrong generation | Do not act on the previous PNG. Check mirroring and obtain a valid current observation. Recalculate coordinates after geometry changes. If only the screen is still, start the batch with `wake`. |
| `Windows denied focus` from `iosctl` | The terminal is not the foreground window. Bring it forward and retry. No press was sent. |
| Status is ready but the wrong phone responds | Bluetooth binding and mirroring are not checked against each other. Confirm the selected phone in AirWindows before sending input. In builds after 0.3.1-rc.5, choose the other phone from the phone button; the receiver remembers that choice for this mirror. |
| Status says the selected iPhone is asleep | The selected phone is probably not the one on screen. Choose the mirroring phone from the phone button. Entries marked `(a different iPhone)` belong to another mirror. |
| Phone button reads `Waiting for iPhone…` during mirroring | The receiver knows this mirror's phone and is waiting for its Bluetooth. Do not choose another phone. If it lasts over a minute, ask the user to open Settings > Bluetooth on that iPhone and tap the PC. |
| No desktop-input tool is available | State that observation works but GUI input is unavailable in this Claude session. Do not claim a phone or BLE failure. |

## Minimal qualification

Use the user's current task or a harmless Settings search. Record a fresh before image; click one visible target; verify the new screen; click Search and type `Bluetooth`; verify the exact text; click back into a PC window and verify input returns there. Test Home separately after its mapping. Stop at the first failed layer and report the tool used, intended action, before/after result, image dimensions and relevant `[CONTROL]` log lines.

Clicking, typing, focus return, list drags and Home have worked on real phones, with intermittent failures. Paste and wheel still need separate live checks. The policy and helper tests exercise software behavior, not physical phone outcomes. Qualify the current phone and desktop tool with the test above before claiming device acceptance.

See [native control details](NATIVE-CONTROL.md) and [frame protocol](FRAME-INTERFACE.md). Keep phone screenshots local; they may contain personal information.
