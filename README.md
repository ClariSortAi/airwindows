# AirWindows

Mirror your iPhone on Windows, then click and type in the mirrored screen using the PC's mouse and keyboard.

AirWindows receives video over AirPlay and sends input over Bluetooth. The phone stays wireless. You do not need an iPhone app, an ESP32 or a cable between the phone and PC. Click another Windows app and your keyboard returns to the PC.

Earlier builds have been used on a real iPhone for mirroring, clicking, typing and dragging. Claude Code also exercised navigation, Home and Camera workflows. This release candidate integrates that helper; live acceptance of the revised helper and wider adapter/iOS support is still pending.

## Get started

**[Download AirWindows for Windows x64](https://github.com/ClariSortAi/airwindows/releases/download/v0.3.1-rc.2/AirWindows-0.3.1-rc.2-win-x64.zip)** · [Release notes, source and checksums](https://github.com/ClariSortAi/airwindows/releases/tag/v0.3.1-rc.2)

Extract the ZIP and double-click **Start-AirWindows.cmd**. The package includes the receiver, `iosctl.exe`, frame reader, licenses and setup guides. No compiler or GitHub account is required.

This candidate fixes three first-day problems:

- **Trackpad overscroll:** small wheel events accumulate into one swipe instead of each launching a full swipe.
- **Idle-screen inspection:** `iosctl --pid PID 'snapshot idle.png'` saves the retained picture with its age. Fresh capture and input checks remain strict.
- **Setup diagnosis:** `iosctl --doctor` finds the receiver and reports the next step, including duplicate processes, missing mirroring and incomplete control connections. It sends no input.

[Changes and verification](docs/DAY-ONE-RC2.md) cover compatibility, tests and remaining device checks.

Setting this up with Claude Code or Codex? Give it [FOR-AGENTS.md](FOR-AGENTS.md), also included in the ZIP. It walks through installation, pairing, Home setup and the final agent-operated test.

You need a 64-bit Windows PC, an iPhone on the same reachable network, and a Bluetooth adapter that supports BLE peripheral mode. The native control path has been tested on Windows 11 with a MediaTek adapter. Mirroring also supports iPad and Mac senders; Bluetooth input has been tested with an iPhone.

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
| Scroll a list | Drag with the left button held, or use the vertical wheel/trackpad |
| Go Home | Use the Home button or right-click after assigning Button 2 to Home |
| Return to Windows | Click another application |
| Open AirWindows settings | Right-click outside the phone picture, in the bars or window chrome |
| Full screen | F11 or Alt+Enter; Escape leaves full screen |

For Home, open AssistiveTouch > Devices > your PC > Customize Additional Buttons on the phone. Click **Home** in AirWindows, then assign the detected Button 2 to **Home**. Bottom-edge mouse dragging was unreliable in testing, so use the assigned button for this action.

Text input supports US ASCII. Explicit Ctrl+V accepts up to 1,024 ASCII characters, including tabs and newlines. Paste and wheel input still need separate live checks. Unicode input and general keyboard shortcuts are not supported. This candidate keeps input bound to the selected phone when another subscriber appears. The two-phone behavior still needs a live-device check.

## Using a desktop agent

An agent operates the same window you do. It reads the phone image and sends desktop mouse and keyboard input inside the picture. The phone's controls are pixels, so browser selectors and Windows accessibility element lookup cannot identify them.

[The operating guide](app/CLAUDE-CONTROL.md) covers focus, coordinates, frame capture and failure diagnosis. The launcher enables `--frames`, which supplies native-resolution images to programs running as the same Windows user. It is read-only; input goes through the focused window. The bundled `iosctl.exe` supplies command-line capture, taps, typing, drags, Home and preflight. Use it where your agent environment permits desktop input through a local CLI, or use a supported desktop tool. Python is optional.

Start with [the iosctl guide](app/tools/iosctl/README.md). The helper uses the selected receiver's actual dimensions, checks input/focus results, releases held buttons on failure and never retries a click automatically. [The Claude Code review](docs/CLAUDE-CODE-REVIEW.md) records what was integrated and what still needs a device test. Personal target maps and recipients are excluded.

## Development

To build the receiver, install Visual Studio C++ Build Tools, a Windows SDK, Git, CMake and Ninja, then follow [the build guide](app/README.md#build). The first build compiles the pinned dependencies.

```powershell
# From the repository root
python -m pip install -r requirements-dev.txt
.\scripts\check.ps1
python -m pytest app/tests/test_frames.py
python scripts/check-repository.py
.\app\tools\iosctl\build.ps1
.\app\build\Release\bin\iosctl.exe --self-test
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
