# Native Bluetooth control

AirWindows uses the PC's BLE keyboard and mouse service to control an iPhone while AirPlay supplies the picture. No phone app, USB connection or ESP32 is required. Build `AirWindowsNative`; older packaged receivers do not include this integration.

## Pair and connect

1. Start `AirWindowsNative.exe --frames` and connect the phone through Screen Mirroring.
2. Enable AssistiveTouch on the iPhone.
3. Click the phone button at the left of the button row. It reads **Pair Bluetooth** until a phone is paired. In the phone's main Settings > Bluetooth screen, select the PC's Bluetooth name. Its name may differ from a renamed AirPlay receiver.
4. Click the phone button again and choose the phone you are mirroring. Its dot turns green when the Bluetooth and AirPlay identities match. Wait for that before sending input. You choose once for each phone.

The first launch opens a setup guide beside the picture. It follows these steps and ticks each one off when the receiver sees it happen. Reopen it from Menu > Help > Setup guide. In 0.3.1-rc.4 and earlier, steps 3 and 4 are a **Pair Bluetooth** button, a phone list and a **Connect** button, and there is no guide.

The adapter must support BLE peripheral mode. A label saying Connected in AssistiveTouch alone does not prove that the phone subscribed to the HID reports. During the original test, pairing from the main Bluetooth settings established the encrypted link and report subscriptions.

Click inside the picture to focus it and send input. Click another Windows application to return control to the PC. A press within 16 px of the picture, in the bar beside it, snaps to the nearest phone edge, so edge swipes can start in the bar. A click further out in a bar sends nothing and leaves control on. There is no separate Enable step or required Escape sequence. **Disconnect**, in the phone button's menu, releases input and withdraws this process's BLE services.

The application remembers each chosen phone with the AirPlay sender it was chosen for, as `NativeControlPairs` in `%LOCALAPPDATA%\AirWindows\settings.ini`. `NativeControlPhone` and `NativeControlSender` hold the pair used last. On later starts, it advertises for 120 seconds and selects the phone used last. While a phone is selected, publication stays active and other paired phones stay connected in the background. Run one controller process at a time.

## More than one iPhone

Choose each phone once. After that, control follows whichever of them is mirroring. To switch, start Screen Mirroring on the other iPhone. It takes over the picture, and the first phone does not have to stop mirroring. When a different iPhone starts mirroring, AirWindows lets go of the selected phone and selects the one remembered for the new sender. Bluetooth stays open, so nothing is paired or chosen again. If that phone's Bluetooth has not connected yet, the phone button reads **Waiting for iPhone…** and publication reopens for 120 seconds.

A sender that AirWindows has not seen before needs one choice from the phone button. In that list, a phone already chosen for another mirror is marked **(a different iPhone)**. If every listed phone is marked that way, the button reads **Pair this iPhone**: the phone on screen has no Bluetooth link yet, so pair it as in step 3.

Windows cannot tell which Bluetooth phone is the one mirroring, so a wrong first choice is possible. The pointer then moves on the other phone, or the status bar says the selected iPhone is asleep. Choose the right phone and AirWindows remembers the correction. An iPhone's AirPlay identity is its private Wi-Fi address. A phone that joins a different network counts as a new sender and needs choosing once more.

Each phone keeps its own state. A phone that is connected but not selected can lock, sleep or reconnect without stopping input to the selected one. If the selected phone's Bluetooth drops, AirWindows keeps the service published and selects the phone again when it returns. Input reports go only to the selected phone.

The diagnostic `--control-phone <fingerprint>` option binds the first observed AirPlay sender to that explicitly selected phone. Normal pairing uses the phone button; do not copy another machine's fingerprint or use the option on routine launches.

## Home and scrolling

On the iPhone, open Settings > Accessibility > Touch > AssistiveTouch > Devices > your PC > Customize Additional Buttons. Click the AirWindows **Home** button, then assign the detected Button 2 to **Home**.

After that assignment, the Home button or a right-click inside the phone picture invokes Home. A right-click in the bars beside the picture opens the AirWindows menu, as do the Menu button and the Alt key. The Home control sends an 80 ms Button 2 pulse and releases at the same position; it does not require a descriptor or pairing change.

A left-button drag scrolls a list. Mouse-wheel and vertical trackpad input accumulate to one swipe per 120 Windows wheel units, so small trackpad packets no longer each launch a full swipe. Multiple notches are preserved up to four per event. Remainders expire after 500 ms and reset on focus loss, outside-picture scrolling or picture-generation changes. This follows [Windows wheel-delta guidance](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mousewheel). Horizontal trackpad scrolling is not implemented. A mouse drag from the bottom edge did not reliably reproduce Home; use the assigned button instead.

## Text and focus

Supported keyboard input is printable US ASCII, Enter, Backspace, Tab and arrow keys. Ctrl+V or `WM_PASTE` reads the clipboard only when explicitly requested while the video has focus. Paste validates the whole text, normalizes line endings and accepts at most 1,024 ASCII characters, including tabs and newlines. Unicode and other shortcuts are unsupported.

Consecutive text is batched without crossing mouse or key boundaries. Text jobs have a 30-second total deadline and send short chunks through a two-second BLE lease. The queue holds 1,024 pending text characters and 32 actions; pointer and key commands expire after two seconds. Capacity, expiry or Bluetooth failure discards the remainder rather than replaying it.

Focus loss, pause, stream loss, sender change and geometry changes cancel pending actions and release held input. Resume and rotation wait for a newly decoded frame from the current generation. A static picture alone does not expire manual GUI input. Held mouse input releases after two seconds without movement or ten seconds total.

## Verification and troubleshooting

The 2026-10-30 bench observed native typing, relative movement, absolute coordinates, two absolute-button clicks and a list drag on an iPhone. The user confirmed clicking, typing and returning focus through the GUI. Claude Code's later session reports taps, list drags, Home and Camera workflows on that running build, with intermittent failures. Paste and wheel still need separate live checks. A submitted HID report is not proof of a visible result.

The receiver selects by the intended fingerprint, never by subscriber count, and the loss of the selected phone does not select another one. Preserve unrelated bonds.

On 2026-11-11 two iPhones with the same name (an iPhone15,2 and an iPhone16,2) were paired at once on a build after 0.3.1-rc.5. Both reconnected within a second of launch. The idle one locked and slept without affecting input to the selected one. A mirror from the phone that was not selected released the other phone in 0.1 s with Bluetooth left open, and after one choice from the phone button the user controlled it with the mouse. A later mirror from the same phone needed no choice. The selected phone's Bluetooth session dropped four times in half an hour, each time for about a second, and each time the receiver selected it again while the other phone stayed connected. Two cases have policy tests but no hardware result yet: a mirror from a remembered phone while a different phone is selected, and a second phone taking over a mirror that is still running.

Logs are in `%LOCALAPPDATA%\AirWindows\debug.txt`; look for recent `[CONTROL]` entries and the status bar at the bottom of the window. `Mirror sender`, `Released phone` and `Reconnected saved phone` entries show a switch between phones. Repeated Ready=0/Ready=1 transitions indicate instability: stop new input, wait for a stable connection and observe the current screen before continuing.

[The agent guide](CLAUDE-CONTROL.md) gives the operating loop, coordinate mapping and capture commands. [The frame protocol](FRAME-INTERFACE.md) describes the read-only named pipe. Policy tests cover queue cancellation, text ordering, generation changes, letterbox mapping and Button 2 reports; they do not replace a real-phone test.
