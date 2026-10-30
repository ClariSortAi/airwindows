# Claude Code: operate the mirrored iPhone

Use this guide for the native Windows Bluetooth build. The phone's image comes from AirPlay; mouse and keyboard input goes through the PC's BLE HID service. The ESP32 is not involved.

For a new user or a fresh PC, complete [the setup runbook](../FOR-AGENTS.md) first. This document covers operating an existing setup.

## Start with the correct interface

The iPhone controls are pixels inside a Windows video window. They are not browser DOM elements or Windows accessibility controls. Browser automation, UI Automation `SetValue`, and searching for an accessible iPhone “Settings” button will not operate them. Windows accessibility tools may expose the AirWindows toolbar itself.

Use the bundled [iosctl helper](tools/iosctl/README.md) where your environment permits local CLI-based desktop input, or a supported Windows desktop tool. The helper supplies taps, typing, paced drags, Home, fresh image capture and preflight. Follow your environment's tool restrictions; a bundled executable does not authorize a prohibited input mechanism. If neither input route is available, identify the missing capability before attempting phone operations.

The decoded-frame pipe is read-only. `iosctl` reads it for observation and sends checked desktop input to the same focused video window; it does not bypass the receiver's queue or Bluetooth identity checks. There is no phone-input HTTP/MCP endpoint. The separate `native_hid.exe` bench provider must not run alongside it. The older ESP `IPhone-Ble.py` and AI KVM MCP actions are not the native GUI control route.

## Locate the running receiver

Run these read-only checks in PowerShell from the repository root:

```powershell
$receiverPath = (Resolve-Path '.\app\build\Release\bin\AirWindowsNative.exe').Path
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

If using a packaged executable, set `$receiverPath` to its actual location. Discover the PID again after a restart. Start the receiver with `--frames` to enable decoded capture. Its window title shows the AirPlay receiver name; the PC's Bluetooth name may differ.

Reuse the running receiver. Do not start a second copy, delete pairings or restart just to acquire focus. A restart interrupts Screen Mirroring. If disconnected, reconnect on the phone through Control Center > Screen Mirroring > the receiver name. Keep the phone unlocked with AssistiveTouch enabled. Use [Native control](NATIVE-CONTROL.md) for first-time pairing and select the intended phone explicitly.

## Observe, act once, observe again

1. Bring the existing AirWindows window forward through your desktop tool. Obtain a fresh desktop screenshot and locate the actual phone picture. Exclude the title bar, control toolbar and letterbox bars.
2. Check that the intended phone is mirroring and the control status is ready. `[CONTROL] Ready=1` means the BLE and AirPlay identities matched at that log timestamp; an old entry does not establish current readiness.
3. Choose one visible target. A left click inside the picture both focuses the video and sends a phone click. It is not a harmless focus-only action. Choose the intended button or text field.
4. Complete one click, text entry or bounded drag. Keep focus in the picture until it finishes. Switching focus to a terminal, chat, toolbar or another window cancels pending input.
5. Allow about 600-700 ms as an initial observation delay, then inspect the result. Animations and longer text may need more time. Verify the expected screen or exact entered text before continuing.
6. If the result is uncertain, observe again and inspect status/logs. Do not automatically repeat the action. A submitted HID report does not prove the phone performed it.

For sharp phone-only images, use the existing reader. This command sends no input and can run without focusing a terminal window:

```powershell
# Install requirements-agent.txt once; run the discovery block first in this session.
python .\app\scripts\observe-phone.py --pid $receiverPid --output .\.local\phone.png
if ($LASTEXITCODE -ne 0) { throw 'No valid fresh phone frame was obtained.' }
```

Open the PNG with your image-reading tool. The adjacent JSON records PID, instance, generation, sequence, dimensions and age. A failed capture may leave an older file on disk: do not mistake it for a new image. Normal readers reject inactive or older-than-one-second frames and accept only real AirPlay frames. For setup diagnosis, run `iosctl --doctor` (or `--doctor --pid PID` when several receivers exist). For idle-image inspection, use `iosctl --pid PID 'snapshot ".local\idle.png"'` alone. Its reported age is meaningful: it cannot distinguish a static screen from a stalled stream and cannot verify an action. Use a fresh `shot` or a permitted live desktop observation before input.

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
| Type | Click the phone's text field, then use ordinary keyboard text input. Printable US ASCII is supported; use short chunks initially and verify the full result. |
| Paste | Explicit Ctrl+V while the video has focus. At most 1,024 ASCII characters including tabs/newlines; Unicode is rejected. A tool that pastes automatically depends on this path. |
| Editing/navigation keys | Enter, Backspace, Tab and arrow keys. Other shortcuts, including Ctrl+A and Cmd+H, are not established phone commands. |
| Scroll | A smooth left-button drag within a list, then release. Start with roughly 0.5-0.9 seconds and modest travel. Wheel input is implemented as a short swipe but still needs its own physical check. |
| Home | AirWindows **Home** toolbar button, or right-click inside the phone picture, after mapping accessory Button 2 to Home. |
| Return to PC | Click outside AirWindows. No Enable step or Escape sequence is required. |

Do not use a long bottom-edge drag for Home; the user found that unreliable. For list drags, send down, paced intermediate moves and up; a hover or single final move is not a complete drag. Always release. Avoid flooding mouse moves: the queue holds 32 actions, pointer actions expire after two seconds, and held input has idle/total time limits.

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
| Frame reader reports stale/inactive or wrong generation | Do not act on the previous PNG. Check mirroring and obtain a valid current observation. Recalculate coordinates after geometry changes. |
| No desktop-input tool is available | State that observation works but GUI input is unavailable in this Claude session. Do not claim a phone or BLE failure. |

## Minimal qualification

Use the user's current task or a harmless Settings search. Record a fresh before image; click one visible target; verify the new screen; click Search and type `Bluetooth`; verify the exact text; click back into a PC window and verify input returns there. Test Home separately after its mapping. Stop at the first failed layer and report the tool used, intended action, before/after result, image dimensions and relevant `[CONTROL]` log lines.

The user confirmed GUI clicking, typing and focus return. Claude Code's later session reports successful taps, list drags, Home and Camera workflows on that running integration build, with intermittent failures. Qualify the current phone and desktop tool through the test above. Paste and wheel still need separate live checks. The policy and helper tests exercise software behavior, not physical phone outcomes. Qualify the integrated helper on the current phone before claiming device acceptance.

See [native control details](NATIVE-CONTROL.md) and [frame protocol](FRAME-INTERFACE.md). Keep phone screenshots local; they may contain personal information.
