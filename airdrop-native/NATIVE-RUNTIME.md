# Native Windows runtime: current evidence

The radio bridge, AWDL engine and HTTPS file receiver now have native Windows
build/runtime evidence. The complete wireless path has **not** run. This PC has
not appeared in an iOS AirDrop picker and no iOS file transfer is verified.

| Component | Current artifact | What was actually exercised |
|---|---|---|
| MT7922 kernel candidate | Kernel 17 (local-only evidence: `artifacts/mt7922-kernel-17/build-report.json`) | Native compilation and modeled core tests; unsigned, uninstalled |
| Radio-link bridge | Bridge 03 (local-only evidence: `artifacts/mt7922-bridge-03/verification.json`) | 533 contract assertions, restricted Windows pipe, actual AWDL child against an in-memory device |
| Native AWDL engine | AWDL 01 (local-only evidence: `artifacts/awdl-01/verification.json`) | Ten offline test executables; native daemon drove the bridge with `-n` (no host adapter) |
| HTTPS receiver | Receiver 02 (local-only evidence: `artifacts/receiver-02/verification.json`) | 32 checks, real IPv6 loopback TLS/HTTP/CPIO, approval/decline, three file hashes, collision handling and shutdown |
| Wintun AMD64 | Source receipt (local-only evidence: `artifacts/wintun-0.14.1-01/source-receipt.json`) | Official archive/DLL hashes and valid Authenticode signature; not loaded or installed |

## Actual AWDL process test

`test_awdl_process.cpp` launches the newly built `awdld.exe -c 6 -n -r` as a
hidden child and connects it to the production bridge and MT7922 adapter class.
Only the device transport is replaced by an in-memory model. The real engine
programmed one source address, submitted 82 frames at 12 Mbps in 3.5 seconds,
and exited with code zero after the pipe closed. The model checked frame source,
request layout, rate and channel geometry. Counts are run-specific.

This proves user-mode command interoperability, including the actual OWL
serializer. It does not prove MCU acceptance, RF transmission, receive decoding,
peer synchronization or file delivery. No Wintun adapter was created. US is an
offline placeholder in this test, not a live country selection.

The AWDL source remains pinned to Airdrop-for-Windows commit
`74606871b001bdba5e447f84a43bb96124fb495a`. The initial build reports 17 warnings
in upstream radiotap: packed-member address expressions and pointer-to-32-bit
`unsigned long` casts on Windows x64. These warnings are retained in its receipt;
they are not reported as a clean-warning build or as proven vulnerabilities.
Windows adaptation sources pass their strict warning checks. Receive-path
qualification and review of these portability warnings remain necessary.

## Receiver adaptation

[Owned receiver sources](receiver/SOURCES.md) retain their MIT notices and an
original-source hash manifest. Changes bind the listener to the selected AWDL
IPv6 link-local address, or exactly `::1` for loopback tests; reject malformed
Ask structures; and register approval decisions before publishing the request
event. This last change prevents an immediate user-interface answer from being
lost before the waiting slot exists.

The loopback test used a 1,048,832-byte binary, an empty file and a Unicode-named
text file. All received hashes matched. A declined transfer and an upload without
approval wrote no files. Repeated filenames created distinct files while
preserving originals. Test keys and received files lived in temporary directories
and were removed; receipts contain synthetic filenames and hashes, not keys.

The engine's JSON event/command interface is available; a desktop approval flow
and coordinated radio/AWDL/receiver lifecycle still need integration. Native
Windows Python is used, with no WSL or pip dependencies. TLS identity creation
uses the installed OpenSSL executable.

## Toolchain and signed virtual interface

The [LLVM 21.1.8 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-21.1.8)
archive is pinned by SHA-256 and extracted locally under `.cache/llvm-21.1.8`.
Only selected native tools, their runtime headers and notices were extracted.
No installer or global PATH change ran. `tools/fetch-awdl-toolchain.py` records
the official archive and extracted-file hashes.

[Wintun 0.14.1](https://www.wintun.net/) was acquired from its official archive.
Its AMD64 DLL matches both the official archive digest and the pinned reference
DLL digest. The WireGuard signature validates. Wintun supplies a virtual IP
interface; it does not supply the missing MT7922 radio driver or signing.

## Hardware loading boundary

Fresh read-only checks found the candidate `NotSigned`, Secure Boot registry
state 1, and stock driver `oem49.inf` / `3.5.0.1383` still bound with a Microsoft
Windows Hardware Compatibility Publisher signature. No code-signing certificate
was found in the queried current-user/personal and local-machine/personal stores;
that does not rule out an external signing service or hardware token.

Microsoft's [driver-signing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-requirements--windows-vista-and-later-)
require a signed kernel driver; its [Secure Boot guidance](https://learn.microsoft.com/en-us/windows/compatibility/secured-boot-signing-requirements-for-kernel-mode-drivers)
explains why the ordinary test-signing route is unavailable with Secure Boot on.
Package 01 (local-only evidence: `artifacts/mt7922-package-01/package-report.json`) now validates the
INF and generates an unsigned catalog. The original signed driver is backed up
and its recovery helper passes a read-only device match. The user confirmed no
signing route is available. A [development-mode trial](HARDWARE-TRIAL.md) now
needs explicit approval of temporary boot-security changes and Ethernet-interrupting
reboots; live rollback remains untested. Boot security was not changed.

Remaining physical requirements include country confirmation, platform/SAR/FCS
qualification, actual firmware and DMA startup/shutdown, peer discovery and
AWDL IPv6, native iOS picker visibility, a received-file hash, and Wi-Fi recovery.
Ethernet remains protected. A kernel fault could affect the whole PC even when
the driver touches only Wi-Fi; hardware testing cannot promise zero system risk.

## Reproduce current checks

Use new output directories; these commands do not start RF:

```powershell
python .\airdrop-native\tools\verify-receiver.py .\airdrop-native\artifacts\receiver-new
python .\airdrop-native\tools\build-mt7922-bridge.py .\airdrop-native\artifacts\bridge-new --awdld .\airdrop-native\artifacts\awdl-01\awdld.exe
```

The exact AWDL compiler arguments and all source hashes are retained in its
verification receipt. `--run` on the radio executable is a separate hardware
operation and was not used for these checks.
