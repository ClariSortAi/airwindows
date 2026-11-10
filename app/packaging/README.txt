AirWindows
==========

Mirror an iPhone on Windows and control it with the PC's mouse and keyboard.
Source and documentation: https://github.com/ClariSortAi/airwindows

Double-click Start-AirWindows.cmd. It starts the receiver with frame access
enabled for desktop agents. Manual use needs no Python or build tools.
Allow it through Windows Firewall on your private network. On the phone,
open Control Center > Screen Mirroring and choose the receiver's name.

A first launch opens a setup guide beside the picture. It walks through
the steps below and ticks each one off as it happens. Reopen it any time
from Menu > Help > Setup guide.

For input, enable AssistiveTouch on the iPhone. Click the phone button at
the left of the button row (it reads Pair Bluetooth), pair the PC from the
phone's main Settings > Bluetooth screen, then click the phone button
again and choose your phone. Its dot turns green when it is connected.

Click inside the phone picture to control it. Click another Windows app
to return focus to the PC. Text input supports US ASCII. Ctrl+V accepts
up to 1,024 ASCII characters. Unicode and general shortcuts are unsupported.

For Home, open AssistiveTouch > Devices > your PC > Customize Additional
Buttons. Click Home in AirWindows, then assign the detected Button 2 to
Home on the phone. Right-click inside the phone picture invokes the same
assigned action. Right-click outside the picture, the Menu button or the
Alt key opens the AirWindows menu.

Screenshot (F8) saves the phone's picture as a PNG in Pictures\AirWindows
and copies it to the clipboard. Record (F9) saves an MP4 with the phone's
sound in Videos\AirWindows; press it again to stop. F11 is full screen.

Starting AirWindows again brings the running window forward; it does not
open a second receiver. For Claude Code or Codex, start with FOR-AGENTS.md.
Run iosctl.exe --doctor for a read-only diagnosis and the next setup step.
Use iosctl.exe --pid PID "snapshot idle.png" to inspect an idle picture;
its reported age matters. Normal capture and input still require fresh frames.
The compiled iosctl.exe helper supplies capture, taps, typing, drags, Home
and preflight. Read app/tools/iosctl/README.md for commands and use input
only where the agent's environment permits it. Python is optional; the
older Python reader remains in app/scripts. No source checkout is required.

This is an unsigned release candidate for Windows x64. Native control
requires BLE peripheral mode and has been tested on Windows 11. The helper
uses .NET Framework 4.8. The revised helper, two-phone behavior, paste,
wheel input and other adapter/iOS combinations still need live acceptance.

Logs and settings: %LOCALAPPDATA%\AirWindows
For connection problems, start with --debug and inspect debug.txt.
If AirWindows stops unexpectedly, it saves a crash report in the crashes
folder there and shows where it is. Nothing is uploaded.

AirWindows is GPL-3.0-or-later, without warranty. See LICENSE.txt,
THIRD-PARTY.txt and licenses/. Distribute the matching source archive and
SHA256SUMS.txt with this binary. AirPlay and iPhone are Apple trademarks;
this project is not affiliated with Apple.
