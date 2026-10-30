# AirDrop archive notes (2026-10-28)

Private source archive for the Windows AirPlay receiver and experimental native
AirDrop reception through the built-in MediaTek MT7922/RZ616. No WSL requirement.

**AirDrop is not operational.** The native radio core, unsigned KMDF candidate,
AWDL bridge and receiver have offline or loopback validation. The candidate has
never run on the adapter. Neither appearance in the iOS AirDrop picker nor a file
received from iOS has been demonstrated. Desktop approval UI and coordinated
runtime startup remain unfinished.

| Component | Source and instructions | State |
|---|---|---|
| AirPlay desktop receiver | [app](../app/README.md) | Existing application; see its version-specific test limits |
| Native AirDrop investigation | [airdrop-native](../airdrop-native/README.md) | Experimental, hardware validation pending |
| Wi-Fi findings | [WIFI-FINDINGS.md](../airdrop-native/WIFI-FINDINGS.md) | Evidence-qualified findings and potential impact |
| Hardware trial | [HARDWARE-TRIAL.md](../airdrop-native/HARDWARE-TRIAL.md) | Deferred; no permission to reboot or interrupt networking |

## Resume point: 2026-10-28

The user has no Microsoft driver-signing account or service. The unsigned package
passed INF and catalog generation checks; that does not make it loadable under
the current Secure Boot policy. The signed stock Wi-Fi driver remains installed.
No driver replacement, firmware loading, certificate trust or boot-security change
has been performed. Publication does not authorize those actions.

When a maintenance window is available, review the hardware trial and verify the
local recovery archive and encryption recovery readiness first. The proposed
first trial is bounded stock-firmware boot/stop, not AWDL transmission. A reboot
will interrupt Ethernet. Actual operating country must be established before any
later radio trial. Keep memory integrity enabled.

## Reproduce the safe checks

Use native Windows x64, Python 3.10+, Git, OpenSSL on PATH, MSVC 14.29.30133 and
Windows SDK 10.0.19041.0. The current core/bridge scripts use those toolchain paths.
Driver builds use pinned WDK/SDK 10.0.26100.1 packages and KMDF 1.33. Other versions
have not been qualified.

From this repository root:

```powershell
python scripts/bootstrap-references.py
python airdrop-native/tools/build-mt7922-core.py airdrop-native/artifacts/core-local
python airdrop-native/tools/build-mt7922-bridge.py airdrop-native/artifacts/bridge-local
python airdrop-native/tools/verify-receiver.py airdrop-native/artifacts/receiver-local
```

Use new output directory names for each run. Core checks without `--firmware-dir`
omit the optional real firmware-image cases. Bridge checks without `--awdld` omit
the AWDL child-process integration test. Receiver verification binds only `::1`.
These commands do not install or open a driver. See [native runtime details](../airdrop-native/NATIVE-RUNTIME.md)
and [driver bring-up](../airdrop-native/DRIVER-BRINGUP.md) for the remaining builds.
The LLVM acquisition helper downloads a pinned archive of about 943 MB into a
local cache. The package validator requires a separately prepared, hash-receipted
WDK tool cache; that local cache is not included in this archive.

For AirPlay, run `app/scripts/bootstrap.ps1`, then `app/scripts/build.ps1` as
described in its README. Dependency downloads and the first build can be large.

## Archive boundaries

This repository includes owned source, tests, documentation, license notices,
pinned upstream references and a [verification summary](../provenance/VERIFICATION.md).
Upstream sources are fetched by the bootstrap scripts, not copied here.

Raw captures, device logs, driver-store backups, proprietary package contents,
firmware blobs, compiled binaries, caches, generated certificates and private keys
are excluded. Historical documents identify excluded evidence as **local-only**;
those receipts remain in the original development workspace. Their absence from
GitHub is intentional. Host-specific recovery tools retain exact device and receipt
pins, require the excluded local backup, and are not general-purpose installers.
Do not use `--restore` as a validation command.

[Source provenance](../provenance/source-export.json) records original file hashes
before publication documentation edits. The original workspace remains intact.
The publication checkout is separate, so subsequent development changes must be
made here or explicitly synchronized before another push.

Licensing is component-specific: AirPlay GPL-3.0-or-later, radio executable
GPL-2.0-only, MT7922 core BSD-3-Clause-Clear, adapted Python receiver MIT.
Consult each component's LICENSE, source headers and SOURCES/THIRD-PARTY notices.
This archive makes no claim that upstream license obligations have disappeared.
