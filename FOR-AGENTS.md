# For agents: zero to ready iPhone control

Use this runbook when a user asks Claude Code, Codex or another desktop agent to set up AirWindows on a new Windows PC. Work through the steps in order. Complete each checkpoint before continuing, and resume from the first incomplete step when interrupted.

The end state is specific: the intended iPhone is visibly mirrored, mouse and keyboard input reach that phone, Home works, clicking another Windows application returns control to the PC, and the agent can observe and verify its own actions.

AirPlay carries the picture over the network. The PC's Bluetooth adapter carries keyboard and mouse input. There is no iPhone app to install and no ESP32 or phone-to-PC cable to attach. This remains experimental software; a successful setup on one PC does not establish compatibility with another adapter or iOS version.

## 0. Establish what you can operate

Read the repository's applicable instructions and [native control guide](app/NATIVE-CONTROL.md). Use the tools actually available in your session.

| Capability | Needed for |
|---|---|
| Windows shell and file access | Install/build, inspect processes and logs, run the frame reader |
| Image reading | Interpret the mirrored screen and verify results |
| Authorized Windows desktop input | Focus AirWindows, click its toolbar and phone picture, drag and type |
| User holding the unlocked iPhone | Initial mirroring, AssistiveTouch, Bluetooth pairing and button assignment |

A shell and PNG reader do not provide desktop input. A browser-only tool cannot operate this native window. The iPhone's controls are pixels inside a video surface, not DOM or Windows accessibility elements. Use ordinary desktop mouse/keyboard input through a supported tool; follow that tool's restrictions.

If desktop input is unavailable, continue preparing the software and guide the user through manual controls. Report **manual control ready; agent input unavailable** if the manual test passes. Do not claim agent readiness or invent an input API. The frame pipe is read-only.

Tell the user what they will need to do on the phone. Ask one short, concrete question at each phone checkpoint, rather than handing them the whole procedure at once. Handle approved PC work yourself. Leave passwords, unlock codes, sign-in and system consent prompts to the user when your tools cannot handle them.

**Checkpoint:** identify the tool that will supply desktop input, or record that this session will be assisted by the user.

## 1. Check the PC, phone and network

Use native 64-bit Windows. Windows 11 x64 is the tested starting point; do not treat ARM64, a VM, WSL or another OS as equivalent. The Bluetooth adapter must support **BLE peripheral mode**, not merely Bluetooth audio or connecting a mouse.

Run these read-only checks in PowerShell:

```powershell
[System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, OSArchitecture
Get-PnpDevice -Class Bluetooth -PresentOnly | Select-Object FriendlyName, Status
Get-NetConnectionProfile | Select-Object InterfaceAlias, NetworkCategory, IPv4Connectivity
Get-CimInstance Win32_Process |
    Where-Object { $_.Name -like 'AirWindows*.exe' -or $_.Name -eq 'native_hid.exe' } |
    Select-Object ProcessId, ExecutablePath, CommandLine
Get-Command winget, git, gh, py, python, cmake, ninja -ErrorAction SilentlyContinue |
    Select-Object Name, Source
```

Ask the user to confirm the intended iPhone, its iOS version, that it is unlocked and that Bluetooth is on. Keep it charged and near the PC during setup. Put the phone and PC on a trusted network where they can reach each other. The PC can use Ethernet while the phone uses Wi-Fi; sharing an SSID alone does not prove reachability. Guest/client isolation or a VPN can interfere.

The device list above identifies adapters; it does **not** prove peripheral support. AirWindows checks that capability when Pair Bluetooth starts. If it reports `BLE peripheral role is unavailable`, resolve the adapter/driver limitation before attempting more pairings. Do not promise that any USB Bluetooth dongle will work.

Reuse an existing, intended native receiver if present. Do not launch competing AirWindows instances or a separate BLE bench provider. If another paired phone keeps taking the connection, have the user temporarily disconnect that specific phone; do not delete unrelated bonds.

**Checkpoint:** supported host, intended phone, reachable network and one prospective controller process identified.

## 2. Choose how to obtain AirWindows

Check the [repository](https://github.com/ClariSortAi/airwindows) and [releases](https://github.com/ClariSortAi/airwindows/releases). Do not assume a release is public, published, current or accessible to the user's account. At this guide's creation on 2026-10-30, the repository was private and `v0.3.0-preview.1` was a draft. Recheck those facts.

- **Prebuilt route:** use a trusted native-control release or a maintainer-supplied package. Obtain its Windows ZIP, matching source ZIP and `SHA256SUMS.txt`. The source contains the frame reader and operating instructions. Skip compiler installation and building.
- **Source route:** obtain repository access and clone it, or use a maintainer-supplied source archive. Install the build tools in Step 3, then build in Step 4.

An older `AirWindows.exe` download may provide mirroring without input. Choose the native-control build described by its release notes. A GitHub-generated repository ZIP is also different from the release's matching source ZIP, which includes the linked upstream sources.

For an access error, have the user sign in with the intended GitHub account or request access from the maintainer. Do not ask them to paste a token into chat, invent an anonymous download URL or change repository visibility. GitHub CLI users can run `gh auth login` interactively, then `gh repo view ClariSortAi/airwindows`.

Choose a user-writable setup folder. These examples use:

```powershell
$setupRoot = Join-Path $env:USERPROFILE 'AirWindowsSetup'
New-Item -ItemType Directory -Path $setupRoot -Force | Out-Null
```

Record full paths as you discover them. Agent shell calls often start fresh processes: PowerShell variables do not necessarily survive between calls. Supply the correct working directory and recreate the recorded variables for each command block.

**Checkpoint:** the user has access to the selected source/package, and the chosen installation path is recorded.

### Prebuilt route: verify and extract

Put the three matching release files in a `downloads` subfolder of the setup folder. Verify both ZIPs against the supplied checksum file before extracting:

```powershell
$setupRoot = Join-Path $env:USERPROFILE 'AirWindowsSetup'
$downloadDir = Join-Path $setupRoot 'downloads'
$zipFiles = @(Get-ChildItem -LiteralPath $downloadDir -Filter 'AirWindows-*.zip')
if ($zipFiles.Count -ne 2) { throw 'Use one matching Windows ZIP and source ZIP.' }
$sumLines = Get-Content -LiteralPath (Join-Path $downloadDir 'SHA256SUMS.txt')
foreach ($zip in $zipFiles) {
    $entry = @($sumLines | Where-Object {
        $_ -match ('^[0-9a-fA-F]{64}\s+' + [regex]::Escape($zip.Name) + '$')
    })
    if ($entry.Count -ne 1) { throw "Missing or ambiguous checksum: $($zip.Name)" }
    $expected = ($entry[0] -split '\s+', 2)[0]
    if ((Get-FileHash -LiteralPath $zip.FullName -Algorithm SHA256).Hash -ine $expected) {
        throw "Checksum mismatch: $($zip.Name)"
    }
}
```

Extract the Windows ZIP to a new `binary` directory and the source ZIP to a new `source` directory under the setup folder. Use `Expand-Archive -LiteralPath <verified-zip> -DestinationPath <new-directory>` with the resolved paths; do not overwrite an existing installation. A checksum confirms matching bytes, not the trustworthiness of an unknown distributor.

Record:

- `$receiverExe`: the extracted `AirWindows\AirWindowsNative.exe`.
- `$repoRoot`: the extracted source's top-level folder containing both `app` and `native-ble-prototype`.

Continue with Python setup in Step 3, then go directly to Step 5.

## 3. Install only the missing prerequisites

Python is needed for the decoded frame reader. Use Python 3.12 for the documented path. A prebuilt receiver itself needs no compiler or Python to provide manual control.

Use `winget show --exact --id <package-id> --source winget` to inspect a package before installation. The commands below use Microsoft's [WinGet installation interface](https://learn.microsoft.com/en-us/windows/package-manager/winget/install). Run only the rows needed for the chosen route; do not upgrade working tools merely to follow this list.

```powershell
# Frame reader; both routes when preparing agent control:
winget install --exact --id Python.Python.3.12 --source winget

# Source build only:
winget install --exact --id Git.Git --source winget
winget install --exact --id Kitware.CMake --source winget
winget install --exact --id Ninja-build.Ninja --source winget
winget install --exact --id Microsoft.VisualStudio.2022.BuildTools --source winget --override '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'

# Optional, for authenticated GitHub operations:
winget install --exact --id GitHub.cli --source winget
```

The Visual Studio command selects the C++ build workload and recommended components; see Microsoft's [installer parameters](https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio?view=visualstudio) and [workload IDs](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022). For an existing installation, use Visual Studio Installer > Modify to add Desktop development with C++, x64/x86 MSVC tools and a Windows SDK. Do not reinstall it blindly. Handle any requested reboot with the user before proceeding.

If WinGet is unavailable, use the vendors' installers and verify the resulting commands. Open a fresh shell/tool process after installation so PATH changes take effect. Check `git --version`, `cmake --version`, `ninja --version` for source builds, and `py -3.12 --version` for the frame reader. If the Python launcher is absent, use the verified Python executable's full path.

If PowerShell refuses a repository script, inspect the execution-policy error and the user's machine policy. Use an allowed, process-scoped way to run trusted scripts; do not change machine-wide policy or bypass organizational controls just to continue.

**Checkpoint:** the required commands resolve; source builds have MSVC and a Windows SDK. Allow several GB of free disk space and time for a first dependency build.

## 4. Obtain and build the source

Skip this step when using a verified prebuilt receiver.

For a new clone, run:

```powershell
$setupRoot = Join-Path $env:USERPROFILE 'AirWindowsSetup'
$repoRoot = Join-Path $setupRoot 'airwindows'
if (Test-Path -LiteralPath $repoRoot) { throw 'Inspect and reuse the existing checkout, or choose a new folder.' }
git clone https://github.com/ClariSortAi/airwindows.git $repoRoot
if ($LASTEXITCODE -ne 0) { throw 'Clone failed; resolve repository access first.' }
```

If using a supplied source archive, set `$repoRoot` to its extracted top-level directory instead. Verify that `app\CMakeLists.txt` and `native-ble-prototype\native_hid_backend.cpp` exist. Keep those sibling directories together.

Run from `$repoRoot`:

```powershell
$ErrorActionPreference = 'Stop'
.\app\scripts\bootstrap.ps1
.\app\scripts\build.ps1 -Target AirWindowsNative
if ($LASTEXITCODE -ne 0) { throw 'Build failed; inspect the first error.' }
Get-Item -LiteralPath .\app\build\Release\bin\AirWindowsNative.exe |
    Select-Object FullName, Length, LastWriteTime
```

Stop after a failed command; do not launch a stale executable. Bootstrap fetches pinned dependencies, and the first build can take tens of minutes. Report progress rather than starting a competing build. The scripts load the MSVC environment themselves; a normal shell not finding `cl.exe` is not by itself a build failure.

Set `$receiverExe` to the verified build output. Record its version and hash:

```powershell
# Set $receiverExe to the full path obtained above.
(Get-Item -LiteralPath $receiverExe).VersionInfo | Select-Object ProductVersion, FileVersion
Get-FileHash -LiteralPath $receiverExe -Algorithm SHA256
```

**Checkpoint:** one identified native-control executable exists and the build/package version is recorded. Building successfully is not yet proof of phone control.

## 5. Start one visible receiver

Inspect existing processes again. Reuse the intended receiver if it is already running with `--frames`; discover its PID from its executable path. If it is the wrong build or lacks `--frames`, explain the required restart and finish any active operation before replacing it. Do not terminate unrelated processes.

When no receiver is running, launch the chosen executable:

```powershell
# Set $receiverExe to the verified full executable path.
$receiverProcess = Start-Process -FilePath $receiverExe -ArgumentList '--frames' `
    -WorkingDirectory (Split-Path -Parent $receiverExe) -WindowStyle Normal -PassThru
$receiverProcess | Select-Object Id, Path
```

Record the actual PID as `$receiverPid`. The window must be visible. It should show the receiver name and Pair Bluetooth, phone selection, Connect, Disconnect and Home controls. If those controls are absent, resolve the build mismatch before continuing.

If Windows asks for firewall access, have the user allow this executable on the trusted private network in use. Do not disable the firewall globally or change a public network's classification blindly. Builds in different paths may need their own firewall allowance. Keep the receiver and agent in the same normal Windows user session; running the receiver elevated is not required for the tested setup.

**Checkpoint:** one visible native receiver, known executable path/PID, `--frames` enabled and network access permitted.

## 6. Establish the picture first

Ask the user: **"On the unlocked iPhone, open Control Center > Screen Mirroring and select the receiver name shown in AirWindows. Is your phone's screen now visible in the window?"**

Verify the correct phone by having the user open Settings or another harmless screen and observing that change in the mirror. An idle connection message or a static old screenshot is insufficient.

If the receiver does not appear, check the active network, firewall allowance and guest/VPN isolation. Restart the receiver only if a network-interface change requires it; rediscover its PID afterward. Stop troubleshooting this layer once fresh video works. Bluetooth is a separate connection.

**Checkpoint:** a visible change on the intended phone appears in AirWindows.

## 7. Pair Bluetooth and bind the same phone

1. Ask the user to open Settings > Accessibility > Touch > AssistiveTouch and turn it on. [Apple's AssistiveTouch guide](https://support.apple.com/en-md/guide/iphone/-iph96b21954/ios) describes pointer devices and button customization.
2. In AirWindows, click **Pair Bluetooth**. This opens a 120-second pairing window and checks the adapter's peripheral capability.
3. Ask the user to open the iPhone's **main Settings > Bluetooth** screen and select the PC under Other Devices. The PC's Bluetooth name can differ from a renamed AirPlay receiver. Confirm the intended PC rather than relying on a name from another setup.
4. Complete any pairing prompt the devices actually show. The original setup showed no Windows code prompt, so absence of that prompt alone is not failure. Do not invent a passkey.
5. Wait for the intended phone to appear in AirWindows' selector, choose it and click **Connect**. Keep that same phone mirroring.

The native selector requires the phone's report subscriptions, including the absolute mouse report. A phone saying Connected in AssistiveTouch alone is not enough. In the working bench setup, pairing from the main Bluetooth settings established the usable HID link.

If discovery expires, click Pair Bluetooth again and retry discovery once. If the selector is empty, inspect the current toolbar status and log before repeating. If more than one phone subscribes, have the user disconnect the unrelated phone and then select the intended one. Preserve unrelated pairings.

Read recent control entries:

```powershell
Get-Content -LiteralPath "$env:LOCALAPPDATA\AirWindows\debug.txt" -Tail 100 |
    Select-String -Pattern '\[CONTROL\]'
```

Look for current readiness and `[CONTROL] Ready=1; matching Bluetooth and AirPlay identities`. A historical Ready=1 line does not prove present readiness. The UI should say to click the mirrored phone to control it. Do not set `--control-phone`, copy fingerprints from another machine or edit the saved identity fields to force readiness.

**Checkpoint:** the intended phone is selected, its picture is live, and the app reports matching Bluetooth/AirPlay identities without repeated disconnects.

## 8. Prove basic input, then configure Home

Bring AirWindows forward. Click one harmless visible control inside the actual phone picture, such as a Settings row. A click both focuses the video and acts on the phone; it is not a focus-only operation. Confirm the corresponding screen change.

Next, click a phone search field and type `Bluetooth`. Confirm the exact text on the phone. Click another PC application and type a harmless short string there; confirm it stays on the PC. There is no separate Enable action or required Escape sequence.

Then ask the user to open AssistiveTouch > Devices > the paired PC > Customize Additional Buttons. While that prompt is open:

1. Click **Home** in the AirWindows toolbar, or right-click inside the phone picture.
2. Have the user assign the detected **Button 2** to **Home**. Seeing only Customize Additional Buttons before registration is normal.
3. Open an app and invoke Home again. Verify that the iPhone returns to its Home Screen.

If a right-click opens the PC menu, check the executable and click location. Bars and window chrome intentionally open that menu. Prefer the toolbar Home button while diagnosing. The phone's floating AssistiveTouch menu can also cover a target; move it aside if necessary.

Use a left-button drag to scroll a list, release it and confirm the list moved. A bottom-edge mouse drag was unreliable for Home, so do not use it as the Home acceptance test. If Button 2 cannot be mapped, the user can use the AssistiveTouch on-screen Home action while diagnosing; record Home mapping as incomplete.

**Checkpoint:** visible click, exact text entry, focus return and Home all pass on this user's phone.

## 9. Give the agent a reliable image

From `$repoRoot`, prepare an isolated Python environment. This works for a clone or the matching source archive and does not require activating it:

```powershell
if (-not (Test-Path -LiteralPath .\.venv\Scripts\python.exe)) {
    py -3.12 -m venv .venv
    if ($LASTEXITCODE -ne 0) { throw 'Python environment creation failed.' }
}
.\.venv\Scripts\python.exe -m pip install -r requirements-dev.txt
if ($LASTEXITCODE -ne 0) { throw 'Python dependencies failed to install.' }
```

Reuse an existing suitable `.venv` rather than replacing it. Discover the receiver PID again from its verified executable path if a restart occurred, then run from `$repoRoot`:

```powershell
# Set $receiverPid to the running receiver's actual process ID.
.\.venv\Scripts\python.exe .\app\scripts\observe-phone.py `
    --pid $receiverPid --output .\.local\phone.png
if ($LASTEXITCODE -ne 0) { throw 'No valid fresh frame; do not use an older output file.' }
```

Open `.local\phone.png` with the agent's image tool. Check the adjacent JSON for the expected PID, dimensions, generation and age. This command sends no input. It rejects inactive, synthetic or stale frames; a failed capture may leave an older PNG on disk. A static picture can age out while manual GUI input still works. Re-observe using the live desktop tool or diagnose mirroring; do not disable freshness checks to force a pass.

The PNG contains only the phone image. A desktop click needs the displayed picture's screen coordinates, excluding the toolbar and bars. Recalculate bounds after resize, window movement, rotation or DPI changes. Follow [the operating guide's coordinate mapping](app/CLAUDE-CONTROL.md#coordinates).

**Checkpoint:** the agent reads a valid image from this receiver and can locate the corresponding picture on the desktop.

## 10. Prove agent-driven control

Use the agent's actual desktop tool, not the user's mouse, for this final test. Keep focus in the video during an action; switching to a terminal or another window cancels queued input. Background shell commands can read frames without focusing a terminal.

| Test | Required visible result |
|---|---|
| Observe, then click a harmless target | The expected phone screen opens |
| Click Search, then type `Bluetooth` | That exact text appears in the intended field |
| Drag a list upward, then release | The list scrolls and the pointer is no longer dragging |
| Invoke the mapped Home action | The Home Screen appears |
| Click a PC text field and type | Text goes to Windows; the phone receives no further typing |

Perform one action, wait briefly for its result and observe again. Roughly 600-700 ms was useful in the bench tests; animations or longer typing can take longer. Inspect before deciding whether an action failed. Do not automatically replay after a timeout or lost acknowledgement.

US ASCII, Enter, Backspace, Tab and arrows are supported. General shortcuts and Unicode are not. Ctrl+V is implemented for up to 1,024 ASCII characters, but test it separately if needed. Some agent text tools use clipboard paste internally; establish which path your tool uses. Wheel input also needs its own test. Do not make either optional feature a substitute for the basic click/type/drag checks.

**Checkpoint:** the agent itself completes the test. If only the user completed it, report manual readiness and the missing agent capability separately.

## 11. Record readiness and leave a usable handoff

Save a short local setup note under `$repoRoot\.local` with the following fields. Keep phone screenshots and device identifiers out of commits and public issues.

```text
Setup date:
Windows version / architecture:
Bluetooth adapter / driver:
iPhone model / iOS version:
Executable path / version / SHA-256:
Source directory / Python executable:
Receiver PID (temporary; rediscover after restart):
AirPlay picture: PASS / FAIL
Bluetooth identity binding: PASS / FAIL
Manual click / text / drag / focus return: PASS / FAIL
Button 2 -> Home: PASS / FAIL
Agent frame observation: PASS / FAIL
Agent click / text / drag / Home / focus return: PASS / FAIL
Optional paste / wheel: PASS / FAIL / NOT TESTED
Blocker or remaining limitation:
```

Use **ready for agent iPhone control** only when the intended phone, image, binding, input and agent-operated tests pass. If Home or another required item fails, report the working capabilities and the exact remaining blocker. Earlier project results do not qualify this user's setup.

Tell the user the full executable path and how to resume: start that receiver with `--frames`, unlock the phone, reconnect Screen Mirroring if needed and wait for the saved Bluetooth phone to reconnect. Rediscover the PID and obtain a fresh image before agent input. Do not reopen pairing or forget the bond on every launch.

For ordinary use, clicking away returns control to the PC. Disconnect or closing the receiver ends its Bluetooth control service. A later session still needs a current observation; never replay actions left over from an earlier connection.

## If a checkpoint fails

| Failure | Next action |
|---|---|
| Cannot clone or download | Resolve account/repository access or obtain a maintainer-supplied package |
| Build cannot find CMake/Ninja | Verify installation and refresh the shell's PATH |
| MSVC/SDK missing | Modify the C++ workload in Visual Studio Installer |
| BLE peripheral role unavailable | Resolve adapter/driver support; ordinary Bluetooth pairing is not enough |
| Receiver absent from Screen Mirroring | Check network reachability, interface changes and the executable's firewall allowance |
| PC absent from Bluetooth discovery | Confirm Pair Bluetooth is active within its 120-second window |
| Phone says Connected, selector empty | Inspect subscriptions/status; use the main Bluetooth settings pairing route |
| Ready keeps changing to disconnected | Stop sending input; resolve connection stability before another action |
| Clicks miss | Recalculate actual picture bounds, DPI scaling and overlay positions |
| Typing goes elsewhere or stops | Restore the intended text field and video focus; inspect status before sending more |
| Home opens a PC menu or does nothing | Use the toolbar, register Button 2 on the correct accessory and verify the assignment |
| Frame stale, inactive or wrong PID | Recheck receiver identity and live mirroring; do not reuse the previous PNG |
| Agent can see but cannot click | Supply an authorized native desktop-input tool, or finish as user-assisted setup |

Further detail: [native control](app/NATIVE-CONTROL.md), [agent operation](app/CLAUDE-CONTROL.md), [frame protocol](app/FRAME-INTERFACE.md), [build guide](app/README.md).
