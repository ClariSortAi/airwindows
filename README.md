# PhonePort

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-support-FFDD00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/jasontr)

Mirror your iPhone on Windows, then click and type in the mirrored screen using the PC's mouse and keyboard.

PhonePort (formerly AirWindows) receives video over AirPlay and sends input over Bluetooth. The phone stays wireless. You do not need an iPhone app, an ESP32 or a cable between the phone and PC. Click another Windows app and your keyboard returns to the PC.

PhonePort has been used on real iPhones (an iPhone15,2 and an iPhone16,2) for mirroring, clicking, typing and dragging, and Claude Code has driven navigation, Home and Camera workflows through the bundled helper. Testing so far used one Bluetooth adapter, so other adapters and iOS versions are still unchecked.

## Get started

**[Get PhonePort from the Microsoft Store](https://apps.microsoft.com/detail/9N62X9J9KVT8)**

The Store offers a 7-day free trial, then a one-time purchase. Once it is installed, open PhonePort from the Start menu. The product page is [graniteai.co/PhonePort](https://graniteai.co/PhonePort), and help is at [graniteai.co/PhonePort/Support](https://graniteai.co/PhonePort/Support).

The window has one row of controls: a phone button, Home, Screenshot, Record, Full screen and Menu. A setup guide opens beside the picture on a first launch. Screenshot (F8) saves a PNG and Record (F9) saves an MP4 at a constant frame rate. It keeps working when the PC's network changes, moves sound to a new default playback device, rebuilds its picture after a graphics driver reset and saves a crash report if it stops. The [changelog](app/CHANGELOG.md) lists what was tested, what wasn't, and the known issues, along with earlier releases.

Setting this up with Claude Code or Codex? Give it [FOR-AGENTS.md](FOR-AGENTS.md). It walks through installation, pairing, Home setup and the final agent-operated test.

You need a 64-bit Windows 11 PC, an iPhone on the same reachable network, and a Bluetooth adapter that supports BLE peripheral mode. The native control path has been tested on Windows 11 with a MediaTek adapter. Mirroring also supports iPad and Mac senders; Bluetooth input has been tested with an iPhone.

The setup guide beside the picture walks through these steps and ticks each one off as it happens:

1. Connect the PC to the same network as the iPhone, and make sure Windows treats it as a Private network. The Store install lets PhonePort through Windows Firewall on private networks.
2. On the iPhone, open Control Center > Screen Mirroring and choose the receiver name shown in PhonePort.
3. Turn on Settings > Accessibility > Touch > AssistiveTouch.
4. In PhonePort, click the phone button, which reads **Pair Bluetooth** until a phone is paired. Pair the PC from the iPhone's main Settings > Bluetooth screen, then click the phone button again and choose the phone.
5. Click a control inside the mirrored phone. You can now use the mouse and keyboard there.

The app remembers each Bluetooth phone you choose and the AirPlay sender it belongs to. With more than one iPhone paired, control follows whichever of them is mirroring. Run one receiver at a time. Pairing details and recovery steps are in [Native control](app/NATIVE-CONTROL.md).

## Using it

| Task | Control |
|---|---|
| Tap or select | Left-click inside the phone picture |
| Type | Click a phone text field, then type |
| Scroll a list | Drag with the left button held, or use the vertical wheel/trackpad |
| Go Home | Use the Home button or right-click after assigning Button 2 to Home |
| Return to Windows | Click another application |
| Take a screenshot | Screenshot button or F8; saved in `Pictures\PhonePort` and copied to the clipboard |
| Record the screen | Record button or F9, and again to stop; saved as an MP4 in `Videos\PhonePort` |
| Open PhonePort settings | Menu button, the Alt key, or right-click outside the phone picture |
| Full screen | Full screen button, F11 or Alt+Enter; Escape leaves full screen |

For Home, open AssistiveTouch > Devices > your PC > Customize Additional Buttons on the phone. Click **Home** in PhonePort, then assign the detected Button 2 to **Home**. Dragging up from the bottom edge also goes Home.

Text input supports US ASCII. Explicit Ctrl+V accepts up to 1,024 ASCII characters, including tabs and newlines. Paste and wheel input still need separate live checks. Unicode input and general keyboard shortcuts are not supported. Input goes only to the selected phone when more than one is paired. PhonePort was tried with two iPhones paired at once; [Native control](app/NATIVE-CONTROL.md#verification-and-troubleshooting) lists what was seen and the two cases still untested.

## Using a desktop agent

An agent operates the same window you do. It reads the phone image and sends desktop mouse and keyboard input inside the picture. The phone's controls are pixels, so browser selectors and Windows accessibility element lookup cannot identify them.

[The operating guide](app/CLAUDE-CONTROL.md) covers focus, coordinates, frame capture and failure diagnosis. The Start menu entry and the `phoneport --frames` command enable `--frames`, which supplies native-resolution images to programs running as the same Windows user. It is read-only; input goes through the PhonePort window. The bundled `iosctl` command supplies command-line capture, taps, typing, drags, Home and preflight. From 1.0.2 its input reaches the window in the background, so an agent can drive the phone while you work in another program. Use it where your agent environment permits desktop input through a local CLI, or use a supported desktop tool. Python is optional.

From 1.0.2, an agent host that speaks the Model Context Protocol can use the phone as tools: register `iosctl --mcp` with the host. [The iosctl guide](app/tools/iosctl/README.md#mcp-server) lists the tools and the setup line.

Start with [the iosctl guide](app/tools/iosctl/README.md). The helper uses the selected receiver's actual dimensions, checks input results, releases held buttons on failure and never retries a click automatically. [The Claude Code review](docs/CLAUDE-CODE-REVIEW.md) records what was integrated and what still needs a device test. Personal target maps and recipients are excluded.

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

AirDrop reception is not operational. Its earlier research and hardware limits remain in [the archive notes](docs/AIRDROP-ARCHIVE.md). PhonePort does not receive video links from an app's cast button. Sound from a mirroring iPhone was confirmed on a real phone on 2026-11-13. H.265 from a real device and recovery from an unexpected network drop still need testing.

## Support the project

PhonePort took a lot of time and thought to build. Getting AirPlay mirroring and Bluetooth input to work together on Windows meant plenty of testing on a real iPhone, and the AirDrop research is still unfinished. If PhonePort is useful to you, buying me a coffee helps keep the research going.

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-support-FFDD00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/jasontr)

## License

The application is GPL-3.0-or-later. It uses UxPlay, FFmpeg, OpenSSL and libplist, and an MIT-licensed absolute mouse descriptor adapted from iphone-use. See [licenses](LICENSE.md) and [third-party notices](app/THIRD-PARTY.md). AirPlay, iPhone, iPad and Mac are Apple trademarks; this project is not affiliated with Apple.
