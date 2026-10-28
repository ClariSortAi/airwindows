# Third-party components

AirWindows as a whole is distributed under the GNU General Public License, version 3 or later (see `LICENSE`), because it links UxPlay.

Everything below is compiled from source and linked statically into `AirWindows.exe`.

| Component | Version used | License | How it is used |
|---|---|---|---|
| [UxPlay](https://github.com/FDH2/UxPlay) | commit `ec203121355220dd90278a369eeabb0b8ba4f14a` | GPL-3.0. Most library files carry LGPL-2.1-or-later headers, and the bundled playfair code is GPL-3.0 | `lib/`, `lib/mdnsd`, `lib/llhttp` and `lib/playfair`, unmodified |
| [llhttp](https://github.com/nodejs/llhttp) | as bundled in UxPlay | MIT | HTTP and RTSP parsing inside UxPlay |
| [FFmpeg](https://ffmpeg.org) | 9.0.2 | LGPL-2.1-or-later | `libavcodec`, `libavutil` and `libswresample`, built by the port in `vcpkg-ports/ffmpeg` |
| [OpenSSL](https://www.openssl.org) | 3.6.5 | Apache-2.0 | `libcrypto` |
| [libplist](https://github.com/libimobiledevice/libplist) | 2.8.0 | LGPL-2.1-or-later | Property list parsing |

FFmpeg is configured with only the `h264`, `hevc`, `aac` and `alac` decoders and the D3D11VA hardware accelerators for H.264 and H.265. No GPL or nonfree FFmpeg components are enabled.

OpenSSL, libplist and FFmpeg are built through [vcpkg](https://github.com/microsoft/vcpkg) at commit `e456309491fd875487bf02b8a0dca76c1b1b00cc`. The files in `vcpkg-ports/ffmpeg` are a modified copy of vcpkg's FFmpeg port and are MIT licensed (`vcpkg-ports/LICENSE-vcpkg.txt`).

The LGPL asks that a user be able to relink the program against a changed copy of the library. This repository contains application source and build scripts that fetch the pinned dependencies. No binary release or bundled upstream source archive accompanies this private source publication. Any future binary release must supply the corresponding source and satisfy the applicable component licenses.
