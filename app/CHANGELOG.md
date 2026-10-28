# Changelog

## 0.2.0 (not yet released)

The first version prepared for publication.

- The program is now named AirWindows. Version 0.1.0 was called AirPlay Receiver. The executable is `AirWindows.exe`, and settings and the log are in `%LOCALAPPDATA%\AirWindows`. Settings saved under the earlier name are not carried over.
- FFmpeg 9.0.2 is built from source and linked into the executable. Only the H.264, H.265, AAC and ALAC decoders and the D3D11 hardware decoding path are compiled in. The program is now one file with no DLLs beside it.
- The libraries are declared in `vcpkg.json` and built by vcpkg during the first build. `scripts\bootstrap.ps1` only fetches UxPlay and vcpkg.
- The program has its own icon.
- Control Flow Guard is enabled for the code compiled in this project.
- OpenSSL no longer reads a system-wide `openssl.cnf`.
- The version number is set once, in `CMakeLists.txt`, and appears in the file properties, the About box and the log.
- The About box and the idle screen have new wording, and the About box carries the license and trademark notices.
- `scripts\package.ps1 -Release` makes the release zip, a source archive with the upstream library sources, and checksums.

## 0.1.0 (2026-10-12)

The first working build. Not published.

- AirPlay screen mirroring through UxPlay's library, compiled with MSVC.
- H.264 and H.265 decoding on the GPU or in software, drawn with Direct3D 11.
- AAC-ELD, AAC-LC and ALAC decoding with WASAPI output.
- FFmpeg loaded from three separate DLLs.
