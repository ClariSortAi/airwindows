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

Keep the sibling `native-ble-prototype` directory beside `app`. Both `AirWindowsNative` and `AirWindows` targets compile the same integrated source; the former is the documented control executable. For a ready-to-run build, use [the 0.3.1 release candidate](https://github.com/ClariSortAi/airwindows/releases/tag/v0.3.1-rc.4).

The vcpkg commit in `scripts/bootstrap.ps1` pins library versions. `vcpkg-ports/ffmpeg` enables H.264, H.265, AAC and ALAC decoding with D3D11VA acceleration. A custom CMake invocation must use the `x64-windows-static` triplet and the pinned vcpkg toolchain.

## Run and configure

Allow AirWindows through Windows Firewall on the private network used by the phone. Open Control Center > Screen Mirroring on the sender and choose the name shown in the receiver. See [Native control](NATIVE-CONTROL.md) to pair the Bluetooth input connection.

AirWindows follows the PC's network connections while it runs. When Wi-Fi reconnects, a cable is plugged in or the address changes, discovery joins that network within a few seconds, and the receiver can be started before any network is up. Discovery uses each active physical interface and returns that interface's address to queries on its network. Wi-Fi and Ethernet may use different firewall profiles.

Starting AirWindows while it is already running brings the running window forward instead of opening a second receiver. If the new launch asked for `--frames`, the running receiver turns frame access on.

The window has one row of buttons above the picture and a status bar below it. From the left: the phone button (pair Bluetooth, choose a phone, disconnect), then Home, Screenshot, Record, Full screen and Menu. In a narrow window the middle buttons keep only their icons.

| Action | Control |
|---|---|
| Screenshot | Screenshot button or F8. Saves the phone's picture at its own size as a PNG in `Pictures\AirWindows` and copies it to the clipboard |
| Record | Record button or F9; press again to stop. Saves an MP4 (H.264 video at a constant frame rate, AAC sound) in `Videos\AirWindows`. A new file starts if the phone rotates |
| Full screen | Full screen button, F11 or Alt+Enter; Escape leaves. The buttons and status bar hide while it is on |
| Menu | Menu button, the Alt key, or a right-click outside the picture |
| Setup guide | Opens beside the picture on a first launch; Help > Setup guide brings it back |
| Rename AirPlay receiver | Receiver > Rename |
| Offered resolution and frame rate | Video > Resolution and Video > Frame rate |
| Hardware/software decoding | Video > Decoder |
| Fit window to picture | View > Fit window to video; leaves room for the buttons and status bar |
| Video statistics | View > Stream details in status bar |

Rename, Resolution, Frame rate, Decoder and the H.265 setting restart the AirPlay service, which ends a session in progress. The idle screen then says so; start Screen Mirroring again.

Recording uses the H.264 and AAC encoders that come with Windows. It encodes on the GPU when the PC has a hardware encoder. A recording has a constant frame rate: the one set under Video > Frame rate, 60 unless changed. A phone sends a picture only when its screen changes, so the recorder repeats the last picture until the next one arrives. Every frame in the file is then the same length, which video editors expect. A file is complete only after Record is pressed again or AirWindows is closed normally. Windows N editions need the Media Feature Pack for recording; everything else works without it.

Settings live in `%LOCALAPPDATA%\AirWindows\settings.ini`; the log is `debug.txt` in the same folder. Receiver > Open log folder opens it. At 4 MB the log is renamed `debug.old.txt` and a new one starts. If AirWindows stops unexpectedly, it saves a crash report in the `crashes` folder there, keeps the five newest and shows a message with the location. Reports stay on the PC unless you send one. The window reopens where you left it. The executable is unsigned, so Windows may show a SmartScreen notice for downloaded copies. Check the source and checksum before running a download.

| Option | Purpose |
|---|---|
| `--debug` | Include detailed protocol messages |
| `--frames` | Enable same-user decoded-image access; see [Frame interface](FRAME-INTERFACE.md) |
| `--decoder auto\|hardware\|software` | Override decoding mode for this run |
| `--capture-dir <folder>` | Save screenshots and recordings in this folder instead of Pictures and Videos |
| `--selftest-video <file>` | Feed an H.264/H.265 Annex-B file through the video pipeline |
| `--selftest-audio` | Open the AAC-ELD, AAC-LC and ALAC decoders with AirPlay parameters |
| `--selftest-tone` | Play a one-second output test tone |
| `--selftest-audio-switch` | Silently report a new default playback device and log the stream moving to it |
| `--selftest-device-loss <seconds>` | Report the graphics device lost after this long, to exercise recovery |
| `--selftest-crash access\|terminate` | Fail on purpose, to exercise the crash report |
| `--selftest-screenshot` | With `--selftest-video`: save one screenshot once the picture is up (not copied to the clipboard) |
| `--selftest-record <seconds>` | With `--selftest-video`: record for this long once the picture is up |
| `--selftest-menu` | Write the menu's labels, check marks and grey-outs to the log when command 41200 is posted to the window |
| `--exit-after <seconds>` | Exit after a bounded run; a failure then saves its report without a dialog |

H.265 is offered only when the GPU can decode it; otherwise the sender uses H.264. If the graphics device is reset or removed, by a driver update for example, the session ends and the receiver rebuilds its video path. Start Screen Mirroring again to continue. Video is drawn after decoding. Audio uses a roughly 60-80 ms buffer and can trail the picture. It plays on Windows' default playback device and moves when that changes, for example when headphones are plugged in. Real-device sound, H.265 and unexpected network-drop recovery remain untested. Video-link casting, an audio-only now-playing display and additional languages are not implemented.

## Checks

From the repository root, install `requirements-dev.txt`, run `scripts/check.ps1`, then `python -m pytest app/tests/test_frames.py` and `python scripts/check-repository.py`.

From `app`, build and exercise the frame pipe:

```powershell
.\scripts\build.ps1 -Target FrameServerCheck
.\build\Release\bin\FrameServerCheck.exe
.\scripts\build.ps1 -Target MdnsRefreshCheck
.\build\Release\bin\MdnsRefreshCheck.exe
```

`MdnsRefreshCheck` scripts an adapter that appears, changes address and disappears on the loopback interface, and asks the responder for the AirPlay service after each step. It does not touch a real adapter.

Protocol checks require a running receiver. Read its port and name from the log:

```powershell
python .\tests\protocol_check.py --port <port> --name <receiver-name>
python .\tests\mdns_interfaces_check.py --port <port> <local-ip> [<local-ip> ...]
```

These checks cover discovery, `/info`, pair setup/verification and the first FairPlay phase. They do not prove discovery from another device. The frame tests cover lifecycle, geometry and freshness. The native policy checks exercise input ordering, cancellation and report construction without publishing Bluetooth services.

For video self-tests, feed an Annex-B clip through `--selftest-video`. The log records decode counts and whether the GPU was used. Add `--selftest-screenshot`, `--selftest-record 4` and `--capture-dir <folder>` to exercise capture on the same clip; the log names each file written.

`RecorderCheck` (`.\scripts\build.ps1 -Target RecorderCheck`) tests the recorder on its own with generated pictures and a tone, then reads the MP4 files back. It pushes pictures at the file's rate in bunches of four, at half of it, at twice it and not at all, and checks each time that every frame sits in its place and that no bunched picture is lost. It needs the Windows H.264 encoder and runs in real time, so it is not part of CI. For a real phone, use [the agent guide's observation loop](CLAUDE-CONTROL.md) and record the visible result of each action.

## Packaging

After building, use a new output directory:

```powershell
.\scripts\package.ps1 -Release -OutDir .\dist\candidate-0.3.1-rc.4
```

This creates the runnable folder, `AirWindows-0.3.1-rc.4-win-x64.zip`, a matching source zip, and `SHA256SUMS.txt`. It also writes `AirWindows-0.3.1-rc.4-symbols.zip`, the debug symbols for that build. Keep it with the release: a crash report can only be read with the symbols of the exact build that wrote it. The symbols are not part of the download. Existing package outputs are never overwritten. The Windows ZIP includes the launcher, compiled `iosctl` helper, agent guides, optional Python reader, GPL and dependency notices. Building the receiver also builds the helper with the .NET Framework C# compiler. Both executable versions must match before packaging.

The source archive contains `app`, its sibling Bluetooth backend, UxPlay, and the upstream FFmpeg, OpenSSL and libplist source archives with the relevant ports. Those archives must be present in vcpkg's downloads directory. Publish the matching source and checksums with any binary. The separate AirDrop research is not bundled.

## Source map

| Source | Responsibility |
|---|---|
| `third_party/UxPlay/lib` | AirPlay pairing, FairPlay and streams; fetched by bootstrap |
| `src/mdnsd_win.c` | Discovery on active Windows interfaces |
| `src/airplay_server.cpp`, `src/media_sink.h` | Server callbacks and media interface |
| `src/video_pipeline.cpp`, `src/frame_server.cpp` | Decode/render and optional frame export |
| `src/audio_pipeline.cpp` | Decode, resample and WASAPI output |
| `src/main.cpp`, `src/settings.cpp` | Window, commands, input routing and settings |
| `src/ui.cpp`, `src/guide.cpp` | Colours, fonts and button drawing; the setup guide panel |
| `src/capture.cpp`, `src/recorder.cpp` | PNG screenshots; MP4 recording through Media Foundation |
| `src/app-icon.svg`, `src/app-icon-small.svg` | Icon drawings; `scripts/make-icon.py` renders them into `src/app.ico` (needs Inkscape) |
| `src/log.cpp`, `src/crash.cpp` | Log file and crash reports |
| `src/native_control.cpp`, `src/control_queue.h` | Phone identity, focus and action queue |
| `../native-ble-prototype` | Windows BLE HID backend |

`compat/msvc` supplies POSIX compatibility for UxPlay without editing its source. Its mutex shim uses zero-initializable SRW locks because UxPlay can lock a mutex without explicitly initializing it.

The application is GPL-3.0-or-later. See [LICENSE](LICENSE) and [THIRD-PARTY.md](THIRD-PARTY.md). AirPlay, iPhone, iPad and Mac are Apple trademarks; this project is not affiliated with Apple.
