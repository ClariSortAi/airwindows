# AirWindows

AirWindows is an AirPlay screen mirroring receiver for Windows. It shows the screen of an iPhone, iPad or Mac in a window on a Windows PC. It is a single executable with nothing to install.

The AirPlay protocol itself is handled by [UxPlay](https://github.com/FDH2/UxPlay), compiled here with MSVC. AirWindows replaces UxPlay's mDNS responder with a Windows implementation that advertises on active physical Ethernet and Wi-Fi interfaces. The native Windows program also provides FFmpeg decoding with Direct3D 11 hardware acceleration, a Direct3D renderer, and WASAPI audio output.

## Status

This is an early release, version 0.2.0.

Version 0.1.0 mirrored an iPhone (AirPlay/960.13.1) with H.264 video decoded on the GPU. Version 0.2.0 changed how FFmpeg is built and linked. It passes the automated checks listed under [Testing](#testing) but has not yet been run against a device.

Not yet tested with a real device in any version: sound, H.265 video, and recovery after a dropped connection.

## Download and run

Get `AirWindows-<version>-win-x64.zip` from [graniteai.co/tools/airwindows](https://www.graniteai.co/tools/airwindows), unzip it anywhere, and start `AirWindows.exe`. It needs 64-bit Windows 10 or later and nothing else.

The first time it runs, Windows Firewall asks whether to allow it. Allow it on private networks, or Apple devices will not find the PC.

On the iPhone, iPad or Mac, which has to be on the same network, open Control Center, choose Screen Mirroring, and pick the PC's name. The window switches from the instructions to the mirrored picture when video arrives.

Discovery joins each active physical Ethernet and Wi-Fi interface at startup and advertises that interface's address on its network. Restart AirWindows after changing network connections. Windows Firewall must permit the app on the network profile in use; Wi-Fi and Ethernet can have different profiles.

The executable is not code signed, so Windows may show a "Windows protected your PC" notice for a downloaded copy. Choose "More info" and then "Run anyway".

## Controls

| Action | How |
|---|---|
| Full screen | F11, Alt+Enter or double-click. Esc leaves it |
| Menu while in full screen | Right-click |
| Change the AirPlay name | Receiver > Rename |
| Resolution and frame rate offered to the sender | Video > Resolution, Video > Frame rate |
| Hardware or software decoding | Video > Decoder |
| Remove black bars in a window | View > Fit window to video |
| Frame rate, bitrate and codec readout | View > Show status in title bar |

Settings are stored in `%LOCALAPPDATA%\AirWindows\settings.ini` and the log is `%LOCALAPPDATA%\AirWindows\debug.txt`. Receiver > Open log folder opens that folder.

These command-line options exist, mostly for troubleshooting and testing:

| Option | Effect |
|---|---|
| `--debug` | Adds the protocol engine's detailed messages to the log. Try this first when a device connects but nothing appears |
| `--decoder auto\|hardware\|software` | Overrides the decoder setting for one run |
| `--selftest-video <file>` | Plays an H.264 or H.265 Annex-B file through the same path AirPlay video takes |
| `--selftest-audio` | Opens the AAC-ELD, AAC-LC and ALAC decoders with AirPlay's parameters and logs the result |
| `--selftest-tone` | Plays a one second tone through the audio output |
| `--exit-after <seconds>` | Closes the program after that many seconds |

## What it does not do

It receives a mirrored screen. These are not implemented:

- Video casting, where an app such as YouTube sends the address of a video instead of mirroring the screen
- Recording
- A now-playing display for audio-only streams
- Languages other than English

H.265 is offered to the sender only when the PC's GPU can decode it. Otherwise video is H.264.

Video is drawn as soon as it is decoded, and audio passes through a buffer of about 60 to 80 ms, so sound can trail the picture slightly.

## Building from source

You need 64-bit Windows, Visual Studio Build Tools 2019 or later with the C++ workload, git, and about 3 GB of free disk space.

```powershell
.\scripts\bootstrap.ps1          # fetches UxPlay and vcpkg at pinned commits
.\scripts\build.ps1              # output: build\Release\bin\AirWindows.exe
.\scripts\package.ps1            # copies the program and license texts to dist\AirWindows
.\scripts\package.ps1 -Release   # also makes the release zip, the source zip and checksums
```

The first build compiles OpenSSL, libplist and FFmpeg from source through vcpkg, which takes a while. Later builds reuse them. Nothing is installed outside the project folder.

The library versions are fixed by the vcpkg commit in `scripts\bootstrap.ps1`, and `vcpkg.json` lists what is used. FFmpeg is built by a modified copy of vcpkg's port, in `vcpkg-ports\ffmpeg`, that enables only the four decoders the program opens. The README in that folder describes the change.

The build scripts use the Ninja generator from a `vcvars64.bat` environment. If you already have vcpkg and prefer your own CMake setup, point `CMAKE_TOOLCHAIN_FILE` at it and use the `x64-windows-static` triplet.

## Testing

Native iOS AirDrop reception is a separate, unfinished investigation using the
built-in MT7922. See [the current status and evidence](../airdrop-native/README.md).
The AirWindows application does not currently receive AirDrop files.

`tests\protocol_check.py` plays the part of an AirPlay sender up to the point where a real device's FairPlay keys would be needed. It checks the mDNS announcements, `GET /info`, `/pair-setup`, both steps of `/pair-verify` including the receiver's Ed25519 signature, and the first phase of `/fp-setup`. It needs Python 3 with the `cryptography` package.

```powershell
# start the program, read the port from the log, then
python .\tests\protocol_check.py --port <port> --name <AirPlay name>
python .\tests\mdns_interfaces_check.py --port <port> <local-ip> [<local-ip> ...]
```

The second check verifies that each specified local interface receives its own address in the reply. Neither local check proves discovery or mirroring from a separate device.

The video path can be exercised without a device. Any H.264 or H.265 Annex-B file works, for example one made with the `ffmpeg` command-line tool:

```powershell
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 5 -pix_fmt yuv420p -c:v libx264 clip.h264
.\build\Release\bin\AirWindows.exe --selftest-video clip.h264 --exit-after 10
```

The log then reports how many frames were decoded and whether the GPU or the software decoder was used.

## How it is put together

| Part | Where | What it does |
|---|---|---|
| AirPlay protocol and discovery | `third_party\UxPlay\lib`, fetched by the bootstrap script | Pairing, FairPlay, the mirror and audio streams, and the mDNS responder |
| Server wrapper | `src\airplay_server.cpp` | Starts UxPlay's server and turns its callbacks into the `IMediaSink` interface in `src\media_sink.h` |
| Video | `src\video_pipeline.cpp` | FFmpeg decode on a D3D11VA device or in software, then an NV12 to RGB pixel shader on a flip-model swap chain |
| Audio | `src\audio_pipeline.cpp` | FFmpeg decode, resampling to 44.1 kHz float stereo, a small jitter buffer, WASAPI shared-mode output |
| Window and settings | `src\main.cpp`, `src\settings.cpp` | Win32 window, menu, idle screen, full screen |

UxPlay targets POSIX compilers. It builds with MSVC here through a small compatibility layer in `compat\msvc`, without edits to its source. One detail in that layer matters: UxPlay locks a mutex it never initializes. That works with pthreads but crashes with a Windows critical section, so the layer implements mutexes as SRW locks, which are valid when zeroed.

`src\mdnsd_win.c` replaces UxPlay's mDNS responder at build time. It uses Windows packet information to return the address of the interface on which a discovery query arrived.

## License

AirWindows is free software under the GNU General Public License, version 3 or later. See `LICENSE`. It comes with no warranty.

It links UxPlay (GPL-3.0), FFmpeg (LGPL-2.1-or-later), OpenSSL (Apache-2.0) and libplist (LGPL-2.1-or-later). `THIRD-PARTY.md` has the versions and details. Each release comes with a source archive that contains this project, UxPlay, and the upstream sources of the three libraries.

AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc. This project is not affiliated with or endorsed by Apple.
