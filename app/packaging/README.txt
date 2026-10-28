AirWindows
==========

Shows the screen of an iPhone, iPad or Mac on a Windows PC over AirPlay
screen mirroring.

graniteai.co/tools/airwindows

Getting started
---------------

1. Start AirWindows.exe. There is nothing to install, and the folder can be
   kept anywhere. It needs Windows 10 or later, 64-bit.
2. The first time, Windows Firewall asks whether to allow the program. Allow
   it on private networks, or the Apple device will not find the PC.
3. On the iPhone, iPad or Mac, which has to be on the same network, open
   Control Center, choose Screen Mirroring and pick the PC's name.

The executable is not code signed. If you downloaded it, Windows may show a
"Windows protected your PC" notice the first time. Choose "More info" and then
"Run anyway".

Controls
--------

Full screen                 F11, Alt+Enter or double-click. Esc leaves it.
Menu while in full screen   Right-click.
Change the AirPlay name     Receiver > Rename
Resolution and frame rate   Video > Resolution, Video > Frame rate
Hardware or software video  Video > Decoder
Remove black bars           View > Fit window to video
Frame rate and bitrate      View > Show status in title bar

If the PC does not appear on the device
---------------------------------------

Check that both are on the same network and that Windows Firewall allows
AirWindows on private networks. Some routers and most guest networks keep
devices from seeing each other, and that setting has to be off.

Settings and the log are in %LOCALAPPDATA%\AirWindows. Receiver > Open log
folder opens it. Starting the program with --debug writes much more detail to
the log, which helps when a device connects but nothing appears.

What it does not do
-------------------

It receives a mirrored screen. It does not play videos that an app sends as a
link, which is what the cast button inside YouTube and similar apps does. It
does not record.

This is an early release. Screen mirroring has been tested with an iPhone.
Sound has had less testing.

License
-------

AirWindows is free software under the GNU General Public License, version 3
or later, and comes with no warranty. See LICENSE.txt. Its source code comes
with the download.

It is built on UxPlay, FFmpeg, OpenSSL and libplist. THIRD-PARTY.txt lists
them and the licenses folder has their license texts.

AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc. This program is not
affiliated with or endorsed by Apple.
