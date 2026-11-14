# Changelog

## 1.0.0 (2026-11-14)

- PhonePort 1.0.0 is sold through the Microsoft Store: a 7-day free trial, then a one-time purchase. The Store installs and updates it and takes the payment. Open it from the Start menu, or run `phoneport --frames`; `iosctl` is on the command path too. There is no ZIP download of 1.0.0.
- The Start menu also lists "PhonePort command line (iosctl)", because the Store does not take a package whose command-line helper has no entry of its own. Opened from there, iosctl says what it is, that it runs from a terminal as `iosctl --help`, and where the support page is, then waits for Enter. Run from a terminal, with arguments or with redirected input or output, it behaves as before.
- Help > PhonePort support opens the support page, graniteai.co/PhonePort/Support. The About box links the product page, graniteai.co/PhonePort.
- AirWindows is now called PhonePort. Only the name changes.
  - The window title, the menus (Help > About PhonePort, Help > Buy PhonePort…), the setup guide, status messages, notices, the crash dialog and the program's file details now say PhonePort. The program file is `PhonePort.exe`, its Start menu entry is "PhonePort" and its command is `phoneport --frames`. New screenshots and recordings go to `Pictures\PhonePort` and `Videos\PhonePort`, and their names start with "PhonePort". Crash reports are named `PhonePort-<version>-<date>-<time>.dmp`. In Screen Mirroring the iPhone still shows the PC's name. Only a PC without a computer name now shows "PhonePort" there.
  - The first start of the Store install copies `settings.ini` from `%LOCALAPPDATA%\AirWindows`, where ZIP builds keep it, so no phone needs pairing again. Screenshots and recordings made before the rename stay in `Pictures\AirWindows` and `Videos\AirWindows`; nothing is moved. The receiver keeps its window, single-instance and frame pipe names. A receiver from before the rename and a new one still hand over to each other, and scripts written for the frame pipe keep working. `iosctl`, `observe-phone.py` and `airwindows_frames.py` keep their names. `iosctl --list`, `iosctl --doctor` and the agent driver find `PhonePort.exe` as well as `AirWindowsNative.exe` and `AirWindows.exe`. The GitHub address and the downloads already published keep their names.
- Sound from a mirroring iPhone plays. Every release up to 0.3.1-rc.5 was silent. The AirPlay engine's UDP read failed on Windows builds made with MSVC (Winsock error 10014), so the receiver never read the phone's timing replies or its audio packets. The fix is in `compat/msvc/msvc_prelude.h`; the UxPlay sources stay unmodified.
- Control follows the iPhone that is mirroring. PhonePort remembers a Bluetooth phone for each AirPlay sender (`NativeControlPairs` in `settings.ini`), so each phone is chosen once. When a different iPhone starts mirroring, the receiver lets go of the selected phone and selects the one remembered for that mirror. It used to close Bluetooth and forget the saved phone, so every switch meant pairing and choosing again.
- A device that starts Screen Mirroring takes over from the one on screen. A second iPhone used to be refused until the first stopped mirroring.
- A phone that is connected but not selected no longer stops input to the selected phone. An iPhone reports a suspend state when it locks, and the receiver kept one value for all phones. A locked phone on the desk blocked every click on the other one. Each phone now has its own state.
- If the selected phone's Bluetooth drops, the service stays published and the phone is selected again when it returns. The receiver used to close and republish the service. That made every paired phone reconnect and, with two phones, the loss could repeat several times in a row.
- The phone button reads "Waiting for iPhone…" while the mirroring phone's Bluetooth has not connected. It used to say "Choose phone" and list whichever other phone was connected. A phone chosen for another mirror is marked "(a different iPhone)" in the list. When only such phones are listed, the button reads "Pair this iPhone".
- An input that is not sent says why in the status bar: it was canceled, it took too long, or the selected iPhone is asleep. The last one usually means the selected phone is not the one on screen.
- The log records each change of mirror sender, each phone released and each phone selected, and when a paired phone sleeps or wakes.
- The log says when sound starts arriving from the sender, then every ten seconds how much arrived, how loud it was and the volume the sender asked for. It could not tell a silent sender from lost sound before.
- When PhonePort is installed from the Microsoft Store, it asks the Store for its license. During the free trial, the status bar shows how many days are left, the setup guide says it is a trial, and Help > Buy PhonePort… opens the Store's own purchase dialog. The Store shows the price and takes the payment; PhonePort shows no price. If the Store says the license is no longer active, PhonePort does not start AirPlay or Bluetooth control and shows a panel with a "Buy in Microsoft Store" button. Buying there starts mirroring without a restart. If the first Store call fails, or no answer comes within 20 seconds, PhonePort runs as if it had been bought. A later call that fails changes nothing: a trial stays a trial and a locked copy stays locked. A copy run as administrator hides Buy, because the Store does not sell to an elevated app. Source builds, the ZIP download and any copy not installed from the Store make no Store call and look exactly as before.
- New self-test option `--selftest-license trial:<days>|expired|licensed` shows each licensing state without asking the Store. A new check, `LicenseCheck`, covers what each Store answer means, including the answers measured for a locally registered package and for an unpackaged copy. Both count as "not from the Store". It runs in CI. `build.ps1 -HomeUrl` sets the product page linked from About.
- A copy installed from the Microsoft Store keeps its settings, log and crash reports in its own data folder, `%LOCALAPPDATA%\Packages\<package name>\LocalState`. Windows hides a new `%LOCALAPPDATA%\AirWindows` folder made by a Store app from every other program, so Open log folder, the crash message and agent scripts would have pointed at a folder that did not exist for them. The first start of the Store copy copies `settings.ini` from `%LOCALAPPDATA%\AirWindows`, so paired phones and the receiver name carry over from the ZIP version. The original is only read. The ZIP and source builds keep using `%LOCALAPPDATA%\AirWindows`. `iosctl --doctor` now prints the receiver's `data_folder`, and `observe-phone.py --data-folder` does the same.
- Shortly after it starts, PhonePort checks the Bluetooth adapter. If there is none, or it cannot act as a keyboard and mouse (no Bluetooth LE peripheral role), the status bar and the setup guide say so at once and add that mirroring still works. Before, the user found out only after clicking Pair Bluetooth.
- PhonePort reads the Windows network type of each connected network that has a gateway, and reads Windows Firewall (read-only) to see whether it lets PhonePort in on that network. VPN, tunnel and other virtual adapters are left out. Both are read again while it runs. The Store copy's firewall rules cover private and domain networks only, and Windows 11 marks new Wi-Fi networks Public.
  - When the firewall blocks PhonePort on every Public network and no network lets it in, the idle screen and the first step of the setup guide say that the iPhone cannot find this PC there, and that step stays open.
  - When another network lets PhonePort in, or the firewall cannot be read, the step is done and a note says: "If your iPhone is on …, it may not find this PC".
  - Both come with an Open network settings button for the Wi-Fi or Ethernet page of Windows Settings, where the network can be set to Private. For the ZIP the note adds that allowing PhonePort through Windows Firewall on public networks also works.
  - A Public network that an allow rule already covers gets no notice. When the firewall lets PhonePort in on every network, the guide says so instead of asking the user to allow it.
- If the free trial ends while PhonePort is open and the user then buys it, Bluetooth control comes back the way it does at launch: the phone used last reconnects and is selected for its mirror. It used to stay disconnected until the phone was chosen again. A Disconnect now also drops a Pair or a reconnection that was still waiting, so a trial that ends never republishes the Bluetooth service. A request made after the Disconnect still runs.
- Store license handling is sturdier.
  - After the Store reports a purchase as completed or already owned, PhonePort stays unlocked for the rest of the run, even if a license read right after it still shows the old license. It reads the license again 2, 10 and 30 seconds later.
  - "Thank you for buying PhonePort. It is unlocked." appears only for a real Store answer that unlocks a trial or a locked copy.
  - The trial's day count is recounted every minute. Once the trial's end date passes on the PC's clock, the status bar says "Trial ends today" and PhonePort asks the Store. It locks only when the Store says the license is no longer active, and asks again every 10 minutes if the Store cannot be reached.
  - The purchase dialog now opens from the window's thread, as Microsoft's documentation requires, and PhonePort never waits on it. Closing PhonePort with the dialog open cancels it.
  - A trial that ends during mirroring no longer keeps the display awake.
- `iosctl` no longer assumes the ZIP. `--doctor` tells a Store user to start PhonePort from the Start menu or with `phoneport --frames`, and no longer mentions the Connect button removed in rc.5. When it cannot inspect the receiver process, it reports `data_folder_basis` as `receiver_unknown` and says in `data_folder_note` what it assumed. `observe-phone.py --data-folder` reports a missing `LOCALAPPDATA` as an error instead of a crash.
- New self-test options: `--selftest-bluetooth none|no-peripheral|ready|probe`, `--selftest-network public|private|mixed|public-allowed|unknown`, and `store` for `--selftest-license`, which asks the real Store, read-only, in a packaged run. With `--selftest-license trial:N`, command 41201 ends the trial while the window is open. With `--selftest-menu`, the open-folder commands and Open network settings write their target to the log instead of opening a window. The agent driver gained `-Packaged`, `-Clip`, `-NoClip` and `-KnownFolders`.

Settings from 0.3.1-rc.5 carry over: the saved phone and sender become the first remembered pair.

Tested on an iPhone15,2 and an iPhone16,2 paired at once, both named "iPhone": both reconnected within a second of launch and the phone used last was selected; the idle phone slept without stopping input to the other; a mirror from the unselected phone released the selected one in 0.1 s without closing Bluetooth, and after one choice the user controlled it with the mouse; a later mirror from that phone needed no choice; four drops of the selected phone's Bluetooth session each healed in about a second with the other phone still connected. Sound from the iPhone15,2 played through the PC's speakers: the log counted 909 audio packets in ten seconds and Windows' meter showed the receiver's output. Untested on hardware: a mirror from a remembered phone while a different phone is selected, and a second phone taking over a running mirror. The policy checks cover the first of these.

Store licensing was tested on Windows 11 without a phone, in self-test runs. The trial showed "Trial: 5 days left" in the status bar, the trial line in the setup guide and Help > Buy PhonePort…. A simulated purchase removed the trial line and the Buy item and thanked the buyer. The expired state showed the panel, played no clip and kept the phone button grey; a simulated purchase from the panel started the clip without a restart. The panel scaled correctly on a 125% display. The licensed state showed nothing new, and a run without the option had the same 28 menu items as before. Untested: the real Store license and purchase dialog, which need the app published in Partner Center (a private audience is enough) and installed from the Store once; Buy hidden in an elevated run; and the dialog Windows itself shows when an expired trial is launched.

The Store install changes were tested on Windows 11 without a phone on 2026-11-13. A locally registered Store package of a RelWithDebInfo build, started through its execution alias, had package identity, played the test clip, wrote `debug.txt` and `crashes` to the package's `LocalState` folder and copied `settings.ini` on its first start. Nothing appeared in the redirected location. Open log folder named the `LocalState` folder. The unpackaged Python frame reader read its frames, the packaged `iosctl --doctor` reported the folder, and a screenshot and a 3-second recording landed in the real Pictures and Videos folders. Asked for its real license, the registered package got no Store SKU and showed no trial line and no Buy item. With the saved phones in its settings, the packaged receiver published the HID service, its advertisement reached Started, two phones subscribed and it reconnected the phone used last within a second. In the same run a simulated trial ran out, which disconnected Bluetooth and stopped the clip, and a simulated purchase reconnected the same phone a second later without a choice. The real adapter check reported the peripheral role inside and outside the package. The network check found the Wi-Fi Public and the wired network Private, and left out the WSL virtual adapter. The no-adapter, no-peripheral-role, Public and Private states, the unlock panel and the guide were checked at 100% and 125% display scaling. An H.265 test clip (1170 × 2532, 30 fps) decoded on the GPU and in software. Untested: H.265 from a real iPhone; the network notice on a PC whose iPhone really cannot connect, and the Open network settings button itself (checked only through the log, so no Settings window opened); the adapter notices on a PC that really lacks an adapter or the peripheral role; a real Store install, trial and purchase.

The follow-up fixes were tested the same day without a phone. LicenseCheck covers each new rule: a failed read after a trial, after a locked state, after a purchase and with no answer yet; a purchase followed by a stale "not active" read; the day count as the clock moves on; a trial end on the PC's clock with and without the Store's answer. In self-test runs, a trial that ran out while open showed the panel and a simulated purchase brought the clip back. On this PC the firewall check found the owner's allow rule for public networks, so the Public Wi-Fi got no notice. A registered package matched its own manifest rules by path: its Private network was allowed and its Public Wi-Fi drew the conditional note. Each simulated network case was checked on screen. Untested: the real purchase dialog on the window's thread, the 2, 10 and 30 second re-reads and the minute tick against a real Store, which all need a Store install.

Tested on 2026-11-14 with an iPhone 14 Pro (iPhone15,2) on a Private network. A locally registered Store package built from the same code (version 0.3.1) mirrored the phone and Bluetooth control came up ready. Taps from the PC drove the phone's Calculator, typing into the phone's Search worked, and a recording started and stopped. The iOS Calculator did not take digits typed on the PC's keyboard; tapping its keys worked.

The 1.0.0 Release build passed LicenseCheck, FrameServerCheck, MdnsRefreshCheck, the policy checks and the iosctl self-test. In a self-test run it logged "PhonePort 1.0.0", showed a simulated 7-day trial in the status bar and listed Help > PhonePort support (29 menu items, 30 during the trial); that item logged the support address instead of opening it.

Untested until PhonePort is installed from the Store: the Store's real license answer and trial count, the purchase dialog and a real purchase, the dialog Windows shows when an expired trial is launched, Buy hidden in a copy run as administrator, and a start with no network, where the first license call fails and PhonePort runs as if bought.

Known issues:

- The iOS Calculator ignores digits typed on the PC's keyboard. Tap its keys instead.
- With `--debug`, the receiver stopped with "invalid parameter passed to a C runtime function" a quarter of a second after the sound stream opened. That was before the sound fix, while every timing request was timing out and being logged. It has not been tried again since.

## 0.3.1-rc.5 (2026-11-10)

- New window. The menu bar, grey buttons and phone list are gone. One dark row holds a phone button, Home, Screenshot, Record, Full screen and Menu, and a status bar shows what Bluetooth control is doing. The title bar matches, and full screen hides everything but the picture.
- The phone button replaces Pair Bluetooth, the phone list, Connect and Disconnect. It reads Pair Bluetooth until a phone is paired. After that it shows the phone's name with a dot (amber while waiting, green when connected) and opens a menu to choose a phone, pair again or disconnect. Choosing a phone connects it, so there is no separate Connect step. The hexadecimal id beside a phone's name now appears only when two phones share a name.
- Screenshot (F8) saves the phone's picture at its own size as a PNG in `Pictures\AirWindows` and copies it to the clipboard.
- Record (F9) saves an MP4 with the phone's sound in `Videos\AirWindows`. The button turns red and the status bar shows a clock while it runs. It uses the H.264 and AAC encoders that come with Windows. A recording has a constant frame rate, the one set under Video > Frame rate (60 unless changed). A phone sends a picture only when its screen changes, so the recorder repeats the last picture until the next one arrives, and every frame in the file is the same length. Pictures that Wi-Fi delivers in a bunch after a pause each keep a frame of their own. A new file starts if the phone rotates, and a recording in progress is finished when mirroring stops or AirWindows closes.
- A setup guide opens beside the picture on a first launch. It goes from joining the network to mirroring, pairing, choosing a phone and taking control, and ticks each step off when the receiver sees it happen. Help > Setup guide brings it back.
- The menu opens from the Menu button, the Alt key or a right-click outside the picture. Capture is a new submenu. "Show status in title bar" is now "Stream details in status bar", and "How to connect" gave way to the setup guide. Settings that restart the service now say on the idle screen that mirroring stopped because of them.
- New icon, drawn as SVG (`src/app-icon.svg`) with a simpler drawing for 16 to 32 pixels.
- The status bar and the phone button's dot now follow what is on screen. They used to keep saying "mirror the selected iPhone" while it was mirroring, and stayed green after mirroring stopped. An old complaint such as "Paste rejected" clears when you click the phone again. The status sentence is no longer cut short by the stream details beside it: in a narrow window the details lose parts from the right instead.
- Log lines no longer break on a receiver, phone or folder name outside ASCII.
- AirWindows follows network changes while it runs. Discovery used to join only the networks that were up at launch. After Wi-Fi reconnected or the PC's address changed, the receiver dropped out of the iPhone's Screen Mirroring list until it was restarted, and a receiver started before the network was up never appeared at all. It now joins a network within a few seconds of it appearing and can start with none.
- Sound follows Windows' default playback device. Plugging in headphones used to leave the phone's sound on the speakers until mirroring restarted.
- A graphics device reset no longer leaves a black picture. After a driver update or GPU reset, the session ends, the receiver rebuilds its video path on the new device and the idle screen says what happened. Start Screen Mirroring again to continue.
- Starting AirWindows twice no longer runs two receivers with the same name and the same Bluetooth service. The second launch brings the running window forward and exits. If it asked for `--frames` and the running receiver did not have it, frame access turns on there.
- If AirWindows stops unexpectedly, it saves a crash report in `%LOCALAPPDATA%\AirWindows\crashes` and shows where it is. The window used to just close. The five newest reports are kept and nothing is uploaded. Release builds keep their debug symbols, and `package.ps1 -Release` archives them beside the release so a report can be read.
- A full log is kept as `debug.old.txt` when a new one starts. It used to be emptied, which lost the events that led up to a problem.
- The window reopens at its last position and size. The toolbar uses the system font at the display's scale and sits on its own strip.
- The status line names the Bluetooth problems that stop pairing: no adapter, an adapter that cannot act as a keyboard and mouse, or Bluetooth that could not start.
- New self-test options `--selftest-device-loss`, `--selftest-crash`, `--selftest-audio-switch`, `--selftest-screenshot`, `--selftest-record` and `--selftest-menu`, and `--capture-dir` to choose where captures go. A new check, `MdnsRefreshCheck`, runs the discovery code against a scripted adapter and is part of CI. `RecorderCheck` tests the recorder alone, including that every frame sits in its place at the file's frame rate, and is run locally.

Tested on Windows 11 without a phone, on a 1170 by 2532 test clip: every menu command was fired and its effect, check mark and grey state checked (the Always on top check needed the window opened normally, not in the background); a screenshot came out at 1170 by 2532; a three-second recording of the 30 fps clip held 181 frames of H.264, which ffprobe reported as 60 a second with every gap between frames the same, and three seconds of AAC; an earlier four-second recording had colours within 4 levels in 255 of the screenshot; F8, F9 and F11 worked with the keyboard on the picture; the guide opened and closed and the window grew and shrank by its width. Only 100% display scaling was tried.

Also tested without a phone: the discovery check passed, and failed when each part of the fix was taken out; a device loss simulated during a self-test clip rebuilt the video path and the picture came back; both deliberate failures saved a report, and the dialog appeared and closed; a second launch exited in under a second and turned frame access on in the first; position and maximized state survived a restart, and a position off every display fell back to the default; a 5.8 MB log was kept in full as `debug.old.txt`.

Tested on one iPhone15,2 with the new window: Home and `page left` landed; a screenshot came out at the phone's 498 by 1080; a six-second recording held 303 frames and showed the Home Screen, the Today View and the Home Screen again; Disconnect, Pair Bluetooth and choosing the phone from the phone button brought control back in about a second; real clicks on Screenshot and Record left the keyboard on the phone; restarting the service from the menu ended the session and the idle screen said why, and when Screen Mirroring was started again the picture and control came back and a 20-second recording held 805 frames. Both of those recordings were made before recordings got a constant frame rate. A later session on the same phone confirmed two changes made since: while it mirrored, the status bar read "Click the mirrored phone to control it" and both dots were green; and a 12-second recording held 720 frames, which ffprobe reported as 60 a second with every gap between frames the same.

Tested on the same phone before the redesign: the receiver was started with only Ethernet up. The PC's Wi-Fi connected a minute later, the running receiver answered discovery on the new address, and the phone then mirrored to it. Home and `page left` worked. A device loss simulated during the session ended it and the idle screen showed the reason.

Untested: opening a recording in a video editor; the sound codec's name in the stream details, which needs a phone that is playing sound (the stray character in front of it was reproduced and fixed outside the program); the wider allowance for pictures that arrive together, on a phone (the 12-second phone recording left out 17 such pictures; the allowance was then widened and checked against that phone's recorded arrival times and in `RecorderCheck`); recording a phone's sound (the phone opened no sound stream in any session, so every recording made from it was silent); pairing a phone for the first time through the guide (the test phone was already paired); display scaling above 100%; Windows 10 and Windows N editions; the phone mirroring again after a device loss rebuilt the video path (it was tried after a plain service restart); a real change of default playback device (the move was exercised by reporting one from inside the program); the new Bluetooth adapter messages; and the discovery check on the CI runner.

Known issues:

- `iosctl page` can open what is under its start point instead of paging. On the Today View, `page right` opened the Screen Time widget's settings page. Seen once, on an iPhone15,2.
- A real graphics driver reset has not been tried. Recovery was exercised by reporting the device lost from inside the program.

## 0.3.1-rc.4 (2026-11-03)

- Pairing no longer races a two-minute timer. The input service now stays up while a phone is pairing or subscribed. Before, a pairing could finish just after the service closed; iOS then saved a pairing that never subscribed, and only forgetting and re-pairing fixed it. That is the cause of rc.3's "control drops and only returns after re-pairing" issue in the case we traced.
- Taps after a long pointer move land as taps. iOS glides its pointer to a new position, and AirWindows now waits for it to arrive before pressing or releasing.
- Drags reach the phone as a smooth path. Flicks scroll, a swipe in from the left edge goes back, and a swipe up from the bottom edge goes Home.
- The black bars beside the phone act like its bezel. A press within 16 px of the picture starts at the screen edge, so edge swipes can begin in the bar. A click further out does nothing and no longer turns control off.
- Resizing the window or entering and leaving full screen no longer turns control off, so typing afterwards still reaches the phone. Pending input is still dropped, because its coordinates no longer apply.
- Fast scrolling and holding a key no longer stop control with "queued action expired". Wheel steps and key repeats that arrive while Bluetooth is busy are dropped instead of queued.
- Esc leaves full screen after you click the phone. A one-run `--decoder` choice is no longer saved to settings. The log rotates while running instead of only at startup.
- While pairing, the log records each Bluetooth step: subscriptions, reads, writes and errors.
- `iosctl`: flicks use a 40 ms initial hold and `page` keeps 300 ms. `wake` no longer gets refused as busy right after its own moves, and it says when phone control is off instead of blaming the mirror.

Tested on one iPhone15,2 with Windows 11 and a MediaTek adapter: a fresh pairing subscribed within a second; 4 of 4 taps right after `wake` landed (all had failed before); 6 of 6 flicks with the new hold scrolled without opening rows; left-edge back and bottom-edge Home worked from the picture edge and from the bar; a far click in the bar left control on; an 8-step wheel burst and a 40-repeat held Backspace kept control on; Esc left full screen after a click; control stayed on through F11, Esc and a window resize; Home Screen paging worked both ways.

Untested: paste, two phones at once, and any phone model other than iPhone15,2 and the rc.3 iPhone16,2.

Known issues:

- One cause of rc.3's "forget and re-pair" Bluetooth drop is fixed. Other causes have not been ruled out.
- The wrong paired phone can bind and still report ready. Check which phone is selected.
- A person has to click once to focus the window before controlling the phone. That click also reaches the phone.
- With no text field focused, typing on the phone does not type: Space and Enter activate the focused control. Check for a cursor first.

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
