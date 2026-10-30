AirWindows
==========

Mirror an iPhone on Windows and control it with the PC's mouse and keyboard.
Source and documentation: https://github.com/ClariSortAi/airwindows

Double-click Start-AirWindows.cmd. It starts the receiver with frame access
enabled for desktop agents. Manual use needs no Python or build tools.
Allow it through Windows Firewall on your private network. On the phone,
open Control Center > Screen Mirroring and choose the receiver's name.

For input, enable AssistiveTouch on the iPhone. Click Pair Bluetooth in
AirWindows, pair the PC from the phone's main Settings > Bluetooth screen,
then select the intended phone in AirWindows and click Connect.

Click inside the phone picture to control it. Click another Windows app
to return focus to the PC. Text input supports US ASCII. Ctrl+V accepts
up to 1,024 ASCII characters. Unicode and general shortcuts are unsupported.

For Home, open AssistiveTouch > Devices > your PC > Customize Additional
Buttons. Click Home in AirWindows, then assign the detected Button 2 to
Home on the phone. Right-click inside the phone picture invokes the same
assigned action. Right-click in the bars or chrome opens the PC menu.

Run only one receiver. For Claude Code or Codex, start with FOR-AGENTS.md
in this folder. The frame reader is included in app/scripts; its Python
dependency is listed in requirements-agent.txt. The agent also needs a
supported Windows desktop-input tool. No source checkout is required.

This is an experimental, unsigned build for Windows x64. Native control
requires BLE peripheral mode and was tested on Windows 11. Disconnect
other previously paired phones while using this preview; a second HID
subscriber can cause repeated reconnects. Paste, wheel input and other
adapter/iOS combinations still need testing.

Logs and settings: %LOCALAPPDATA%\AirWindows
For connection problems, start with --debug and inspect debug.txt.

AirWindows is GPL-3.0-or-later, without warranty. See LICENSE.txt,
THIRD-PARTY.txt and licenses/. Distribute the matching source archive and
SHA256SUMS.txt with this binary. AirPlay and iPhone are Apple trademarks;
this project is not affiliated with Apple.
