# For agents: zero to ready iPhone control

Set up AirWindows, pair the user's iPhone, then prove that you can see, click, type, scroll and go Home. Work through these steps in order. Handle PC setup yourself and ask the user for one phone action at a time.

AirPlay supplies the picture; the PC's Bluetooth adapter sends mouse and keyboard input. No iPhone app, ESP32 or phone-to-PC cable is required.

## 1. Check the essentials

You need:

- Windows 11 x64 and a Bluetooth adapter with **BLE peripheral mode**. MediaTek is the tested adapter; ordinary Bluetooth connectivity alone does not establish peripheral support.
- An unlocked iPhone with Bluetooth on, near the PC and charged.
- A network that lets the phone and PC reach each other. Ethernet on the PC and Wi-Fi on the phone work. Guest isolation or a VPN can block discovery.
- An agent that can read images and use the bundled **iosctl** helper where local desktop automation is permitted, or a supported native Windows desktop-input tool. Browser-only tools cannot operate this window.

Check the host and any existing receiver:

```powershell
[System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version
Get-PnpDevice -Class Bluetooth -PresentOnly | Select-Object FriendlyName, Status
Get-CimInstance Win32_Process |
    Where-Object { $_.Name -like 'AirWindows*.exe' -or $_.Name -eq 'native_hid.exe' } |
    Select-Object ProcessId, ExecutablePath, CommandLine
```

Confirm which iPhone the user wants to control. Reuse its receiver when possible; run one receiver at a time. Do not run the separate BLE bench provider alongside AirWindows.

Follow your environment's tool restrictions. The phone's controls are pixels, not browser or Windows accessibility elements. The bundled helper supplies desktop input through the existing receiver window; it does not override a restriction on CLI-based input. If neither that route nor a supported desktop tool is available, complete setup with the user's mouse and report **manual control ready; agent input unavailable**.

**Checkpoint:** intended phone, supported PC, reachable network and desktop-input tool identified.

## 2. Download and unpack

Use the [Windows package](https://github.com/ClariSortAi/airwindows/releases/download/v0.3.1-rc.2/AirWindows-0.3.1-rc.2-win-x64.zip). It includes the receiver, launcher, compiled `iosctl` helper, frame reader, this runbook and operating guides. No compiler, GitHub account or source checkout is needed.

Run this in PowerShell to download the package and check its SHA-256 checksum:

```powershell
$ErrorActionPreference = 'Stop'
$setupRoot = Join-Path $env:USERPROFILE 'AirWindowsSetup'
$downloadDir = Join-Path $setupRoot 'downloads'
$installDir = Join-Path $setupRoot '0.3.1-rc.2'
if (Test-Path -LiteralPath $installDir) {
    throw 'This installation already exists. Reuse it, or choose a new installDir.'
}
New-Item -ItemType Directory -Path $downloadDir -Force | Out-Null
$releaseUrl = 'https://github.com/ClariSortAi/airwindows/releases/download/v0.3.1-rc.2'
$zipName = 'AirWindows-0.3.1-rc.2-win-x64.zip'
Invoke-WebRequest "$releaseUrl/$zipName" -OutFile (Join-Path $downloadDir $zipName)
Invoke-WebRequest "$releaseUrl/SHA256SUMS.txt" -OutFile (Join-Path $downloadDir 'SHA256SUMS.txt')
$entry = @(Get-Content -LiteralPath (Join-Path $downloadDir 'SHA256SUMS.txt') |
    Where-Object { $_ -match ('^[0-9a-fA-F]{64}\s+' + [regex]::Escape($zipName) + '$') })
if ($entry.Count -ne 1) { throw 'Missing or ambiguous Windows ZIP checksum.' }
$expected = ($entry[0] -split '\s+', 2)[0]
$zipPath = Join-Path $downloadDir $zipName
if ((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash -ine $expected) {
    throw 'Checksum mismatch. Download the package again before continuing.'
}
Expand-Archive -LiteralPath $zipPath -DestinationPath $installDir
$packageRoot = Join-Path $installDir 'AirWindows'
$receiverExe = Join-Path $packageRoot 'AirWindowsNative.exe'
Get-Item -LiteralPath $receiverExe | Select-Object FullName, Length
```

Record `$packageRoot` and `$receiverExe` as full paths. Shell calls may start new processes, so recreate these variables when needed. A user who already extracted the ZIP can start here by supplying its `AirWindows` folder.

The [release page](https://github.com/ClariSortAi/airwindows/releases/tag/v0.3.1-rc.2) also supplies matching source and checksums. Source is needed for development or redistribution, not normal setup. The receiver is unsigned; Windows may show a SmartScreen notice.

**Checkpoint:** the verified package is extracted and its executable path is known.

## 3. Start AirWindows

For the user, double-click **Start-AirWindows.cmd**. It opens the receiver with frame access enabled.

For an agent, launch the executable directly when no intended receiver is already running:

```powershell
# Set $receiverExe to the full path recorded in Step 2.
$receiverProcess = Start-Process -FilePath $receiverExe -ArgumentList '--frames' `
    -WorkingDirectory (Split-Path -Parent $receiverExe) -WindowStyle Normal -PassThru
$receiverPid = $receiverProcess.Id
$receiverProcess | Select-Object Id, Path
```

Keep the window visible. It has Pair Bluetooth, a phone selector, Connect, Disconnect and Home controls. Allow this executable through Windows Firewall on the trusted private network in use. Keep the receiver and agent in the same normal Windows user session.

If an existing receiver lacks `--frames`, finish any active operation before restarting it. A restart interrupts Screen Mirroring. Rediscover its PID afterward.

**Checkpoint:** one visible receiver, known PID, `--frames` enabled.

## 4. Connect the picture

Ask the user: **“On the unlocked iPhone, open Control Center > Screen Mirroring and select the name shown in AirWindows. Is your phone's screen visible?”**

Have them open Settings and verify that the change appears in the mirror. If the receiver is absent, check network reachability, the firewall allowance and VPN/guest isolation. Restart after a network-interface change if necessary.

**Checkpoint:** a visible change on the intended iPhone appears in AirWindows.

## 5. Connect Bluetooth input

1. Ask the user to turn on **Settings > Accessibility > Touch > AssistiveTouch**.
2. Click **Pair Bluetooth** in AirWindows. Discovery stays open for 120 seconds. If it reports `BLE peripheral role is unavailable`, resolve the adapter or driver limitation first.
3. Ask the user to open the iPhone's **main Settings > Bluetooth** page and select the PC under Other Devices. The PC's Bluetooth name can differ from the AirPlay receiver name.
4. Complete any pairing prompt shown. The tested setup did not show a Windows code prompt; that alone is not a failure.
5. Select the intended phone in AirWindows and click **Connect**, with that same phone still mirroring.

The app should say to click the mirrored phone to control it. If discovery expires, click Pair Bluetooth again. If the selector stays empty, inspect the status and recent log entries:

```powershell
Get-Content -LiteralPath "$env:LOCALAPPDATA\AirWindows\debug.txt" -Tail 100 |
    Select-String -Pattern '\[CONTROL\]'
```

Readiness requires matching Bluetooth and AirPlay identities. An old `Ready=1` log entry or the phone's Connected label is insufficient. Do not copy device fingerprints or force saved identity settings.

Select the intended phone explicitly if several subscribers appear. The receiver keeps notifications addressed to that selected device and remembers it for reconnection. If the connection is unstable, inspect the current binding and subscriptions; disconnect an unrelated subscriber only as a diagnostic. Preserve unrelated pairings.

**Checkpoint:** the intended phone is selected, its picture is live and control remains ready.

## 6. Check input and set up Home

Click a harmless visible Settings row inside the phone picture. Confirm that it opens. Click a search field, type `Bluetooth` and check the exact text. Click another Windows application and confirm that typing returns to the PC. No separate Enable step or Escape sequence is required.

Set up Home once:

1. On the phone, open **AssistiveTouch > Devices > your PC > Customize Additional Buttons**.
2. Click **Home** in the AirWindows toolbar.
3. Assign the detected **Button 2** to **Home** on the phone.
4. Open an app, click AirWindows Home again and verify the Home Screen appears.

Right-click inside the phone picture invokes the same assigned action. Right-click in the bars or window chrome opens the PC menu. Use a left-button drag to scroll a list; release and verify movement. Use the assigned button for Home, since bottom-edge dragging was unreliable in testing.

**Checkpoint:** click, exact text, list drag, Home and return to PC all work on this phone.

## 7. Give the agent a current image

The Windows ZIP includes `iosctl.exe`; it captures decoded phone images without Python and without changing focus. From the extracted package folder:

```powershell
# Recreate $receiverExe and $packageRoot from the recorded paths.
Set-Location -LiteralPath $packageRoot
$iosctlExe = Join-Path $packageRoot 'iosctl.exe'
if (-not (Test-Path -LiteralPath $iosctlExe)) {
    $iosctlExe = Join-Path $packageRoot 'app\build\Release\bin\iosctl.exe'
}
$receivers = @(Get-CimInstance Win32_Process | Where-Object {
    $_.Name -eq 'AirWindowsNative.exe' -and $_.ExecutablePath -eq $receiverExe
})
if ($receivers.Count -ne 1) { throw 'Expected exactly one receiver at the selected path.' }
$receiverPid = [int]$receivers[0].ProcessId
& $iosctlExe --pid $receiverPid 'preflight'
if ($LASTEXITCODE -ne 0) { throw 'Phone control is not ready; inspect the reported reason.' }
& $iosctlExe --pid $receiverPid 'shot ".local\phone.png"'
if ($LASTEXITCODE -ne 0) { throw 'No fresh image. Do not use an older output file.' }
```

Read the PNG with your image tool. Its JSON sidecar records receiver identity, dimensions and age. `preflight` checks fresh frames and current binding; it does not send input or prove a physical result. The helper uses .NET Framework 4.8, available on the tested Windows 11 setup.

Inactive or older-than-one-second images are rejected. A static phone screen can age out; obtain a current view through a permitted desktop tool or wait for a visible update. Never treat an old PNG as a new observation.

For `iosctl` input, use coordinates from the native-size phone image. It calculates the displayed picture bounds. For another desktop tool, map those pixels to that tool's screenshot coordinate space; see [coordinate mapping](app/CLAUDE-CONTROL.md#coordinates).

The older Python reader remains in `app\scripts\observe-phone.py`. If using it, install `requirements-agent.txt` into a Python 3.12+ environment. It is optional for the compiled helper.

**Checkpoint:** the agent reads a current image from the intended receiver.

## 8. Prove agent control

Use `iosctl` where your environment permits its desktop input, or your supported desktop tool, for these tests. Read [the helper guide](app/tools/iosctl/README.md) before sending commands:

| Action | Visible result |
|---|---|
| Click a harmless target | The expected screen opens |
| Click Search and type `Bluetooth` | That exact text appears |
| Drag a list and release | The list scrolls; dragging stops |
| Invoke the mapped Home action | The Home Screen appears |
| Click a PC text field and type | Text goes to Windows |

Observe, act once, observe again. Allow roughly 600–700 ms initially; animations and longer text can take more time. Keep focus inside the video until input finishes. If the result is uncertain, inspect it before choosing the next action. Do not retry automatically or infer task success from changed pixels alone.

Text supports US ASCII, Enter, Backspace, Tab and arrows. General shortcuts and Unicode are unsupported. Ctrl+V supports up to 1,024 ASCII characters; test paste and wheel separately if needed. Some agent tools type through the clipboard.

Save a brief local handoff in `$packageRoot\.local`: executable path/version, adapter, phone model/iOS version, desktop tool, and each test's result. Keep device identifiers and phone screenshots out of public issues and commits.

Report **ready for agent iPhone control** when the agent completes these tests. If only manual control passed, name the missing agent capability.

## Resume next time

Start **Start-AirWindows.cmd**, unlock the same iPhone and reconnect Screen Mirroring. AirWindows remembers the selected Bluetooth phone and matching AirPlay sender. Wait for control readiness, rediscover the process and obtain a current image. Pairing is not required on every launch.

Clicking another application returns control to Windows. Disconnect or closing AirWindows ends its Bluetooth control service. Do not replay actions from an earlier connection.

## Troubleshooting

Start with `iosctl --doctor` from the extracted package folder. It sends no input and reports the next setup step. Exit `0` means software prerequisites are ready; exit `2` means action is needed. With multiple receivers, inspect one explicitly with `--doctor --pid PID`. Upgrade the receiver and helper together; these diagnostics require 0.3.1-rc.2 or later.

An idle screen can exceed the normal one-second image limit. `iosctl --pid PID 'snapshot ".local\idle.png"'` saves its retained picture and reports its age. Use this only for inspection, never as proof of a click or permission to act. Obtain a fresh `shot` or use a permitted live desktop observation for input and verification.

| Symptom | Next step |
|---|---|
| Download fails | Retry the linked release download and check network/proxy access |
| Receiver missing from Screen Mirroring | Check network, firewall and VPN/guest isolation |
| BLE peripheral role unavailable | Check adapter/driver support |
| PC missing from Bluetooth discovery | Reopen the 120-second Pair Bluetooth window |
| Connected on phone, selector empty | Use the main Bluetooth settings route; inspect current subscriptions/status |
| Repeated disconnects | Inspect the selected phone and current subscriptions; isolate an unrelated subscriber if needed |
| Clicks miss | Recalculate picture bounds, DPI scale and overlay positions |
| Typing stops or goes elsewhere | Restore the intended field and video focus, then inspect before continuing |
| Home opens the PC menu | Use the toolbar or click inside the picture; verify Button 2 mapping |
| Frame is stale or wrong PID | Run `--doctor`; inspect an idle image with `snapshot`, then obtain a fresh observation before input |
| Agent sees but cannot click | Use iosctl where permitted, or supply a supported native desktop-input tool |

For source builds, follow [the build guide](app/README.md#build). Set `$packageRoot` to the source root and `$receiverExe` to `app\build\Release\bin\AirWindowsNative.exe`, then continue at Step 3. Keep `app` and `native-ble-prototype` beside each other.

Further reading: [native control](app/NATIVE-CONTROL.md), [agent operation](app/CLAUDE-CONTROL.md), [frame protocol](app/FRAME-INTERFACE.md).
