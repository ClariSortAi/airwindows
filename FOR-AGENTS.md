# For agents: zero to ready iPhone control

Set up PhonePort (formerly AirWindows), pair the user's iPhone, then prove that you can see, click, type, scroll and go Home. Work through these steps in order. Handle PC setup yourself and ask the user for one phone action at a time.

AirPlay supplies the picture; the PC's Bluetooth adapter sends mouse and keyboard input. No iPhone app, ESP32 or phone-to-PC cable is required.

PhonePort 1.0.0 and later come from the Microsoft Store: a 7-day free trial, then a one-time purchase. A PC that already runs an older ZIP build (0.3.1-rc.5 or earlier) is covered in [Older builds](#older-builds).

## 1. Check the essentials

You need:

- Windows 11 x64 and a Bluetooth adapter with **BLE peripheral mode**. MediaTek is the tested adapter; ordinary Bluetooth connectivity alone does not establish peripheral support. PhonePort checks the adapter shortly after it starts and says in the status bar and the setup guide if it cannot act as a keyboard and mouse.
- An unlocked iPhone with Bluetooth on, near the PC and charged.
- A network that lets the phone and PC reach each other. Ethernet on the PC and Wi-Fi on the phone work. Guest isolation or a VPN can block discovery.
- A Microsoft account signed in to the Microsoft Store on this PC.
- An agent that can read images and use the bundled **iosctl** helper where local desktop automation is permitted, or a supported native Windows desktop-input tool. Browser-only tools cannot operate this window.

Check the host, an existing install and any running receiver:

```powershell
[System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version
Get-PnpDevice -Class Bluetooth -PresentOnly | Select-Object FriendlyName, Status
Get-AppxPackage -Name GraniteAi.PhonePort | Select-Object Name, Version
Get-CimInstance Win32_Process |
    Where-Object { $_.Name -in @('PhonePort.exe', 'AirWindowsNative.exe', 'AirWindows.exe', 'native_hid.exe') } |
    Select-Object ProcessId, ExecutablePath, CommandLine
```

Confirm which iPhone the user wants to control. Reuse its receiver when possible; run one receiver at a time. Do not run the separate BLE bench provider alongside PhonePort.

Follow your environment's tool restrictions. The phone's controls are pixels, not browser or Windows accessibility elements. The bundled helper supplies desktop input through the existing receiver window; it does not override a restriction on CLI-based input. If neither that route nor a supported desktop tool is available, complete setup with the user's mouse and report **manual control ready; agent input unavailable**.

**Checkpoint:** intended phone, supported PC, reachable network and desktop-input tool identified.

## 2. Install from the Microsoft Store

Skip this step when `Get-AppxPackage -Name GraniteAi.PhonePort` already lists PhonePort.

Open the listing in the Store app:

```powershell
Start-Process 'ms-windows-store://pdp/?productid=9N62X9J9KVT8'
```

The same listing is on the web at https://apps.microsoft.com/detail/9N62X9J9KVT8. Ask the user to start the free trial (or buy) on that page and wait for the install to finish. The Store, not PhonePort, shows the price and takes payment. Then confirm the install and its two commands:

```powershell
Get-AppxPackage -Name GraniteAi.PhonePort | Select-Object Name, Version, PackageFamilyName
Get-Command phoneport.exe, iosctl.exe | Select-Object Name, Source
```

`phoneport` starts the receiver and `iosctl` is the helper; both work from any folder. The install folder under `WindowsApps` is protected: never run `PhonePort.exe` from it by path, because a copy started that way has no package identity.

**Checkpoint:** PhonePort is installed and `phoneport` and `iosctl` resolve.

## 3. Start PhonePort

For the user: open **PhonePort** from the Start menu. The Start menu entry turns frame access on.

For an agent, start it through the command when no intended receiver is already running, then find its PID:

```powershell
Start-Process phoneport.exe -ArgumentList '--frames'
Start-Sleep -Seconds 2
iosctl --list
```

`iosctl --list` prints one JSON line per receiver. Record the `pid` of the intended one as `$receiverPid`; take it again after any restart.

Keep the window visible. It has one button row: the phone button (pair, choose a phone, disconnect), Home, Screenshot, Record, Full screen and Menu, with a status bar at the bottom. A first launch also opens a setup guide beside the picture that ticks off the steps below as they happen. During the trial the status bar shows the days left, and Menu > Help > Buy PhonePort… opens the Store's purchase dialog. When the trial has ended, a panel asks the user to buy; mirroring and Bluetooth control stay off until they do.

The Store install brings Windows Firewall rules for private and domain networks. If PhonePort reports a Public network, ask the user to set that network to Private in Windows Settings (the guide's **Open network settings** button opens the page). Keep the receiver and agent in the same normal Windows user session.

If an existing receiver lacks `--frames`, run `phoneport --frames` again: the second launch turns frame access on in the running receiver and exits, and Screen Mirroring continues.

**Checkpoint:** one visible receiver, known PID, `--frames` enabled.

## 4. Connect the picture

Ask the user: **“On the unlocked iPhone, open Control Center > Screen Mirroring and select the name shown in PhonePort. Is your phone's screen visible?”**

Have them open Settings and verify that the change appears in the mirror. If the receiver is absent, check network reachability, the network type (Private), and VPN/guest isolation. Restart after a network-interface change if necessary.

**Checkpoint:** a visible change on the intended iPhone appears in PhonePort.

## 5. Connect Bluetooth input

1. Ask the user to turn on **Settings > Accessibility > Touch > AssistiveTouch**.
2. Click the phone button in PhonePort while it reads **Pair Bluetooth** (otherwise phone button > **Pair Bluetooth**). Discovery stays open for 120 seconds. If the status bar says the Bluetooth adapter cannot act as a keyboard and mouse, resolve the adapter or driver limitation first. If it says no Bluetooth adapter was found or Bluetooth could not start, turn Bluetooth on in Windows and click **Pair Bluetooth** again.
3. Ask the user to open the iPhone's **main Settings > Bluetooth** page and select the PC under Other Devices while discovery is still open. The PC's Bluetooth name can differ from the AirPlay receiver name. iOS can keep listing the PC after discovery closes; if pairing finishes then, iOS saves the pairing without PhonePort's input service.
4. Complete any pairing prompt shown. The tested setup did not show a Windows code prompt; that alone is not a failure.
5. Select the intended phone in PhonePort by choosing its name from the phone button, with that same phone still mirroring.

The app should say to click the mirrored phone to control it. If discovery expires, click Pair Bluetooth again. If the phone's name never appears in the phone button's menu, inspect the status and recent log entries. The log is in the receiver's data folder, which `iosctl --doctor` reports:

```powershell
$dataFolder = (iosctl --doctor --pid $receiverPid | ConvertFrom-Json).data_folder
Get-Content -LiteralPath (Join-Path $dataFolder 'debug.txt') -Tail 100 |
    Select-String -Pattern '\[CONTROL\]'
```

Readiness requires matching Bluetooth and AirPlay identities. An old `Ready=1` log entry or the phone's Connected label is insufficient. Do not copy device fingerprints or force saved identity settings.

iOS shows the PC as Connected for any Bluetooth link, including classic Bluetooth, so that label does not mean PhonePort has the phone. The phone appears in the phone button's menu only after it subscribes to the keyboard and mouse reports, usually within a second of pairing. After a good pairing the PC can appear twice under My Devices. If the phone is still missing a minute after pairing, ask the user to forget the PC on the iPhone and remove the iPhone in Windows Bluetooth settings, then click Pair Bluetooth and have them select the PC under Other Devices within the window.

Select the intended phone explicitly if several appear. PhonePort does not check that the bound phone and the mirrored phone are the same device, so a different paired phone can bind and still report ready. Confirm the selection with the user before sending input. A bonded phone can take a minute or two to appear; keep Pair Bluetooth open and reopen it when it expires. PhonePort keeps Bluetooth open, remembers one phone per mirror and selects it automatically; a mirror it has not seen needs one selection, and entries marked `(a different iPhone)` belong to another mirror. The receiver keeps notifications addressed to the selected device and remembers it for reconnection. If the connection is unstable, inspect the current binding and subscriptions; disconnect an unrelated phone only as a diagnostic. Preserve unrelated pairings.

**Checkpoint:** the intended phone is selected, its picture is live and control remains ready.

## 6. Check input and set up Home

Click a harmless visible Settings row inside the phone picture. Confirm that it opens. Click a search field, type `Bluetooth` and check the exact text. Click another Windows application and confirm that typing returns to the PC. No separate Enable step or Escape sequence is required.

Set up Home once:

1. On the phone, open **AssistiveTouch > Devices > your PC > Customize Additional Buttons**.
2. Click **Home** in the PhonePort toolbar.
3. Assign the detected **Button 2** to **Home** on the phone.
4. Open an app, click PhonePort Home again and verify the Home Screen appears.

Right-click inside the phone picture invokes the same assigned action. Right-click in the bars or window chrome opens the PC menu. Use a left-button drag to scroll a list; release and verify movement. Use the assigned button for Home; it is one pulse and does not depend on gesture timing.

**Checkpoint:** click, exact text, list drag, Home and return to PC all work on this phone.

## 7. Give the agent a current image

`iosctl` captures decoded phone images without Python and without changing focus. Work in a folder of your own; the install folder is read-only:

```powershell
$workDir = Join-Path $env:USERPROFILE 'PhonePortSetup'
New-Item -ItemType Directory -Path (Join-Path $workDir '.local') -Force | Out-Null
Set-Location -LiteralPath $workDir
# Recreate $receiverPid from `iosctl --list` if this is a new shell.
iosctl --pid $receiverPid 'preflight'
if ($LASTEXITCODE -ne 0) { throw 'Phone control is not ready; inspect the reported reason.' }
iosctl --pid $receiverPid 'shot ".local\phone.png"'
if ($LASTEXITCODE -ne 0) { throw 'No fresh image. Do not use an older output file.' }
```

Read the PNG with your image tool. Its JSON sidecar records receiver identity, dimensions and age. `preflight` checks fresh frames and current binding; it does not send input or prove a physical result. The helper uses .NET Framework 4.8, which Windows 11 includes.

Inactive or older-than-one-second images are rejected. A static phone screen sends no frames and ages out within a second. Start the batch with `wake`, which moves the pointer without pressing and stops if no new frame arrives: `iosctl --pid $receiverPid 'wake; shot ".local\phone.png"'`. Never treat an old PNG as a new observation.

For `iosctl` input, use coordinates from the native-size phone image. It calculates the displayed picture bounds. For another desktop tool, map those pixels to that tool's screenshot coordinate space; see [coordinate mapping](app/CLAUDE-CONTROL.md#coordinates).

The older Python reader is in the install folder at `app\scripts\observe-phone.py` (`(Get-AppxPackage -Name GraniteAi.PhonePort).InstallLocation`). If using it, install `requirements-agent.txt` from the same folder into a Python 3.12+ environment. It is optional for the compiled helper.

**Checkpoint:** the agent reads a current image from the intended receiver.

## 8. Prove agent control

Use `iosctl` where your environment permits its desktop input, or your supported desktop tool, for these tests. Read [the helper guide](app/tools/iosctl/README.md) before sending commands. From 1.0.2, a host that speaks the Model Context Protocol can register `iosctl --mcp` and run the same tests with its `phoneport_` tools ([setup](app/tools/iosctl/README.md#mcp-server)):

| Action | Visible result |
|---|---|
| Click a harmless target | The expected screen opens |
| Click Search and type `Bluetooth` | That exact text appears |
| Drag a list and release | The list scrolls; dragging stops |
| Invoke the mapped Home action | The Home Screen appears |
| Click a PC text field and type | Text goes to Windows |

Observe, act once, observe again. Allow roughly 600–700 ms initially; animations and longer text can take more time. Keep focus inside the video until input finishes. If the result is uncertain, inspect it before choosing the next action. Do not retry automatically or infer task success from changed pixels alone.

Before typing, confirm the text field shows a cursor. With no field focused, iOS treats a hardware keyboard as a navigation keyboard: Space and Enter activate the focused control. Some iOS apps ignore typed digits; the iOS Calculator accepted taps on its keys but not digits typed on the keyboard.

Text supports US ASCII, Enter, Backspace, Tab and arrows. General shortcuts and Unicode are unsupported. Ctrl+V supports up to 1,024 ASCII characters; test paste and wheel separately if needed. Some agent tools type through the clipboard.

Save a brief local handoff in `$workDir\.local`: PhonePort version, adapter, phone model/iOS version, desktop tool, and each test's result. Keep device identifiers and phone screenshots out of public issues and commits.

Report **ready for agent iPhone control** when the agent completes these tests. If only manual control passed, name the missing agent capability.

## Resume next time

Open PhonePort from the Start menu (or run `phoneport --frames`), unlock the same iPhone and reconnect Screen Mirroring. PhonePort remembers the selected Bluetooth phone and matching AirPlay sender. Wait for control readiness, find the PID with `iosctl --list` and obtain a current image. Pairing is not required on every launch.

Clicking another application returns control to Windows. Disconnect or closing PhonePort ends its Bluetooth control service. Do not replay actions from an earlier connection.

## Troubleshooting

Start with `iosctl --doctor`. It sends no input and reports the next setup step and the receiver's `data_folder`. Exit `0` means software prerequisites are ready; exit `2` means action is needed. With multiple receivers, inspect one explicitly with `--doctor --pid PID`. Help > PhonePort support in the app opens the support page.

An idle screen can exceed the normal one-second image limit. `iosctl --pid PID 'snapshot ".local\idle.png"'` saves its retained picture and reports its age. Use this only for inspection, never as proof of a click or permission to act. Obtain a fresh `shot` or use a permitted live desktop observation for input and verification.

| Symptom | Next step |
|---|---|
| Store page does not open | Open https://apps.microsoft.com/detail/9N62X9J9KVT8 in a browser and install from there |
| `phoneport` or `iosctl` not found after install | Open a new terminal; then check that both are on in Settings > Apps > Advanced app settings > App execution aliases |
| Trial ended panel | The user buys PhonePort from the panel or Help > Buy PhonePort…; mirroring resumes without a restart |
| Receiver missing from Screen Mirroring | Check the network type (Private), VPN/guest isolation and that the phone is on the same network |
| Adapter cannot act as a keyboard and mouse | Check adapter/driver support for the BLE peripheral role |
| PC missing from Bluetooth discovery | Reopen the 120-second Pair Bluetooth window |
| Connected on phone, phone missing from the menu | Use the main Bluetooth settings route; inspect current subscriptions/status |
| Repeated disconnects | Inspect the selected phone and current subscriptions; isolate an unrelated phone if needed |
| Control dropped and the phone does not return | Ask the user to remove the PC from the iPhone's Bluetooth list, and the phone from Windows Bluetooth settings if listed. Click Pair Bluetooth, re-pair from the iPhone within the window, then choose the phone again. One confirmed cause: pairing finished after discovery closed, so the saved pairing has no input service |
| iPhone lists the PC as Connected, phone missing from the menu | The link is classic Bluetooth only. Forget the PC on the iPhone, remove the iPhone in Windows, click Pair Bluetooth and re-pair within the window |
| Wrong phone responds while status is ready | Disconnect, choose the intended phone and confirm with the user |
| User's first click does nothing on the phone | Known issue: the first click focuses the window. Click again |
| `Windows denied focus` from `iosctl` | Bring the agent's terminal to the foreground and retry. No press was sent |
| Clicks miss | Recalculate picture bounds, DPI scale and overlay positions |
| Typing stops or goes elsewhere | Restore the intended field and video focus, then inspect before continuing |
| Home opens the PC menu | Use the toolbar or click inside the picture; verify Button 2 mapping |
| Frame is stale or wrong PID | Run `--doctor`; inspect an idle image with `snapshot`, then obtain a fresh observation before input |
| Agent sees but cannot click | Use iosctl where permitted, or supply a supported native desktop-input tool |

## Older builds

These notes apply only to a PC that already runs a ZIP build from before 1.0.0. New installs use the Microsoft Store.

- 0.3.1-rc.5 and earlier use the old name: `AirWindowsNative.exe`, `Start-AirWindows.cmd`, an `AirWindows` folder. Start it with `Start-AirWindows.cmd`; run it again to turn on frame access in a running receiver. Use `iosctl.exe` from the same folder (`.\iosctl.exe` instead of `iosctl`). Its release notes and files are on the [0.3.1-rc.5 release page](https://github.com/ClariSortAi/airwindows/releases/tag/v0.3.1-rc.5).
- ZIP and source builds keep settings and the log in `%LOCALAPPDATA%\AirWindows`; `iosctl --doctor` prints it as `data_folder`. The first start of the Store install copies `settings.ini` from there, so paired phones carry over.
- In 0.3.1-rc.5 and earlier, mirroring a different phone closes Bluetooth control: click Pair Bluetooth and select that phone again.
- 0.3.1-rc.4 and earlier have Pair Bluetooth, a phone selector, Connect, Disconnect and Home controls instead of the phone button, a menu bar, no setup guide and no adapter check (`BLE peripheral role is unavailable` in the status line). A running 0.3.1-rc.4 receiver without `--frames` must be restarted, which interrupts Screen Mirroring.
- `iosctl --doctor` and idle snapshots need a receiver from 0.3.1-rc.2 or later. Upgrade the receiver and helper together.

Further reading: [native control](app/NATIVE-CONTROL.md), [agent operation](app/CLAUDE-CONTROL.md), [frame protocol](app/FRAME-INTERFACE.md).
