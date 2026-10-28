# Proposed first MT7922 hardware trial

**2026-10-28 handoff:** source publication is authorized; the user deferred the
trial until a window permits reboot and network interruption. Do not execute the
sequence below as part of repository setup. Evidence referenced as local-only is
retained in the original workspace, not this GitHub source archive.

**Not authorized or executed.** The user has no Microsoft driver-signing route.
The existing instruction to preserve Ethernet remains in force. This proposal
requires an explicit exception for planned reboots and explicit approval of
temporary boot-security/certificate changes. It is not an AirDrop acceptance test.

## Concrete candidate and recovery copy

Package 01 (local-only evidence: `artifacts/mt7922-package-01/package-report.json`) contains only the
exact adapter INF, Kernel 17 SYS and an unsigned generated catalog. Microsoft
WDK 10.0.26100.1 InfVerif passes `/w` and `/h`; Inf2Cat for `10_GE_X64` reports
zero errors and warnings. These are package/signability checks, not a Microsoft
signature, WHQL certification, installation proof or hardware qualification.

| File | SHA-256 before test signing |
|---|---|
| `airwindows_mt7922_bringup.sys` | `9921d09062c9eb93d0081facbb28534113cf8a67ccc07fba1b1879c07043c0da` |
| `airwindows_mt7922_bringup.inf` | `648efd4a71e4a3982270ab02598653170c5ef7281d006aa7ff90d18892f08386` |
| `airwindows_mt7922_bringup.cat` | `34ea03b7993f99179c137d0e23e3cfa4d311d6fb081cf977d9e9e2d21c75aafc` |

The sole target hardware ID is `PCI\VEN_14C3&DEV_0616&SUBSYS_223C14C3&REV_02`.
The exact current device instance was matched in the
read-only recovery check (local-only evidence: `artifacts/stock-driver-restore-01/check-report.json`).
No generic MediaTek, PCI, network-class or Ethernet match is permitted.

All 15 files from the installed `oem49.inf` package were copied as regular files,
read back and hashed. Catalog verification (local-only evidence: `artifacts/stock-driver-restore-01/catalog-verification.json`)
passes kernel signing policy for all 14 catalog members, including the original
INF, SYS, DLL, policy data and firmware. The backup receipt (local-only evidence: `artifacts/stock-driver-restore-01/backup-receipt.json`)
pins every byte. Live rollback has not been tested.

## Current system boundary

Secure Boot registry state is 1. Device Guard reports VBS running, with service
2 configured and running (memory integrity). No signing certificate was found
in the two queried personal stores. The user confirmed no signing service/account.
The stock signed MediaTek driver is still bound. Ethernet is Up at 1 Gbps.

Reading system-drive BitLocker status failed with access denied under the current
token. Encryption/recovery readiness is **unknown**, not disabled or verified.
Before any boot-setting change, an elevated read must establish encryption
status and the user must have any required recovery key available privately.
No recovery key is requested in chat or written to project evidence. Any need
to suspend protection must be explained and separately approved before doing it.

Microsoft documents [test-signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)
as a development path: enabling/disabling it requires restart, Secure Boot can
block the change, and memory integrity requires a test-signed binary. The trial
must retain memory integrity; a new incompatibility is a reason to stop, not to
disable another protection silently.

## Proposed sequence, subject to approval

1. Recheck the exact device, package hashes, recovery copy, encryption/recovery
   readiness, boot configuration and Ethernet state. Save the non-secret baseline.
2. In a separate fresh artifact directory, create a short-lived, nonexportable
   local code-signing test certificate, test-sign this SYS, regenerate its catalog,
   and sign the catalog. Record new hashes and the certificate thumbprint. Install
   only the public test certificate into the required machine trust stores.
3. During a user-approved maintenance window, temporarily disable Secure Boot in
   firmware, enable Windows TESTSIGNING and restart as required. This interrupts
   Ethernet during reboot. No automatic reboot command is currently armed.
4. Bind only the recorded MT7922 instance to the exact test-signed package.
   Ordinary Wi-Fi networking on that adapter will be unavailable during the test.
   Leave the Ethernet device, routes, firewall and DNS configuration untouched.
5. Run the existing diagnostic client's `status` first. Stop if identity, resource
   preparation or power state is unexpected. Merely loading the driver does not
   request firmware or RF startup.
6. Run one diagnostic `boot` using the backed-up **stock** firmware pair, then
   its explicit STOP. Record boot/capability/error/shutdown results. Do not invoke
   RADIO_START, AWDL or transmit in this first trial. The code's aggregate deadline
   does not guarantee a strict completion bound inside OEM ACPI/lower-stack work.
7. Rebind the signed stock package, verify its actual driver binding and Wi-Fi
   recovery, and verify Ethernet. If a reboot is required, report it before
   scheduling it. Restore TESTSIGNING off and Secure Boot on, remove only the
   exact test certificate created for this trial, and recheck the security baseline.

A kernel fault can crash the entire PC or interrupt Ethernet despite adapter
scoping. The backup and guarded recovery helper reduce recovery uncertainty;
they do not prove the untested driver's safety or promise uninterrupted service.
If normal Windows recovery is unavailable, use Windows recovery/Safe Mode and
the preserved stock package. No system-wide driver deletion is proposed.

Later RF testing still needs the actual country, measured FCS behavior and
successful startup/shutdown evidence. Native iOS picker appearance and a
hash-matched iOS transfer remain separate acceptance requirements.

## Prepared recovery helper

From `C:\Dev\rea-run\airplay-receiver`:

```powershell
# Read-only; executed successfully already.
python .\airdrop-native\tools\restore-stock-mt7922.py

# Driver-changing recovery action; NOT executed. Requires elevation and approval.
python .\airdrop-native\tools\restore-stock-mt7922.py --restore
```

The helper pins the receipt and every backup file, rechecks kernel-policy catalog
membership and requires exactly the recorded present PCI instance. Only explicit
`--restore` invokes the Windows driver-update API. It reports a reboot requirement
without rebooting. Six mocked guard/API tests passed; default mode also matched
the actual device without changing it. The older package is deliberately forced
only during rollback, using Microsoft's
[documented device-update API](https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-updatedriverforplugandplaydevicesw).

## Decision still required

Approve a planned development-mode trial with the changes above, or retain the
current security and uninterrupted-Ethernet constraints. No signing, trust-store,
boot-policy, driver-binding, encryption or reboot change follows from preparing
this document. The user's choice and country are still pending.
