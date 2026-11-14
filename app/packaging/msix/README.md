# PhonePort MSIX package (Microsoft Store)

Agent runbook for the Store package: assets, packaging, local registration, the capability probe and submission. All commands run from the repository root in PowerShell 7, unelevated. Developer Mode must be on for `-Register`.

## Files

- `AppxManifest.template.xml`: the package manifest. Double-brace tokens are filled by `MsixTools.psm1`.
- `MsixTools.psm1`: shared helpers (SDK tool lookup, version rules, manifest fill, makepri, makeappx).
- `Assets/`: 72 generated PNGs. Do not edit by hand.
- `probe/`: capability probe (`probe.cpp`, `run-probe.ps1`). It uses this template under the test identity `GraniteAi.AirWindows.Probe`.
- `../../scripts/package-msix.ps1`: builds the layout and `.msix`, `-Register` and `-Unregister`.
- `../../scripts/make-store-assets.py`: renders `Assets/` from `app/src/app-icon.svg` and `app/src/app-icon-small.svg`.

## Build the logo assets

- When the icon drawing changes, run `python app/scripts/make-store-assets.py`. It needs Inkscape 1.x and Pillow.
- Add `--preview <file>.png` to get a contact sheet. Open a few PNGs and check that none is blank or clipped.
- `app-icon-small.svg` draws every icon of 32 px or less. `app-icon.svg` draws the larger ones.

## Package

- Build the receiver and iosctl first (`./app/scripts/build.ps1 -Target AirWindowsNative`, which writes `PhonePort.exe`). The Store and the ZIP use the same binary. Then run `./app/scripts/package-msix.ps1 -OutDir app/dist/msix-<version>` with the Partner Center values (see Submit). The default `-ExePath` is `app\build\Release\bin\PhonePort.exe`; the layout always names it `PhonePort.exe`. iosctl is taken from the same folder unless you pass `-IosctlPath`.
- `-OutDir` must not exist. The script never overwrites output.
- `-Version` defaults to the receiver's numeric `ProductVersion` as `x.y.z.0`. The script refuses a first part of 0 unless you pass `-AllowZeroMajor`, which is for local tests only. It always refuses a fourth part other than 0.
- Output: `<OutDir>\layout\` (loose package) and `<OutDir>\PhonePort-<version>-x64.msix` (unsigned).
- The layout ships the same notices, agent guides and helper scripts as `package.ps1`. It also gets `SOURCE.txt`, which says where the source of this build is published (GPL-3 section 6). By default it points to `PhonePort-<ProductVersion>-source.zip` on the GitHub release page `releases/tag/v<ProductVersion>`. With `-SourceUrl` set to a tag or tree address (for example `https://github.com/ClariSortAi/airwindows/tree/v1.0.0`, the 1.0.0 release command in `AGENTS.md`), it names that address and says that `bootstrap.ps1` and vcpkg fetch the pinned upstream sources. Publish that address before submitting the build.
- It leaves out `Start-PhonePort.cmd` and `README.txt` on purpose. Running the exe by path from the install folder gives it no package identity, and those files describe the ZIP.
- Version rule, quoted from [App package requirements](https://learn.microsoft.com/windows/apps/publish/publish-your-app/msix/app-package-requirements#version-numbering-for-windows-10-and-11-packages): "the last (fourth) section of the version number is reserved for Store use and must be left as 0 when you build your package ... The other sections must be set to an integer between 0 and 65535 (except for the first section, which cannot be 0)."

## Register locally (no certificate)

- Register: add `-Register` to the package command. This registers `<OutDir>\layout\AppxManifest.xml`. Never launch the receiver from a test registration while the user's phone is connected to another receiver.
- Check: `Get-AppxPackage -Name GraniteAi.PhonePort.Dev`, `Get-StartApps | ? AppID -like 'GraniteAi.PhonePort.Dev_*'`, `Get-Command iosctl.exe, phoneport.exe`.
- Unregister: `./app/scripts/package-msix.ps1 -Unregister` (add `-IdentityName <name>` for a non-default identity). Unregister before you delete a layout folder.
- The dev identity was `GraniteAi.AirWindows.Dev` (`CN=AirWindows Dev`) before the rename. Remove a registration left from then with `./app/scripts/package-msix.ps1 -Unregister -IdentityName GraniteAi.AirWindows.Dev`.

## Run the capability probe

- Run `./app/packaging/msix/probe/run-probe.ps1` and add `-Bluetooth` to include the GATT step. It builds, registers, runs the checks, then unregisters and removes everything it wrote. Output goes to `app\dist\msix-probe-<time>\`. Exit code 0 means no FAIL.
- The Bluetooth step runs only when no `PhonePort`, `AirWindowsNative` or `AirWindows` process exists. The probe keeps its own names (`GraniteAi.AirWindows.Probe`, `AirWindowsProbe.exe`). It uses a random 128-bit service UUID, never 0x1812.
- When any connected network is Public, the probe skips the wildcard UDP 5353 bind (`--no-wildcard`), because Windows would show a firewall prompt. TCP checks listen on loopback only.
- The probe also prints the read-only Store license (`INFO store.license...`) from the receiver slot, through the console alias and unpackaged. It calls no purchase API. `probectl.exe store` runs only that query.
- `probectl.exe bt` runs only the Bluetooth checks. Use it to compare a packaged run (through the `probectl.exe` alias) with an unpackaged one (`app\dist\msix-probe-*\build\probectl.exe bt`).

## Submit

- In Partner Center, open the app > Product management > Product identity. Copy `Package/Identity/Name` to `-IdentityName`, `Package/Identity/Publisher` (the full `CN=...` string) to `-Publisher` and `Package/Properties/PublisherDisplayName` to `-PublisherDisplayName`. Values are case-sensitive ([View app identity details](https://learn.microsoft.com/windows/apps/publish/view-app-identity-details)). PhonePort's values and the exact release command are in `AGENTS.md`, section Releases (`GraniteAi.PhonePort`, `CN=ADD31893-C52D-47EF-A312-6374403B019C`, `GraniteAi`).
- Do not `-Register` a package built with the real identity on a dev PC. Check it by unpacking it with `makeappx unpack /p <msix> /d <new folder>` and reading `AppxManifest.xml`.
- `-DisplayName` must be a name reserved for the app in Partner Center (default `PhonePort`).
- Upload `PhonePort-<version>-x64.msix` on the submission's Packages page. The Store re-signs MSIX uploads, so no certificate is needed ([App package requirements, code signing](https://learn.microsoft.com/windows/apps/publish/publish-your-app/msix/app-package-requirements#code-signing-for-microsoft-store-submissions)).
- `runFullTrust` is a restricted capability. In the submission's Submission options, explain that the app is a full-trust Win32 desktop app (an AirPlay receiver with a Bluetooth HID peripheral and a command-line helper).
- Pricing, trial and pre-release testing: see the next section.
- Run the Windows App Certification Kit before you submit. `appcert.exe` declares `requireAdministrator` and launches the app, so close the user's receiver and disconnect the phone first. From an elevated prompt in `C:\Program Files (x86)\Windows Kits\10\App Certification Kit`, run `appcert.exe reset`, register the layout, then run `appcert.exe test -packagefullname <PackageFullName> -reportoutputpath <new folder>\wack.xml` ([Windows App Certification Kit](https://learn.microsoft.com/windows/uwp/debug-test-perf/windows-app-certification-kit)). `-appxpackagepath <msix>` is the alternative, but it has to install the package, and that needs a signed package.

## Price, trial and Store licensing

- Base price: in the submission's Pricing and availability page, under Pricing, set Base price to the 29.99 USD tier. Price tiers start at 0.99 USD, and the Store converts the base price for every other market. Check the converted prices with Review price per market ([Set app pricing](https://learn.microsoft.com/windows/apps/publish/publish-your-app/msix/schedule-pricing-changes)).
- Free trial: on the same page, set Free trial to Time-limited, 7 days. The choices are 1, 7, 15 or 30 days, or Unlimited. Optional Starts on and Ends on dates control when the trial is offered ([Free trial](https://learn.microsoft.com/windows/apps/publish/publish-your-app/msix/price-and-availability#free-trial)).
- Signing and licensing: the Store re-signs the uploaded MSIX with a Microsoft certificate ([code signing](https://learn.microsoft.com/windows/apps/publish/publish-your-app/msix/app-package-requirements#code-signing-for-microsoft-store-submissions)). It also holds the license: "Every app and add-on has an associated license that indicates whether the user is entitled to use the app or add-on. If the user is entitled to use the app or add-on as a trial, the license also provides additional info about the trial" ([In-app purchases and trials](https://learn.microsoft.com/windows/uwp/monetize/in-app-purchases-and-trials#basic-concepts)). The package carries no license keys and no activation link.
- Trial expiry, quoted from Learn: "If the trial expires before the user launches the app, your app won't launch. Instead, users see a dialog box that gives them the option to purchase your app from the Store." If the trial expires while the app runs, the app decides what to do; Learn suggests a purchase prompt ([Implement a trial version of your app](https://learn.microsoft.com/windows/uwp/monetize/implement-a-trial-version-of-your-app#guidelines-for-implementing-a-trial-version)). Learn does not say whether this launch block covers a full-trust exe started by path or through an execution alias. Treat both as unverified until tested with a Store-installed trial.
- Listing: Learn does not document the button text the Store shows for a paid app with a trial. It does say "Time-limited trials are not shown to customers on Windows 10 build 10.0.10586 or earlier", and it asks you to explain in the description how the app behaves during and after the trial ([Implement a trial version](https://learn.microsoft.com/windows/uwp/monetize/implement-a-trial-version-of-your-app#guidelines-for-implementing-a-trial-version)). Tell the listing agent.
- Desktop apps: `StoreContext` dialogs (purchase) need an owner window set with `IInitializeWithWindow` (`shobjidl.h`). Without it, StoreContext "will return inaccurate data or errors". In-app purchase "is not currently supported in elevated applications" ([Using the StoreContext class with the Desktop Bridge](https://learn.microsoft.com/windows/uwp/monetize/in-app-purchases-and-trials#using-the-storecontext-class-with-the-desktop-bridge)).
- Test trial and purchase before public release: Windows.Services.Store has no license simulator. You "must publish your app to the Store and download the app to your development device to use its license for testing" ([Test your in-app purchase or trial implementation](https://learn.microsoft.com/windows/uwp/monetize/in-app-purchases-and-trials#test-your-in-app-purchase-or-trial-implementation)). Use these steps:
  - For the first submission, set Visibility > Audience to Private audience with a known user group of testers' personal Microsoft accounts. The app is then invisible to everyone else, even at its listing URL. Private-audience testers pay the set price: "if you choose a price other than Free, people in your private audience will have to pay that price". A submission published as Public audience can never go back to Private ([Choose visibility options](https://learn.microsoft.com/windows/apps/publish/publish-your-app/msix/visibility-options#private-audience)).
  - Testers install from the private listing link on the App identity page. The trial starts on install, and purchase goes through the normal Store checkout.
  - After the first publish, use a package flight to send newer packages only to a known user group. The Store listing and price stay the same for everyone ([Package flights](https://learn.microsoft.com/windows/apps/publish/package-flights)).
  - Promotional codes give the app free to a code holder ([visibility options, Stop acquisition](https://learn.microsoft.com/windows/apps/publish/publish-your-app/msix/visibility-options#discoverability)). They test install and run, not purchase.
- A local registration is not a Store install. The probe saw `IsActive=false IsTrial=true` with an empty `SkuStoreId` there (see Probe findings). Do not test trial or purchase logic against `-Register`.

## Manifest elements

| Element | Purpose | Reference |
| --- | --- | --- |
| `Identity` Name, Publisher, Version, `ProcessorArchitecture="x64"` | Partner Center identity; version rules above | [Identity](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-identity) |
| `TargetDeviceFamily Windows.Desktop` MinVersion `10.0.22000.0`, MaxVersionTested `10.0.26200.0` | Windows 11 only (Windows 10 is untested). Max is the newest build the probe and dry run ran on | [TargetDeviceFamily](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-targetdevicefamily) |
| `Application` `uap10:RuntimeBehavior="packagedClassicApp"` `uap10:TrustLevel="mediumIL"` | Full-trust desktop app. Allowed in place of `EntryPoint` because MinVersion is at least 19041 | [Application](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-application) |
| `uap10:Parameters="--frames"` on the receiver | Start menu launches pass `--frames`, as `Start-PhonePort.cmd` does. Not applied to alias launches (probe-verified) | [Application](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-application) |
| `uap:VisualElements`, `uap:DefaultTile`, `Properties/Logo` | Start entry "PhonePort", tiles, Store logo; `BackgroundColor="transparent"` | [VisualElements](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-uap-visualelements), [icon sizes](https://learn.microsoft.com/windows/apps/design/iconography/app-icon-construction) |
| `Application Id="AirWindows"` | Part of the app's user model ID, never shown. Kept from before the rename; the probe starts the app by it | [Application](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-application) |
| `uap5:AppExecutionAlias desktop4:Subsystem="windows"`, alias `phoneport.exe` | Agents start the receiver with flags (`phoneport --frames`) without knowing the versioned, ACL-protected install path | [uap5:AppExecutionAlias](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-uap5-appexecutionalias), [uap5:ExecutionAlias](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-uap5-executionalias) |
| Second `Application Id="iosctl"`, `desktop4:Subsystem="console"`, `desktop4:SupportsMultipleInstances="true"`, `AppListEntry="none"`, alias `iosctl.exe` | `iosctl` on PATH in any terminal, sharing the caller's console, with no Start entry. Console apps must declare multi-instancing | [Application, multi-instancing notes](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-application) |
| `desktop2:Extension Category="windows.firewallRules"`, `FirewallRules Executable="PhonePort.exe"`, inbound TCP and UDP, `Profile="domainAndPrivate"` | No first-run firewall prompt on private and domain networks | [Create firewall exception](https://learn.microsoft.com/windows/apps/desktop/modernize/desktop-to-uwp-extensions#create-firewall-exception-for-your-app), [desktop2:FirewallRules](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-desktop2-firewallrules), [desktop2:Rule](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-desktop2-rule) |
| `rescap:Capability Name="runFullTrust"` | Required for a mediumIL app and for `windows.firewallRules` | [App capability declarations](https://learn.microsoft.com/windows/apps/package-and-deploy/app-capability-declarations) |
| `DeviceCapability Name="bluetooth"` | Documented for GATT in `Windows.Devices.Bluetooth`. Declared for the GATT server and for Store disclosure | [Device capabilities](https://learn.microsoft.com/windows/apps/package-and-deploy/app-capability-declarations#device-capabilities) |

- No network capability is declared. Learn says `internetClient` and similar capabilities "apply only to AppContainer apps", and that a medium IL app "can already perform those operations without requiring those capabilities" ([Which kinds of apps do app capabilities apply to?](https://learn.microsoft.com/windows/apps/package-and-deploy/app-capability-declarations#which-kinds-of-apps-do-app-capabilities-apply-to)). The probe confirmed the mDNS bind, multicast join and TCP listen with no network capability. Inbound access comes from the firewall rules.

## Firewall design

- The receiver binds dynamic ports: UxPlay's HTTP server, mirror TCP, audio and timing UDP all use port 0 (`airplay_server.cpp` passes zeros to `raop_set_tcp_ports` and `raop_set_udp_ports`). mDNS uses UDP 5353 (`mdnsd_win.c`). Port rules cannot cover OS-chosen ports.
- The narrowest working set is therefore two inbound rules for TCP and UDP. Each is scoped to `PhonePort.exe`, has no port limit and covers the private and domain profiles. There are no outbound rules, because Windows allows outbound by default.
- Registration created both rules (named after the display name, "PhonePort", Inbound, Domain+Private), and unregistration removed them (measured again on 2026-11-13 after the rename). Loose registration applies them, so a Store install is not needed to test them.
- On a network that Windows categorises as Public, the rules do not apply. Windows then shows its prompt the first time the receiver listens. To narrow the rules, make the receiver bind fixed ports, then add `LocalPortMin`/`LocalPortMax` per protocol.

## Probe findings (what the package changes for the receiver)

- Identity and aliases: everything launched through the package has package identity. The console alias keeps arguments and exit codes, and stdout can be captured.
- `uap10:Parameters` is added to Start-menu launches only, not to alias launches.
- Store license (`StoreContext.GetDefault().GetAppLicenseAsync()`, read-only): a locally registered package gets `IsActive=false IsTrial=true`, an empty `SkuStoreId`, `ExpirationDate` 9999-12-31 and `IsTrialOwnedByThisUser=false`, both in the receiver slot and through the console alias. An unpackaged process gets `IsActive=true IsTrial=false` with an empty `SkuStoreId`. Neither result means "purchased": only a Store-installed package reports a real SKU.
- Named objects: no isolation for full trust. An unpackaged process finds the packaged window by class and property, sees `Local\` mutexes in both directions, posts registered messages and `WM_COPYDATA`, and opens the user-only pipe. `GetNamedPipeServerProcessId` returns the packaged process.
- Files: a new folder directly under `%LOCALAPPDATA%` is redirected to `%LOCALAPPDATA%\Packages\<PackageFamilyName>\LocalCache\Local\<folder>`, which only the package sees at the original path. Writes inside a folder that already exists outside the package go to the real folder. Existing real files are readable. Pictures and Videos writes go to the real folders.
- Registry: every HKCU write from the package, even into a key that exists outside, is private to the package and removed on uninstall ([Understanding how packaged desktop apps run](https://learn.microsoft.com/windows/msix/desktop/desktop-to-uwp-behind-the-scenes)).
- Bluetooth: `GattServiceProvider.CreateAsync` succeeds with package identity. Advertising a random 128-bit service ended `Aborted`, both packaged and unpackaged, so packaging is not the cause. The real receiver advertises 16-bit services and reaches `Started` on the same adapter. Confirm on-device with the packaged receiver.

## Known limits

- Inbound reachability from another LAN device is not tested. Local connections bypass Windows Firewall filtering.
- Windows 10 is not targeted.
- The `.msix` is x64 only. An Arm64 build would need its own package or a bundle.
- No splash screen asset is generated. Full-trust desktop apps do not show one.
