# Changelog

## 0.3.1-rc.1 (2026-10-30)

- Integrates Claude Code's `iosctl` workflow: taps, text, Home, paced drags, paging, screenshot/crop/grid inspection, target maps and preflight.
- Uses explicit receiver selection and actual frame dimensions. Captures come from the decoded phone pipe, without desktop occlusion or a Python dependency.
- Adds focus/input-result checks, bounded parsing, button/key cleanup, receiver-specific input serialization and nonzero failures. Removes automatic click/page retries.
- Exports current control readiness, identity epoch, focus and queue activity with frames so preflight and input completion do not rely on toolbar heuristics.
- Keeps the selected phone when extra HID subscribers appear, preserves explicit selection, and restricts control writes to the bound peer.
- Packages the compiled helper, portable demo and example map with matching source. Receiver/helper versions are checked together.

Automated checks cover the helper, subscriber-selection policies and frame metadata. The revised helper and two-phone behavior still need live-device acceptance. Personal maps, recipients and raw phone evidence are excluded.

## 0.3.0-preview.1 (2026-10-30)

- Native Windows BLE keyboard and absolute mouse control inside the AirPlay picture.
- Focus returns to Windows when clicking another application; input cancels on focus, identity or stream changes.
- Button 2 forwarding through right-click and a Home toolbar button, with one-time AssistiveTouch mapping.
- Bounded ASCII typing and explicit paste, list dragging and wheel-to-swipe routing.
- Local decoded-frame reader, desktop-agent instructions, policy checks and Windows CI.
- Release archives include the sibling Bluetooth backend and its MIT descriptor notice.
- Windows ZIP includes a launcher, agent setup guide and Python frame reader; source is optional for normal use.

GUI clicking, typing and focus return were accepted on a real iPhone. Claude Code's later session exercised taps, list dragging, Home and Camera workflows, with intermittent input failures. Paste and wheel remain unqualified. A second previously paired HID subscriber can cause repeated reconnects; disconnect that device while using this preview.

## 0.2.0 (not yet released)

The first version prepared for publication.

- The 2026-10-30 source build adds optional `--frames` decoded-image access through a local user-only named pipe, with packet/decode timestamps and lifecycle invalidation. Real H.264 iPhone capture and a supervised ESP32 BLE click were observed. This does not add direct input to the receiver window or update older packaged downloads.
- Self-test runs no longer advertise an AirPlay receiver.

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
