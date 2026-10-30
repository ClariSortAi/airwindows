# Native Bluetooth control

AirWindows uses the PC's BLE keyboard and mouse service to control an iPhone while AirPlay supplies the picture. No phone app, USB connection or ESP32 is required. Build `AirWindowsNative`; older packaged receivers do not include this integration.

## Pair and connect

1. Start `AirWindowsNative.exe --frames` and connect the phone through Screen Mirroring.
2. Enable AssistiveTouch on the iPhone.
3. Click **Pair Bluetooth** in AirWindows. In the phone's main Settings > Bluetooth screen, select the PC's Bluetooth name. Its name may differ from a renamed AirPlay receiver.
4. Select the intended subscribed phone in AirWindows and click **Connect**. Wait for matching Bluetooth and AirPlay identities before sending input.

The adapter must support BLE peripheral mode. A label saying Connected in AssistiveTouch alone does not prove that the phone subscribed to the HID reports. During the original test, pairing from the main Bluetooth settings established the encrypted link and report subscriptions.

Click inside the picture to focus it and send input. Click another Windows application to return control to the PC. Letterbox bars are not phone coordinates. There is no separate Enable step or required Escape sequence. Disconnect releases input and withdraws this process's BLE services.

The application remembers `NativeControlPhone` and `NativeControlSender` in `%LOCALAPPDATA%\AirWindows\settings.ini`. On later starts, it advertises for 120 seconds and binds only the saved phone. While bound, publication stays active. A different AirPlay sender invalidates the association and requires selecting its corresponding phone. Run one controller process at a time.

The diagnostic `--control-phone <fingerprint>` option binds the first observed AirPlay sender to that explicitly selected phone. Normal pairing uses the toolbar; do not copy another machine's fingerprint or use the option on routine launches.

## Home and scrolling

On the iPhone, open Settings > Accessibility > Touch > AssistiveTouch > Devices > your PC > Customize Additional Buttons. Click the AirWindows **Home** button, then assign the detected Button 2 to **Home**.

After that assignment, the toolbar Home button or a right-click inside the phone picture invokes Home. Right-click in the bars or chrome opens the PC menu. The keyboard context-menu key also opens the PC menu. The Home control sends an 80 ms Button 2 pulse and releases at the same position; it does not require a descriptor or pairing change.

A left-button drag scrolls a list. Wheel input becomes a bounded vertical swipe. A mouse drag from the bottom edge did not reliably reproduce the phone's Home gesture. Use the assigned button for Home instead.

## Text and focus

Supported keyboard input is printable US ASCII, Enter, Backspace, Tab and arrow keys. Ctrl+V or `WM_PASTE` reads the clipboard only when explicitly requested while the video has focus. Paste validates the whole text, normalizes line endings and accepts at most 1,024 ASCII characters, including tabs and newlines. Unicode and other shortcuts are unsupported.

Consecutive text is batched without crossing mouse or key boundaries. Text jobs have a 30-second total deadline and send short chunks through a two-second BLE lease. The queue holds 1,024 pending text characters and 32 actions; pointer and key commands expire after two seconds. Capacity, expiry or Bluetooth failure discards the remainder rather than replaying it.

Focus loss, pause, stream loss, sender change and geometry changes cancel pending actions and release held input. Resume and rotation wait for a newly decoded frame from the current generation. A static picture alone does not expire manual GUI input. Held mouse input releases after two seconds without movement or ten seconds total.

## Verification and troubleshooting

The 2026-10-30 bench observed native typing, relative movement, absolute coordinates, two absolute-button clicks and a list drag on an iPhone. The user then confirmed clicking, typing and returning focus through the GUI. Home mapping, paste, wheel and agent-driven input remain separate live checks. A submitted HID report is not proof of a visible result.

Logs are in `%LOCALAPPDATA%\AirWindows\debug.txt`; look for recent `[CONTROL]` entries and the current toolbar status. Repeated Ready=0/Ready=1 transitions indicate instability: stop new input, wait for a stable connection and observe the current screen before continuing.

[The agent guide](CLAUDE-CONTROL.md) gives the operating loop, coordinate mapping and capture commands. [The frame protocol](FRAME-INTERFACE.md) describes the read-only named pipe. Policy tests cover queue cancellation, text ordering, generation changes, letterbox mapping and Button 2 reports; they do not replace a real-phone test.
