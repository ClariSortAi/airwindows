# Native Windows AirDrop / MT7922 investigation

Publication note, 2026-10-28: hardware testing is deferred until a maintenance
window. References marked local-only point to the retained development archive;
they are not included in GitHub. See the repository root README for checkout
setup, safe tests and the current resume point. Earlier probe evidence below is
historical; the later bring-up package has passed INF/catalog generation checks,
but remains unsigned and untested on hardware.

Target: receive a file selected through the native iOS AirDrop picker, on native
Windows, using this PC's built-in MT7922. **Not operational.** No receiver has been
added to AirWindows. The current code includes an unsigned firmware/MAC bring-up
candidate and a separate identity probe. A stock-driver diagnostic demonstrates bounded raw
RX through Windows tracing. A [Connac2 radio core](driver/mt7922/SOURCES.md)
now implements transmit descriptors, queue ownership, firmware-command encoding,
boot sequencing, chip startup/shutdown, command/receive DMA queues and their
polling MCU transport, capability queries, raw receive conversion and common MAC
initialization, a typed regulatory exchange, CLC power-table delivery, platform
ACPI/SAR policy decoding, channel/monitor control, bounded SKU power delivery
initial monitor device/BSS setup, gated raw-TX submission/completion, a composed
radio session and bounded receive-message queue with
passing offline tests (local-only evidence: `artifacts/mt7922-core-34/test-report.json`).
Integrated boot tests use simulated registers and DMA memory with this machine's
firmware files. A [separate upstream firmware evaluation](FIRMWARE-REFERENCE.md)
supplies CLC/regulatory records absent from the Windows image. Its exact pair is
now pinned for explicit RADIO_START; it has not been loaded on this adapter.
The wired kernel candidate (local-only evidence: `artifacts/mt7922-kernel-17/build-report.json`)
connects the core to Windows MMIO/DMA resources, PnP and an administrator-only
firmware/MAC diagnostic interface. Preparation binds raw TX to completion polling and prepares a bounded receive
bridge/session, leaving both inactive.
Regulatory, CLC, channel, SKU, monitor-context and raw-TX activation APIs are
joined by the session API exposed through explicit administrator-only
[RADIO_START and runtime IOCTLs](RADIO-ABI.md). Diagnostic BOOT keeps its
stock-image pins and does not start that session. BOOT now acquires and validates four fixed OEM ACPI power
tables before MMIO; failures stop bring-up. Actual platform data, monitor WCID behavior,
channel/power application and radio behavior remain unverified or unfinished.
It remains unsigned and has not run on hardware;
see [bring-up behavior and limits](DRIVER-BRINGUP.md).
No driver has been installed or rebound and no boot-security
setting has been changed.

A [native user-mode AWDL radio bridge](radio/SOURCES.md) now builds and passes
533 contract checks plus Windows pipe tests. It has not opened the adapter.
The bridge is a radio backend. The native AWDL executable now drives it against
a modeled device, and the adapted receiver passes real loopback HTTPS transfers
with matching hashes. [Current native runtime evidence](NATIVE-RUNTIME.md)
distinguishes these results from outstanding hardware and iOS acceptance.

The [validated driver package and recovery plan](HARDWARE-TRIAL.md) are prepared.
The user has no signing service/account. Development-mode hardware testing needs
explicit approval because it changes boot security and requires reboots.

Novel behavior and security concerns are tracked in
[WIFI-FINDINGS.md](WIFI-FINDINGS.md), with evidence and confidence boundaries.

## Verified on this host, 2026-10-28

| Item | Evidence |
|---|---|
| Adapter | MediaTek MT7922/RZ616, `14c3:0616`, subsystem `223c:14c3`, revision 02 |
| Windows | Windows 11 Home, build 26200 |
| Vendor driver | `oem49.inf`, version `3.5.0.1383`, service `mtkwlex` |
| Stock wireless capabilities | Network monitor and promiscuous modes unsupported; Wi-Fi Direct and action frames supported |
| Boot policy | Secure Boot registry reports enabled |
| Native build | x64 KMDF 1.33 identity probe compiled and linked with warnings treated as errors |
| Runtime | Identity-probe device absent, as expected. Stock-driver sniffer RX passed a bounded test; AirDrop remains unverified. |

Host evidence: host-20261028-101855.json (local-only evidence: `artifacts/host-20261028-101855.json`).
Build evidence: build-report.json (local-only evidence: `artifacts/probe-20261028-02/build-report.json`).
For this earlier identity probe, the `.sys` is unsigned. Its INF is a candidate, with no generated catalog or
installation validation. Binding it would replace the normal Wi-Fi driver with
an identity-only device, losing Wi-Fi functionality. There is no reason to do
that for file reception in its present state.

## Stock-driver interface investigation

Static analysis of `C:\Windows\System32\mtkihvx.dll`:

* SHA-256: `7878446886a4fb457dd7cec52e5b094fcb7cbe86ad05a85b89e4ded5edb270ef`.
* `Dot11ExtIhvInitService` at `0x180003a10` calls the handler-table initializer
  at `0x1800085a0`.
* The Windows SDK `wlanihv.h` defines `Dot11ExtIhvControl` as table slot 18.
  This binary assigns slot 18 to `0x1800038a0`.
* That handler only conditionally emits a trace and returns zero. It does not
  process an input buffer or invoke radio control. This particular IHV control
  route therefore supplies no raw-frame interface.

Evidence: initializer decompilation (local-only evidence: `artifacts/mtkihvx-control.txt`) and
control-handler decompilation (local-only evidence: `artifacts/mtkihvx-handler.txt`). The DLL was
analyzed as a file, never loaded or called. This does not prove that every
possible private interface in the kernel driver is unusable.

The existing Ghidra 11.4.2 installation performed the DLL analysis above;
those are ordinary Ghidra artifacts, not a completed REA analysis receipt.
`tools/InspectMtkIhv.java` reproduces the bounded decompilation and accepts an
output path followed by optional function addresses.

### Kernel-driver investigation

Ghidra 12.1.4 was subsequently installed alongside 11.4.2 at
`C:\Dev\ghidra-dist\ghidra_12.1.4_PUBLIC`. Its official release archive SHA-256
matched `ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db`.
REA 6.7.0's scoped Ghidra health check now passes. This uses a local CLI/MCP
subprocess, without changing the user's agent registrations.

The initial REA kernel overview (local-only evidence: `artifacts/mtkwl6ex-rea-overview.json`), Evidence
`ev_09b200e44d4f02ec6cf120187f404a7eeff9b13289406644f291473ad046a5fd`, identifies
3,290 procedures and 3,606 defined strings. Kernel-driver SHA-256:
`20112897f39752f1877ccbab9ca9b1fe6e5d554fe662b6d50973aa19f3c3d34a`.

Literal names include `FuncSniffer`, `MtCmdSetSnifferMode`,
`AsicConnac2xPciProcessRxSnifferPacket`, `WdiInjectDatapathFrame` and
`NdisCommonProcessIOCTL`. These are leads, not proof of user-accessible raw RX/TX.
Opening `\\.\MTKWLAN` succeeded for zero, read, and read/write requested access;
the handles were immediately closed without sending any IOCTL. See
handle evidence (local-only evidence: `artifacts/mtkwlan-access.json`). Static tracing is in progress
to determine whether the signed stock driver supplies a usable backend.

A subsequent, correctly sized firmware-version query through this endpoint
succeeded from an unelevated process: query result (local-only evidence: `artifacts/stock-firmware-query.json`).
It did not change radio mode. A later elevated, five-second test demonstrated
raw RX through a native ETW consumer, then stopped sniffer mode and restored
Wi-Fi. See [the live evidence and limitations](WIFI-FINDINGS.md#bounded-live-receive-proof).
Raw TX remains unresolved. Ethernet must remain untouched during further work.

## Source basis

Reference checkouts retain their upstream licensing and have not been modified:

| Reference | Pinned revision | Use and limit |
|---|---|---|
| [Airdrop-for-Windows](https://github.com/Dy1an-Zou/Airdrop-for-Windows) | `74606871b001bdba5e447f84a43bb96124fb495a` | Native Windows MT7925 implementation and kit acquisition helper; not MT7922 hardware support |
| [mt76](https://github.com/openwrt/mt76) | `467ae2e3f190baf737e15e318906da0dc7d0ab91` | MT7922 register, firmware and Connac2 transport reference; Linux source, not a Windows binary |

MT7922 is handled in mt76's `mt7921` family with Connac2 descriptors. The
reference MT7925 driver uses Connac3. Changing a PCI ID would not port the
firmware, descriptors, DMA initialization or radio-control protocol.

## Reproduce the diagnostics

From the repository root (historical probe commands, not required for publication):

```powershell
.\airdrop-native\capture-host.ps1
python .\airdrop-native\build-probe.py --msvc 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133' --output .\airdrop-native\artifacts\probe-new
python .\airdrop-native\query-probe.py
```

Choose a new output folder for each build. The build downloads pinned Microsoft
WDK/SDK packages into temporary storage and records their hashes and compiler
output. It does not install, sign or load anything. The query only opens an
already-present diagnostic device; failure with error 2 is expected today.

The probe reads PCI identity through `BUS_INTERFACE_STANDARD.GetBusData`,
restricts its device to administrators and SYSTEM, and accepts only the identity
IOCTL. It performs no MMIO access, DMA, firmware upload or transmission.

## Remaining implementation and acceptance

1. Establish an MT7922 native radio backend: supported vendor interface if one
   can be found, otherwise a properly signed and tested Connac2 driver port.
   Stock-driver raw RX has passed a bounded diagnostic. Raw TX and a sustained
   radio backend remain missing. The Connac2 descriptor/queue core passes
   hardware-free tests but has not transmitted. Firmware loading and DMA lifecycle
   now have offline core implementations, as do capability and receive decoding.
   The Windows resource module and callback/request wiring compile in kernel mode.
   Signing, hardware validation, live format verification,
   reset/power recovery, regulatory limits and channel scheduling remain required;
   the identity probe implements none of these.
2. Connect that backend to AWDL discovery, synchronization, peer traffic and
   IPv6. Wi-Fi Direct capability alone does not demonstrate AWDL compatibility.
3. Integrate AirDrop discovery and HTTPS negotiation/upload, explicit receive
   approval, bounded file handling, cancellation and destination selection.
4. Prove this PC appears in the iPhone/iPad native AirDrop picker and receives
   files with matching hashes; then verify reconnect, reboot and Wi-Fi recovery.

No physical AirDrop transfer has passed. Source availability and successful
compilation do not establish hardware compatibility or iOS interoperability.
