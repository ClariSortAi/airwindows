# AirWindows application

AirWindows displays an iPhone, iPad or Mac screen in a Windows window. The native control build also sends mouse and keyboard input to a paired iPhone over Bluetooth. See the [repository overview](https://github.com/ClariSortAi/airwindows) for setup and current test limits.

The application is a portable executable. It uses UxPlay for AirPlay, FFmpeg for decoding, Direct3D 11 for video and WASAPI for audio. The Windows mDNS responder advertises on active physical Ethernet and Wi-Fi interfaces.

## Build

Use 64-bit Windows and Visual Studio Build Tools 2019 or later with the C++ workload and Windows SDK. Git, CMake 3.20 or later and Ninja must be available. Allow several GB for dependencies and build output. The native BLE path has been tested on Windows 11; adapter support for BLE peripheral mode is required for input.

From this `app` directory:

```powershell
.\scripts\bootstrap.ps1
.\scripts\build.ps1 -Target AirWindowsNative
.\build\Release\bin\AirWindowsNative.exe --frames
```

Bootstrap fetches pinned UxPlay and vcpkg commits into this directory. The first build compiles OpenSSL, libplist and FFmpeg from source. Later builds reuse the results. The build scripts load the MSVC x64 environment and use Ninja; dependencies are not installed system-wide.

Keep the sibling `native-ble-prototype` directory beside `app`. Both `AirWindowsNative` and `AirWindows` targets compile the same integrated source; the former is the documented control executable. For a ready-to-run build, use [the 0.3.0 preview release](https://github.com/ClariSortAi/airwindows/releases/tag/v0.3.0-preview.1).

The vcpkg commit in `scripts/bootstrap.ps1` pins library versions. `vcpkg-ports/ffmpeg` enables H.264, H.265, AAC and ALAC decoding with D3D11VA acceleration. A custom CMake invocation must use the `x64-windows-static` triplet and the pinned vcpkg toolchain.

## Run and configure

Allow AirWindows through Windows Firewall on the private network used by the phone. Open Control Center > Screen Mirroring on the sender and choose the name shown in the receiver. See [Native control](NATIVE-CONTROL.md) to pair the Bluetooth input connection.

Restart after changing network connections. Wi-Fi and Ethernet may use different firewall profiles. Discovery joins each active physical interface at startup and returns that interface's address to queries on its network.

| Action | Control |
|---|---|
| Full screen | F11 or Alt+Enter; Escape leaves full screen |
| Application menu | Right-click in letterbox bars or window chrome; the keyboard context-menu key also works |
| Rename AirPlay receiver | Receiver > Rename |
| Offered resolution and frame rate | Video > Resolution and Video > Frame rate |
| Hardware/software decoding | Video > Decoder |
| Fit window to picture | View > Fit window to video; leaves room for the control toolbar |
| Video statistics | View > Show status in title bar |

Settings live in `%LOCALAPPDATA%\AirWindows\settings.ini`; the log is `debug.txt` in the same folder. Receiver > Open log folder opens it. The executable is unsigned, so Windows may show a SmartScreen notice for downloaded copies. Check the source and checksum before running a download.

| Option | Purpose |
|---|---|
| `--debug` | Include detailed protocol messages |
| `--frames` | Enable same-user decoded-image access; see [Frame interface](FRAME-INTERFACE.md) |
| `--decoder auto\|hardware\|software` | Override decoding mode for this run |
| `--selftest-video <file>` | Feed an H.264/H.265 Annex-B file through the video pipeline |
| `--selftest-audio` | Open the AAC-ELD, AAC-LC and ALAC decoders with AirPlay parameters |
| `--selftest-tone` | Play a one-second output test tone |
| `--exit-after <seconds>` | Exit after a bounded run |

H.265 is offered only when the GPU can decode it; otherwise the sender uses H.264. Video is drawn after decoding. Audio uses a roughly 60-80 ms buffer and can trail the picture. Real-device sound, H.265 and unexpected network-drop recovery remain untested. Recording, video-link casting, an audio-only now-playing display and additional languages are not implemented.

## Checks

From the repository root, install `requirements-dev.txt`, run `scripts/check.ps1`, then `python -m pytest app/tests/test_frames.py` and `python scripts/check-repository.py`.

From `app`, build and exercise the frame pipe:

```powershell
.\scripts\build.ps1 -Target FrameServerCheck
.\build\Release\bin\FrameServerCheck.exe
```

Protocol checks require a running receiver. Read its port and name from the log:

```powershell
python .\tests\protocol_check.py --port <port> --name <receiver-name>
python .\tests\mdns_interfaces_check.py --port <port> <local-ip> [<local-ip> ...]
```

These checks cover discovery, `/info`, pair setup/verification and the first FairPlay phase. They do not prove discovery from another device. The frame tests cover lifecycle, geometry and freshness. The native policy checks exercise input ordering, cancellation and report construction without publishing Bluetooth services.

For video self-tests, feed an Annex-B clip through `--selftest-video`. The log records decode counts and whether the GPU was used. For a real phone, use [the agent guide's observation loop](CLAUDE-CONTROL.md) and record the visible result of each action.

## Packaging

After building, use a new output directory:

```powershell
.\scripts\package.ps1 -Release -OutDir .\dist\candidate-0.3.0
```

This creates the runnable folder, `AirWindows-0.3.0-win-x64.zip`, a matching source zip, and `SHA256SUMS.txt`. Existing package outputs are never overwritten. The Windows ZIP includes the launcher, agent setup guide, Python frame reader, GPL and dependency notices, including iphone-use's MIT notice.

The source archive contains `app`, its sibling Bluetooth backend, UxPlay, and the upstream FFmpeg, OpenSSL and libplist source archives with the relevant ports. Those archives must be present in vcpkg's downloads directory. Publish the matching source and checksums with any binary. The separate AirDrop research is not bundled.

## Source map

| Source | Responsibility |
|---|---|
| `third_party/UxPlay/lib` | AirPlay pairing, FairPlay and streams; fetched by bootstrap |
| `src/mdnsd_win.c` | Discovery on active Windows interfaces |
| `src/airplay_server.cpp`, `src/media_sink.h` | Server callbacks and media interface |
| `src/video_pipeline.cpp`, `src/frame_server.cpp` | Decode/render and optional frame export |
| `src/audio_pipeline.cpp` | Decode, resample and WASAPI output |
| `src/main.cpp`, `src/settings.cpp` | Window, input routing and settings |
| `src/native_control.cpp`, `src/control_queue.h` | Phone identity, focus and action queue |
| `../native-ble-prototype` | Windows BLE HID backend |

`compat/msvc` supplies POSIX compatibility for UxPlay without editing its source. Its mutex shim uses zero-initializable SRW locks because UxPlay can lock a mutex without explicitly initializing it.

The application is GPL-3.0-or-later. See [LICENSE](LICENSE) and [THIRD-PARTY.md](THIRD-PARTY.md). AirPlay, iPhone, iPad and Mac are Apple trademarks; this project is not affiliated with Apple.
