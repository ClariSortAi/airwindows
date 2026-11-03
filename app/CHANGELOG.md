# Changelog

## Unreleased

- Pairing no longer races a two-minute timer. The input service now stays up while a phone is pairing or subscribed. Before, a pairing could finish just after the service closed; iOS then saved a pairing that never subscribed, and only forgetting and re-pairing fixed it. That is the cause of rc.3's "control drops and only returns after re-pairing" issue in the case we traced.
- Taps after a long pointer move land as taps. iOS glides its pointer to a new position, and AirWindows now waits for it to arrive before pressing or releasing.
- Drags reach the phone as a smooth path. Flicks scroll, a swipe in from the left edge goes back, and a swipe up from the bottom edge goes Home.
- The black bars beside the phone act like its bezel. A press within 16 px of the picture starts at the screen edge, so edge swipes can begin in the bar. A click further out does nothing and no longer turns control off.
- Fast scrolling and holding a key no longer stop control with "queued action expired". Wheel steps and key repeats that arrive while Bluetooth is busy are dropped instead of queued.
- Esc leaves full screen after you click the phone. A one-run `--decoder` choice is no longer saved to settings. The log rotates while running instead of only at startup.
- While pairing, the log records each Bluetooth step: subscriptions, reads, writes and errors.
- `iosctl`: flicks use a 40 ms initial hold and `page` keeps 300 ms. `wake` no longer gets refused as busy right after its own moves, and it says when phone control is off instead of blaming the mirror.

Tested on one iPhone15,2 with Windows 11 and a MediaTek adapter: a fresh pairing subscribed within a second; 4 of 4 taps right after `wake` landed (all had failed before); 6 of 6 flicks with the new hold scrolled without opening rows; left-edge back and bottom-edge Home worked from the picture edge and from the bar; a far click in the bar left control on; an 8-step wheel burst and a 40-repeat held Backspace kept control on; Esc left full screen after a click; Home Screen paging worked both ways.

Untested: paste, two phones at once, and any phone model other than iPhone15,2 and the rc.3 iPhone16,2.

## 0.3.1-rc.3 (2026-11-02)

- Separate toolbar IDs from menu IDs. In rc.2, using the phone selector could restart AirPlay and drop the mirror, and Rename, Open Log Folder and Exit ran Pair, Connect and Disconnect instead.
- Hold the phone pointer at its new position for 150 ms before a press, up from 60 ms. One small send button was missed at 60 ms.
- Add `iosctl wake`. A still phone screen sends no frames, so the helper refused input as stale. `wake` goes first in a batch, moves the pointer without pressing, and stops before any press if no new frame arrives.

Tested on one iPhone16,2: 14 of 14 rapid helper clicks landed, and one live run navigated, typed and sent a message with the send landing on the first click. Toolbar Pair and Connect were used repeatedly. The helper self-test covers `wake` parsing and sends no desktop or Bluetooth input.

Untested: a real mouse click on the selector while mirroring, the corrected Rename, Log and Exit menu items, clicks after a long idle (one sample only), and any phone model other than iPhone16,2.

Known issues:

- Bluetooth control can drop and may only return after you forget and re-pair the PC on the iPhone. The cause is not established.
- The wrong paired phone can bind and still report ready. Check which phone is selected.
- A person has to click once to focus the window before controlling the phone. That click also reaches the phone.

## 0.3.1-rc.2 (2026-10-30)

- Accumulate fine-resolution wheel/trackpad events into bounded swipes instead of scrolling once per packet. Clear partial gestures on focus/picture changes and intervening clicks or text.
- Add standalone `iosctl snapshot` for idle-image inspection, with original timestamps, age and an inspection-only label. Pause/disconnect still clear retained images; normal capture, input and verification keep the one-second limit.
- Add read-only `iosctl --doctor [--pid PID]` for receiver selection and actionable setup diagnosis, including before the first frame. Upgrade receiver and helper together for these commands.
- Expand regression checks for both frame clients, real pipe-to-PNG snapshots and wheel normalization. Update README, agent guides and packaged instructions.

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
