# AirWindows

Mirror your iPhone on Windows, then click and type in the mirrored screen using the PC's mouse and keyboard.

AirWindows receives video over AirPlay and sends input over Bluetooth. The phone stays wireless. You do not need an iPhone app, an ESP32 or a cable between the phone and PC. Click another Windows app and your keyboard returns to the PC.

This is experimental software. Screen mirroring, absolute pointing, clicks, text entry and list dragging have worked on a real iPhone. The user also confirmed clicking and typing through the Windows mirror and returning focus to the PC. Support across other Bluetooth adapters and iOS versions still needs testing.

## Get started

You need a 64-bit Windows PC, an iPhone on the same reachable network, and a Bluetooth adapter that supports BLE peripheral mode. The native control path has been tested on Windows 11 with a MediaTek adapter. Mirroring also supports iPad and Mac senders; Bluetooth input has been tested with an iPhone.

The current integration is built from source. Older AirWindows downloads do not include these controls.

```powershell
git clone https://github.com/ClariSortAi/airwindows.git
cd airwindows\app
.\scripts\bootstrap.ps1
.\scripts\build.ps1 -Target AirWindowsNative
.\build\Release\bin\AirWindowsNative.exe --frames
```

Install Visual Studio Build Tools with the C++ workload and Windows SDK, Git, CMake and Ninja first. The initial build also compiles FFmpeg, OpenSSL and libplist through pinned vcpkg sources. See [the build guide](app/README.md) for details.

1. Allow AirWindows through Windows Firewall on your private network.
2. On the iPhone, open Control Center > Screen Mirroring and choose the receiver name shown in AirWindows.
3. Turn on Settings > Accessibility > Touch > AssistiveTouch.
4. In AirWindows, click **Pair Bluetooth**. Pair the PC from the iPhone's main Settings > Bluetooth screen, then select that phone in AirWindows and click **Connect**.
5. Click a control inside the mirrored phone. You can now use the mouse and keyboard there.

The app remembers the selected Bluetooth phone and its matching AirPlay sender. Run one receiver at a time. Pairing details and recovery steps are in [Native control](app/NATIVE-CONTROL.md).

## Using it

| Task | Control |
|---|---|
| Tap or select | Left-click inside the phone picture |
| Type | Click a phone text field, then type |
| Scroll a list | Drag with the left button held, then release |
| Go Home | Use the Home button or right-click after assigning Button 2 to Home |
| Return to Windows | Click another application |
| Open AirWindows settings | Right-click outside the phone picture, in the bars or window chrome |
| Full screen | F11 or Alt+Enter; Escape leaves full screen |

For Home, open AssistiveTouch > Devices > your PC > Customize Additional Buttons on the phone. Click **Home** in AirWindows, then assign the detected Button 2 to **Home**. Bottom-edge mouse dragging was unreliable in testing, so use the assigned button for this action.

Text input supports US ASCII. Explicit Ctrl+V accepts up to 1,024 ASCII characters, including tabs and newlines. Paste, wheel input and the Home assignment still need their own live acceptance checks. Unicode input and general keyboard shortcuts are not supported.

## Using a desktop agent

An agent operates the same window you do. It reads the phone image and sends desktop mouse and keyboard input inside the picture. The phone's controls are pixels, so browser selectors and Windows accessibility element lookup cannot identify them.

[The agent guide](app/CLAUDE-CONTROL.md) covers focus, coordinates, frame capture and failure diagnosis. The optional `--frames` interface supplies native-resolution images to programs running as the same Windows user. It is read-only; input still goes through the focused window. Agent-driven input has not yet passed a separate live test.

## Development

```powershell
# From the repository root
python -m pip install -r requirements-dev.txt
.\scripts\check.ps1
python -m pytest app/tests/test_frames.py
python scripts/check-repository.py
```

GitHub Actions runs the policy checks, builds the receiver and tests the frame pipe on Windows. These checks do not pair a phone or prove a physical input result. [Contributing](CONTRIBUTING.md) explains the device tests and what to include in a bug report.

The release script packages the executable, license notices, matching application and Bluetooth source, upstream library sources, and SHA-256 checksums. See [packaging instructions](app/README.md#packaging).

## Project layout

| Directory | Contents |
|---|---|
| `app` | Win32 receiver, renderer, control UI, tests and build scripts |
| `native-ble-prototype` | Shared Windows BLE HID backend and descriptor notice |
| `scripts` | Repository checks |
| `airdrop-native` | Separate, unfinished AirDrop research; not needed for mirroring or control |

AirDrop reception is not operational. Its earlier research and hardware limits remain in [the archive notes](docs/AIRDROP-ARCHIVE.md). AirWindows also does not record the screen or receive video links from an app's cast button. Real-device audio, H.265 and recovery from an unexpected network drop still need testing.

## License

The application is GPL-3.0-or-later. It uses UxPlay, FFmpeg, OpenSSL and libplist, and an MIT-licensed absolute mouse descriptor adapted from iphone-use. See [licenses](LICENSE.md) and [third-party notices](app/THIRD-PARTY.md). AirPlay, iPhone, iPad and Mac are Apple trademarks; this project is not affiliated with Apple.
