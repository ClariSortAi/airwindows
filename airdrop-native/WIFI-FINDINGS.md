# MT7922 Wi-Fi findings and security review

Updated: 2026-10-28. Scope: this PC's installed MediaTek driver, not all MT7922
drivers. This is a local engineering record, not a published vulnerability claim.

Operational constraint: preserve Ethernet. The user has now ended the Zoom
meeting, removing the meeting-specific restriction on Wi-Fi testing. No Ethernet
or network-stack changes have been made. The replacement driver remains unsigned
and uninstalled; its current MAC work has only run in simulation.

## Exact subject

* Adapter: MT7922/RZ616 PCIe, `14c3:0616`, subsystem `223c:14c3`, revision 02.
* Driver: `mtkwl6ex.sys`, service `mtkwlex`, version `3.5.0.1383`, INF `oem49.inf`.
* SHA-256: `20112897f39752f1877ccbab9ca9b1fe6e5d554fe662b6d50973aa19f3c3d34a`.
* OS: Windows 11 Home build 26200. Secure Boot remains enabled.
* Analysis: Ghidra 12.1.4 through REA 6.7.0; static pseudocode is inferred code,
  not original source or proof that every branch executes on this adapter.

## Interesting findings

| Finding | Evidence and limit |
|---|---|
| Public capability reporting understates internal functionality | `netsh` reports monitor mode unsupported. A bounded live test nevertheless started private sniffer mode on channel 6 and received raw frames through the installed signed driver. |
| A usable vendor control endpoint exists | `\\.\MTKWLAN` opens successfully with read/write access in our unelevated process. Opening alone does not prove all operations work. |
| A real private query succeeds | The reviewed firmware query returned 100 bytes and firmware date strings `20260302163458` and `20260302163403`. No radio mode was changed. |
| Sniffer entry point is substantive | `FuncSniffer` accepts an exact 28-byte structure; its start branch checks that all ports are idle before issuing a firmware monitor command. It has stop and counter-query branches. |
| Raw RX has a working Windows tracing route | A native C++ ETW consumer received WPP record headers, radiotap metadata and 289 packet-blob events during the bounded capture. Only counts were retained. |
| A misleading TX lead was narrowed | `WdiInjectDatapathFrame` is a WDI callback path. Its name alone does not establish arbitrary application-controlled frame transmission. |
| The DLL finding was narrow | The IHV DLL's public control callback is a stub. The kernel device has a separate, functional control path. |
| MT7925 transmit code cannot be substituted unchanged | Connac2 uses different header, multicast, rate and completion fields. An MT7922 encoder and queue core now pass offline byte-format and ownership tests; they have not been connected to hardware. See [source and test boundaries](driver/mt7922/SOURCES.md). |
| Firmware replies also differ between generations | The MT7922/Connac2 response header is 36 bytes, versus 44 bytes in the MT7925 reference. Patch and unified status offsets differ. The native core now checks these formats offline; no hardware command-response validation has passed. |

The successful query and live RX change the next step: investigate stock-driver
transmission before investing further in a replacement driver. AirDrop still
requires raw TX, adequate timing and AWDL/iOS interoperability. Those are not
proven by a successful capture.

### Bounded live receive proof

Evidence: capture (local-only evidence: `artifacts/stock-live-20261028-104841/capture.jsonl`) and
restoration report (local-only evidence: `artifacts/stock-live-20261028-104841/result.json`).

* Elevated ETW start and provider enable succeeded. The unelevated attempt failed
  with Windows error 5; access to the vendor device does not confer ETW privileges.
* With Wi-Fi temporarily disconnected, sniffer start on 2.4 GHz channel 6 succeeded.
* Five-second driver counter sample: 31 data, 100 management, 157 control frames.
* ETW delivered 289 packet blobs. The first consumer counted only frames with at
  least 24 bytes as `raw_frames` (132), excluding short control frames. Its source
  has since been corrected to count valid short frames too; the original evidence
  is preserved. Driver counters were sampled before stop; ETW totals cover the
  full trace. The exact cause of their one-packet difference is unverified.
* No AWDL action was recognized in this short sample. No packet payloads were saved.
* Sniffer stop succeeded, the original Wi-Fi profile reconnected, and Ethernet
  remained Up. No replacement driver, reboot or security-setting change was used.
* The PowerShell wrapper's `captureExitCode` was null due to process-handle
  bookkeeping. The capture's own operation results and explicit restoration checks
  provide the evidence; null is not a successful-exit assertion. The wrapper has
  since been corrected and has not been rerun during the user's meeting.

Npcap 1.86 is also installed. A metadata-only probe reported `can_set_rfmon=0`
and datalink types 1/143 (Ethernet/DOCSIS), not native 802.11. No frames were read
or sent in that probe. Npcap's generic packet-send API is not evidence of working
raw wireless TX; see [Npcap's open raw-Wi-Fi injection issue](https://github.com/nmap/npcap/issues/85).

### Transmission-path findings

The recovered WDI dispatcher separates 20 task-table entries (including a dummy)
from 29 property-table entries. The task table contains
`OID_WDI_TASK_SEND_REQUEST_ACTION_FRAME` (`0xe4400066`) and its response counterpart.
Neither table contains `OID_WDI_IHV_REQUEST`. The inspected property dispatch
returns failure when an OID is absent; the subsequent fallback is also a stub.
Thus, a literal `OID_WDI_IHV_REQUEST` string elsewhere in the binary does not
demonstrate an implemented vendor request handler.

Microsoft documents [WDI IHV requests](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-wdi-ihv-request)
as the forwarding mechanism for an IHV extensibility module. Its existence in
the Windows API does not supply the missing handler in this driver's reviewed
dispatch. The action-frame task remains a separate candidate, but application
access and arbitrary AWDL data-frame transmission have not been established.

Evidence: decoded tables (local-only evidence: `artifacts/wdi-tables-decoded.json`), retaining the
source Evidence IDs and exact driver digest; REA results `273`–`280` in
`artifacts/kernel-session`. [The decoder](tools/decode-wdi-tables.py) checks the
digest, addresses and complete table lengths before interpreting the entries.

### Firmware format and analysis limits

The installed `WIFI_RAM_CODE_MT7922_1.bin` has five regions whose feature flags
all include the encryption bit defined by the pinned mt76 Connac2 format. Its
build-date field is `20260302163458`, matching the earlier live firmware query.
The region lengths leave 120 bytes before the metadata table unexplained by this
parser; their purpose and the trailer CRC have not been validated.

This makes plain string search or direct disassembly of those payload regions
an unsuitable way to rule out firmware TX support. No firmware was decrypted,
modified or loaded. Encryption flags alone are not a security defect.
Evidence: region map (local-only evidence: `artifacts/firmware-region-map.json`), reproduced by
[the file-only inspector](tools/inspect-connac2-firmware.py).

### Offline receive-parser validation

The revised diagnostic compiles with `/W4 /WX` and passes 13 synthetic event
cases, including short ACKs, AWDL signatures, unrelated event IDs and malformed
lengths. `--self-test` branches before device construction and performs no WLAN,
ETW or device calls. See build receipt (local-only evidence: `artifacts/stock-probe-04/build-report.json`)
and self-test result (local-only evidence: `artifacts/stock-probe-04/self-test.json`). This validates
parser behavior only; the new binary has not been used for a live capture.

### Connac2 register and DMA differences

The pinned mt76 source places the MT7922 command queue on TX ring 17, firmware
download on TX16, and receive/event/completion traffic on RX2/RX0/RX4. The
MT7925 reference uses a different command ring. MT7922 also uses an L1 remap
selector at BAR offset `0xfe24c` with a window at `0x40000` for certain physical
registers. PCIe ownership uses a different status bit from MCU ownership.
These differences make an unchanged MT7925 driver unsuitable for this adapter.

The native core now implements these mappings and startup/shutdown sequences,
plus command/download queues and bounded receive-record assembly. Register
access restores the previous remap selector. Shutdown retains DMA allocations
unless disable and idle status are verified. Tests cover failures that occur
after a write may already have reached the device, dropped/oversized receive
records, ring wrap and malformed descriptors.

Evidence: [source provenance and limits](driver/mt7922/SOURCES.md) and
offline build/test receipt (local-only evidence: `artifacts/mt7922-core-08/test-report.json`). These
are source-derived porting findings tested against a simulation. No register
was accessed on the real adapter, no new driver was loaded, and no AirDrop
discovery or transfer was established by this work. These implementation
differences are not evidence of a vulnerability in the installed driver.

The integrated transport test (local-only evidence: `artifacts/mt7922-core-10/test-report.json`)
subsequently exercised firmware loading through the native queue and reply
decoder code against simulated registers/DMA: 230 commands, 889,744 payload
bytes checked in order, and 10 matched replies. ROM and RAM readiness remain
separate from a firmware acknowledgment. An injected early acknowledgment with
missing DMA completion leaves the command buffer owned and causes a bounded
drain timeout. Stale and unsolicited replies cannot satisfy an awaited command.
This is an implementation check, not evidence of live hardware or AirDrop operation.

Two further source-level differences are now handled by the native core:
capability TLVs use 32-bit type/payload-length fields rather than MT7925's
16-bit fields, and normal RX records begin with a 24-byte descriptor rather
than 32 bytes. Connac2 optional receive groups also have different lengths.
The capability/receive test receipt (local-only evidence: `artifacts/mt7922-core-13/test-report.json`)
covers the fixed capability query, bounded parsing and conversion of synthetic
DMA records into AWDL radio-link RX messages. Capability queries were not sent
to the real adapter. MAC/band values in these tests are synthetic; they neither
establish live capability reporting nor authorize channel/power settings.

The core and Windows resource module now also pass
native kernel compilation/linking (local-only evidence: `artifacts/mt7922-kernel-02/build-report.json`).
The module includes exact PCI identity checks, 32-bit DMA span validation and
firmware SHA-256 pins checked before register access. It is linked into an
unsigned identity-only harness, with the new hardware callbacks still unwired.
No driver was installed and no new MMIO, DMA or firmware operation was executed.
These checks are implementation safeguards, not evidence of live radio operation
or a newly confirmed vendor vulnerability.

The subsequent wired bring-up build (local-only evidence: `artifacts/mt7922-kernel-05/build-report.json`)
adds an exclusive administrator/SYSTEM device interface with fixed status,
pinned-firmware boot and stop requests. It exposes no arbitrary register or
firmware-command forwarding. Status handles do not disarm prepared queues;
boot-owned handle cleanup attempts DMA shutdown. Request sizes are bounded and
the status buffer is fully initialized before return. These safeguards have
compiled and their portable buffer parser has passed offline tests, but live
driver access control/lifetime behavior is unverified. A read-only client open
returned error 2: the new driver is not installed. See [interface details](DRIVER-BRINGUP.md).

## Security concerns

### S1 — Unelevated access to a privileged vendor control surface

**Confirmed:** this process is authenticated, is not SYSTEM, and does not have
the Administrators role enabled. It opened the vendor device with generic
read/write access and successfully queried firmware information. This proves
access from this token. A subsequent read of the live device security descriptor
succeeded and returned:

```text
O:BAG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x12019f;;;BU)(A;;0x12019f;;;WD)
```

The DACL grants SYSTEM and Administrators full access, and grants Built-in Users
(`BU`) and Everyone (`WD`) the file generic read/write mask `0x12019f`. This is
stronger evidence than the successful handle open alone. It does not establish
that every command passes its subsequent state or firmware checks.

**Static observation:** the device dispatch function routes the query IOCTL into
vendor command tables. Inspected handlers include radio/sniffer changes,
firmware chip commands and configuration writes. The common dispatch and
sniffer paths inspected so far show adapter-state checks, without an explicit
caller-privilege check. Additional policy in unreviewed paths remains unknown.

**Concern:** locally running software may be able to change radio behavior or
invoke firmware diagnostics without elevation. Depending on which operations
actually succeed, this could affect network availability or capture privacy.
ETW capture permissions are a separate boundary: the unelevated attempt failed,
while the elevated capture succeeded. Unelevated packet capture is not proven.

**Not established:** privilege escalation, arbitrary kernel execution, arbitrary
kernel memory access, remote exploitation, or successful unauthorized capture.
No severity score or CVE claim is assigned.

### S2 — Input-length validation deserves review

**Static observation:** the device dispatcher copies a four-byte vendor selector
from the IRP system buffer before any input-length check visible in its recovered
pseudocode. Several downstream handlers enforce exact sizes; that does not
establish validation of the outer selector buffer.

**Concern:** malformed requests may expose a kernel reliability or memory-safety
defect. Assembly, framework guarantees and call-site validation need checking
before making that claim. No malformed requests, fuzzing, crash tests or exploit
attempts have been sent to the live driver.

### S3 — Potential peer encryption-key disclosure

**Static observation:** a vendor-table handler named `FuncGetTK` looks up a
port and peer, reads a stored key length and key bytes, formats them as a
`KeyLen`/`Key` string, and copies that string into its caller's output buffer.
Its inspected path checks port/association state and buffer size. No explicit
caller-privilege check appears in that handler or the reviewed common dispatch.

**Concern:** combined with the permissive live device DACL, this warrants review
as a possible local disclosure of peer traffic-encryption keys. Which port roles,
cipher suites and current connection states make this path usable still need
static validation. It is not evidence that the Wi-Fi password can be recovered,
or that keys from this connection have been exposed.

**Boundary:** this handler has not been called. No live encryption keys were
requested, displayed or stored. Testing key extraction is unnecessary for the
AirDrop work and is excluded from the diagnostic tools.

Evidence: `artifacts/kernel-session/154-command.result.json`, supported by the
command-table and live-DACL evidence above. Treat this as an unconfirmed security
issue pending review, not a demonstrated exploit.

### Common MAC initialization differs from MT7925

Source comparison found three further incompatibilities: MT7922's MDP registers
map to BAR `0xf000` rather than `0xe800`; its WTBL update register is `0x34230`
with a ten-bit index and bit-12 admission-counter clear, rather than `0x34380`
with a twelve-bit index and bit-14 clear; calibration selection uses extended
command `0x21ed` with four payload bytes, rather than the MT7925 unified command.
An unchanged MT7925 MAC initializer would address different registers/fields.

The native implementation now selects normal firmware mode before download,
uses the MT7922 common MAC sequence, disables Ethernet-header translation for
raw 802.11, and bounds station-table busy waits to five milliseconds. It does
not configure transmit station entries or a permitted channel/power policy.
The calibration selection uses the device's existing EFUSE data; it does not
extract or program EFUSE. This is a porting finding, not a vulnerability.

Evidence: [pinned upstream initialization](https://github.com/openwrt/mt76/blob/467ae2e3f190baf737e15e318906da0dc7d0ab91/mt7921/init.c),
[source provenance](driver/mt7922/SOURCES.md),
core tests 17 (local-only evidence: `artifacts/mt7922-core-17/test-report.json`) and
kernel build 06 (local-only evidence: `artifacts/mt7922-kernel-06/build-report.json`).
The tests inject failures at all 119 register accesses in the simulated success
path. Neither these results nor the successful native build establishes live
radio behavior. No new security issue was demonstrated by this stage.

### Firmware packaging changes the regulatory path

The installed Windows image has no separate non-download CLC region. The pinned
mt76 checkout's older 2023 image does. A separately acquired upstream MT7922
bundle (`20260724143402`) contains a 475,488-byte CLC region with two power
variants and two regulatory blocks. It remains an offline reference; no
firmware pin, installed driver or adapter binding was changed.

The MT7921 loader keeps the first index-3 regulatory block; unlike the MT7925
reference, it does not select that block using a type field at byte six. The
native encoder follows that source behavior and preserves country rule/signature
bytes. Tests forwarded 241 nonempty records byte-exact and rejected nine empty
records. Empty records do not justify inventing unrestricted defaults and are
not themselves evidence of a security defect or a statement of national law.

The stock Windows image's missing region does not mean the signed Windows
driver lacks regulatory controls: its separate `.dat` files and internal policy
have not been decoded here. The formats are not interchangeable merely because
both firmware bundles target MT7922.

Evidence: [firmware provenance, parser and limits](FIRMWARE-REFERENCE.md),
core tests 19 (local-only evidence: `artifacts/mt7922-core-19/test-report.json`), and
kernel build 07 (local-only evidence: `artifacts/mt7922-kernel-07/build-report.json`). Regulatory replies
in these tests are synthetic. Firmware validation, actual channel eligibility
and RF output remain unproven. No new security vulnerability was demonstrated.

### Power records depend on hardware type and platform country settings

MT7922 CLC uses flat records and a 76-byte request header, unlike the segmented
MT7925 reference format. The evaluated bundle has two index-0 variants selected
by EFUSE byte `0x55b`, bit 0. A country can have multiple records: US has seven
per variant, and world (`00`) has three. Sending only the first would omit data.
Upstream also recomputes the country-dependent MTCL platform setting when it
falls back to world/indoor; retaining the original country's setting would not
match that behavior.

The native core now implements bounded delivery, all-record preflight and a
fixed read-only hardware-type query. It rejects unknown ACPI acquisition state
and records whether firmware supplied channel-configuration information. A DMA
completion without a response is delivery evidence, not confirmation of channel
eligibility. This code has not queried EFUSE or applied CLC on the actual device.

Evidence: [pinned MT7921 CLC implementation](https://github.com/openwrt/mt76/blob/467ae2e3f190baf737e15e318906da0dc7d0ab91/mt7921/mcu.c),
[platform MTCL selection](https://github.com/openwrt/mt76/blob/467ae2e3f190baf737e15e318906da0dc7d0ab91/mt792x_acpi_sar.c),
core tests 22 (local-only evidence: `artifacts/mt7922-core-22/test-report.json`) and
kernel build 09 (local-only evidence: `artifacts/mt7922-kernel-09/build-report.json`).
This is a compatibility finding, not a newly demonstrated vulnerability.

### Platform power tables require independent layout and status checks

The pinned MT792x source handles OEM SAR tables whose actual layout differs
from the version advertised by MTCL. MTDS and MTGS therefore need their own
length/count validation. Country policy is separate: MTCL contains country
bitmaps, while MTFG contains flag indices that must be translated into firmware
bits. Blindly copying these bytes would not preserve their meaning.

The native Windows path now evaluates only the four fixed OEM methods before
radio MMIO. It rejects failed/unsupported evaluations, partial buffers, invalid
argument types, out-of-range integers and flag counts beyond the supplied data.
Only an explicit name-not-found response with no data is treated as absence.
An absent table and a failed table read are distinct states. No new generic ACPI
interface or external table override is exposed.

Evidence: [source mapping and tests](driver/mt7922/SOURCES.md#platform-acpi-and-sar-policy),
core tests 23 (local-only evidence: `artifacts/mt7922-core-23/test-report.json`) and
kernel build 10 (local-only evidence: `artifacts/mt7922-kernel-10/build-report.json`).
The responses are synthetic; the adapter's actual OEM tables are unknown.
These checks are defensive porting requirements, not a demonstrated stock-driver
or firmware vulnerability.

### Channel commands mix masks, counts and distinct formats

MT7922 RX-path setup and channel switching share a 76-byte command payload, but
the receive-stream byte has different meanings: an antenna bitmask for RX-path
setup, a stream count for channel switching. With two streams, those values are
`3` and `2`. Its channel domain and RX filter also use CE commands, whereas the
MT7925 reference uses different unified-command layouts. Sniffer setup still
uses a unified command. Copying only the payload shape would miss these differences.

The native controller now checks those exact bytes, requires command-DMA
completion even for setters without replies, and prevents channel changes while
raw TX is queued or firmware-owned. Listening eligibility remains separate from
transmit eligibility. Unknown CLC channel flags exclude UNII-4; DFS, no-IR, PSD
without a derived cap and unknown regulatory flags cannot authorize raw TX.

Evidence: [source mapping and limits](driver/mt7922/SOURCES.md#mt7922-channel-and-monitor-control),
core tests 34 (local-only evidence: `artifacts/mt7922-core-34/test-report.json`) and
kernel build 17 (local-only evidence: `artifacts/mt7922-kernel-17/build-report.json`).
All new channel operations ran against simulated registers/DMA only. This is a
porting finding, not a newly demonstrated vulnerability or proof of RF behavior.

### Unified requests do not all have unified status replies

The pinned MT7921 response parser treats DEV/BSS as UNI status replies but
SNIFFER as a sequence-matched, full-header ACK. The request's UNI bit alone is
insufficient to choose its reply format. Core 24/25 used the wrong sniffer status
assumption; Core 26 onward corrects it. Historical receipts are retained.
This is a corrected porting assumption, not a discovered stock-driver defect.

### Power-table delivery needs firmware pacing

Connac2 SKU delivery uses a 44-byte header and 162 bytes per channel, in batches
of eight. All three bands total 171 channel records in 22 batches. The pinned
source reads a fixed PSE register after each batch to avoid buffer underflow.
That response can contain a high-bit unsigned value; treating it as a signed
error would falsely fail delivery. The new port preserves the value, verifies
the echoed address and waits for both DMA completion and the read response.

The port now combines an explicit finite cap with decoded SAR limits, preserves
reserved rate bytes and distinguishes zero power from an unlimited sentinel.
Actual calibration, country/CLC enforcement, antenna effects and radiated power
remain unverified. This does not demonstrate a firmware vulnerability.

### Monitor context precedes a usable transmit station

Upstream creates its initial DEV/BSS context before assigning the monitor
interface's reserved WCID 19. Thus the first BSS request carries WCID zero,
while removal after assignment carries 19. The new port preserves these stages
and requires real zero UNI status for both DEV and BSS, plus drained DMA.
Acknowledged context setup alone does not establish a usable transmit station.

Evidence for these additions: [source mapping](driver/mt7922/SOURCES.md),
Core 34 (local-only evidence: `artifacts/mt7922-core-34/test-report.json`) and
Kernel 17 (local-only evidence: `artifacts/mt7922-kernel-17/build-report.json`). All new operations ran
only against simulated registers and DMA. The signed stock driver remains bound.

### Fixed-rate monitor transmission has its own antenna and ownership rules

The inspected MT7921 monitor add-interface path assigns reserved WCID 19 and
clears its admission counters without sending an associated-peer station-add
command. Fixed-rate frames use an antenna index in their transmit descriptor.
For two chains, the pinned source selects SPE 24; setting Connac2's separate
`SPE_ID_IDX` bit would instead select the WTBL index and ignore that explicit
selection. The native wrapper now preserves this distinction.

The queue also has two independent completions: DMA stops reading the
descriptor, then firmware releases its payload token, or the reverse. The
integrated poll path retains payload ownership until both occur. Firmware token
release does not prove successful over-the-air delivery. MCU failure disarms
the bound raw queue, and a doorbell write error retains the potentially published
frame. These are driver-porting/lifetime requirements, not new stock-driver
vulnerability claims. Core 34 (local-only evidence: `artifacts/mt7922-core-34/test-report.json`) tests
them against modeled hardware; Kernel 17 (local-only evidence: `artifacts/mt7922-kernel-17/build-report.json`)
wires queue binding but leaves live raw TX inactive.

### Receive backpressure must preserve control and DMA ownership

The new session receives radio_link messages through a bounded nonpaged queue.
It copies each decoded frame before the borrowed DMA assembly can be reused,
retains a complete message when a caller supplies too little output space, and
drops new frames when full. Firmware command replies and TX release records
continue through the MCU path. Queue overflow therefore does not block those
control operations in the modeled tests.

Malformed on-air frames and corrupt hardware transport metadata are different
failure classes. The former is a counted receive drop; the latter stops the
session and retains DMA allocations. The first Core 31 session test confused
these cases and failed correctly. Core 34 checks both without weakening the
transport parser. Queued bytes are cleared on consumption or stop. FCS policy
is explicit; its live value remains unqualified.

Evidence: Core 34 (local-only evidence: `artifacts/mt7922-core-34/test-report.json`),
Kernel 17 (local-only evidence: `artifacts/mt7922-kernel-17/build-report.json`) and
[session contract](DRIVER-BRINGUP.md#composed-radio-session-and-bounded-receive-queue).
Windows prepares the session and output buffers but does not start RF through
the diagnostic BOOT command. No live packet capture or new driver load occurred.

### Native bridge privilege and rate boundaries

The native AWDL bridge requests legacy OFDM rates, commonly 12 Mbps; silently
forcing the original 6-Mbps wrapper would discard that request. The new adapter
translates all eight supported rates into explicit MT7922 units. Core 34 checks
the resulting Connac2 descriptors; Bridge 02 checks the complete user-mode
translation. These are source/model findings, not observed RF performance.

The reference pipe server uses default security attributes. That does not by
itself establish an exploitable DACL. Because this pipe can authorize radio
commands, the MT7922 server instead supplies a protected DACL for the process
user, administrators and SYSTEM, rejects remote clients, and uses a single
first-instance pipe. Windows tests verify the descriptor and local byte-stream
behavior. No hostile-user impersonation or privilege-escalation test was run.
The public driver interface itself remains exclusive and admin/SYSTEM-only.

[Radio ABI](RADIO-ABI.md), [bridge source basis](radio/SOURCES.md),
Bridge 02 verification (local-only evidence: `artifacts/mt7922-bridge-02/verification.json`).

### Receiver binding and approval ordering need explicit boundaries

The pinned receiver reference binds `::` even in loopback mode and accepts a
peer by scope ID alone. Global IPv6 peers normally have scope zero, as loopback
does. Static inspection therefore does not support its claim that loopback mode
is computer-local. The owned receiver now binds exactly `::1` for tests, or the
selected AWDL link-local address with its interface index for live operation,
and checks peer address class and scope. Actual listener inspection and
negative predicate tests pass. No remote probe or exposure of user files was
performed; this is a third-party user-mode receiver finding, not a stock Wi-Fi
driver vulnerability.

The reference also emits its `asked` event before creating the decision slot.
An immediate answer can consequently be lost. The adapted engine creates the
slot first; the verifier deliberately accepts/declines synchronously inside
the event callback. Malformed Ask structures now receive HTTP 400 before
approval handling. Receiver 02 (local-only evidence: `artifacts/receiver-02/verification.json`) records
32 checks and three matching file hashes through real local HTTPS.

### Native AWDL serialization reaches the MT7922 user-mode adapter

The actual native `awdld.exe`, with host-adapter creation disabled, programmed
one source address and submitted 82 frames at 12 Mbps through the production
bridge into an in-memory device model. The child exited cleanly when its pipe
closed. This confirms protocol compatibility that isolated encoders alone do
not show. No frame reached the MT7922 or the air. The AWDL build also reports
17 upstream radiotap portability warnings, including narrowing pointers to
Windows' 32-bit `unsigned long`; they remain a receive-path review item, not a
proven memory-safety flaw. [Native runtime evidence](NATIVE-RUNTIME.md).

## Potential impact

These concerns apply to the exact installed driver identified above. Confirmed
device access does not establish successful exploitation of every handler.

| Concern | Potential consequence | Evidence and uncertainty |
|---|---|---|
| **S1: Local control of radio diagnostics** | A local process might interrupt Wi-Fi connectivity or change radio behavior without elevation. If a usable receive path were also accessible, capture could expose nearby wireless traffic metadata and unencrypted contents. | Unelevated read/write device access and a firmware query are confirmed. Mode changes and raw RX were tested with elevation. Unauthorized mode changes and unelevated capture remain unproven. |
| **S2: Possible missing input-length validation** | A malformed request might crash the driver or Windows, interrupting applications and causing loss of unsaved work. A system crash would also interrupt Ethernet and Zoom even though the defect is in the Wi-Fi driver. | Static pseudocode raises the concern. No malformed requests or crash tests were run; a memory-safety defect, code execution and privilege escalation are not established. |
| **S3: Possible peer traffic-key disclosure** | If the handler is reachable for an active peer and returns usable key material, local software might compromise confidentiality of traffic protected by that key. | Static code appears to copy peer key material to the caller. Applicable roles, ciphers, state checks and actual output remain unverified. No live keys were requested. This is not evidence of Wi-Fi password recovery. |

**Threat boundary:** the demonstrated entry point is a local Windows device
interface. There is no demonstrated remote or over-the-air exploit, cross-host
compromise, persistence, or compromise of the user's iPhone/iPad. The firmware's
internal features alone do not establish a vulnerability.

**Porting consequence:** using the wrong power variant, omitting repeated country
records or guessing ACPI/MTCL policy could misconfigure transmission limits or
make the radio fail. No out-of-limit transmission has been demonstrated. The
new radio-control path requires explicit administrator RADIO_START and is not
invoked by diagnostic BOOT; operational country selection,
verification of actual platform policy and hardware qualification are still required.
Treating a failed ACPI read as an absent restriction could discard OEM SAR limits;
the implemented acquisition path stops on that failure instead. Evaluating a
fixed table method still executes OEM AML, so a cancellation timeout alone is
not proof of a strict completion bound or absence of firmware side effects.
Misinterpreting stream fields can misconfigure reception; treating a monitor
channel as transmit-authorized could bypass required channel restrictions.
Partial channel changes also make cached tuning state unreliable. The new
controller clears that state and disarms transmission on failure, but actual
tuning and power enforcement still require hardware qualification.
Skipping SKU pacing could prevent reliable power-table delivery; confusing a
register value with status could falsely reject it. Mistaking a sniffer ACK for
a status-bearing response could also stall initialization. Conflating initial
BSS context with a configured transmit WCID could make raw TX fail or use the
wrong context. These are porting risks, not demonstrated exploitation or
out-of-limit transmission on this PC.
Wrong antenna-index selection could prevent expected transmission even when
descriptor bytes otherwise appear valid. Reusing a payload after only one
completion could corrupt an outstanding frame; freeing DMA storage after a
command failure could expose unrelated memory to device access. The candidate
retains ownership/storage on those failures, but hardware lifetime and shutdown
behavior still need direct validation. No such corruption or disclosure was
demonstrated on this host.
An unbounded receive backlog could exhaust kernel memory; retaining a borrowed
DMA assembly pointer could expose overwritten or unrelated frames. Treating
ordinary malformed on-air frames as fatal could also make reception fragile.
The new bridge bounds storage and separates these paths. Flood resistance,
sustained throughput and actual RF behavior have not been established by the
offline model, and no new stock-driver vulnerability is asserted here.

**Bridge consequence:** a broadly writable control pipe could let another local
user submit privileged radio commands or interfere with a trial. The explicit
DACL narrows that boundary. This is hardening of the new bridge, not a confirmed
stock-driver vulnerability. Partial/invalid responses and uncertain I/O fail
without replay; treating queue acceptance as peer receipt would still give a
false interoperability claim.

**Receiver consequence:** wildcard binding combined with scope-only filtering
could expose a purported loopback test endpoint to unintended IPv6 peers,
subject to host routing/firewall policy. Approval is still required by the
engine, so this does not establish unauthorized file acceptance. Losing an
immediate answer can leave a legitimate transfer waiting until timeout.
The local changes address both boundaries; no remote exploit was demonstrated.

**Development-loading consequence:** the package now passes Microsoft INF
checks and unsigned catalog generation, but that does not make it loadable under
the current Secure Boot policy. Memory integrity is also running. The user has
no signing service/account. A local test-signing route would change which signed
kernel code Windows accepts and require reboot, interrupting Ethernet. That
tradeoff requires explicit approval; memory integrity must not be silently disabled.
The original 15-file stock package is preserved and its catalog members verified.
The recovery helper's device match and six guards pass, but live rollback is
unproven. System-drive encryption status could not be read without elevation,
so recovery readiness is still unknown. No security, certificate-store, encryption,
driver binding or boot configuration was changed. [Trial and evidence](HARDWARE-TRIAL.md).

**Operational consequence:** the meeting has ended, but Ethernet must remain
intact. Suspected crash paths and live-key retrieval remain outside this work.
The candidate driver has not been rebound or loaded, and boot-security settings
are unchanged. No vulnerability severity or CVE designation is assigned without
stronger evidence.

## Handling and follow-up

* Keep live tests restricted to reviewed, correctly sized requests. Do not run
  register-write, efuse, reset or malformed-buffer experiments on this machine.
* The device DACL is now recorded. Continue tracing command-specific privilege
  checks and firmware validation before assigning impact.
* Capture only the traffic needed for this AirDrop investigation; retain metadata
  and hashes where possible rather than unrelated packet contents.
* Preserve the original driver and security settings. Evaluate any confirmed
  defect for private vendor reporting; no report has been sent.

## Evidence index

* Host identity and capabilities (local-only evidence: `artifacts/host-20261028-101855.json`).
* Token context (local-only evidence: `artifacts/query-token-context.json`).
* Read/write handle tests (local-only evidence: `artifacts/mtkwlan-access.json`).
* Live device owner, group and DACL (local-only evidence: `artifacts/mtkwlan-security.json`).
* Successful firmware query (local-only evidence: `artifacts/stock-firmware-query.json`), reproduced by
  [the restricted query script](tools/mtk-stock-query.py).
* `artifacts/kernel-session/014-dispatch.result.json`: vendor device dispatcher.
* `artifacts/kernel-session/011-ioctl.result.json`: command-table dispatch.
* `artifacts/kernel-session/020-table.result.json`: complete requested table bytes.
* `artifacts/kernel-session/010-sniffer.result.json`: start/stop/query handler.
* `artifacts/kernel-session/022-sniffer-precondition.result.json`: idle-port check.
* `artifacts/kernel-session/012-monitor.result.json`: firmware monitor command.
* `artifacts/kernel-session/026-rx.result.json`: Connac2 sniffer receive path.
* `artifacts/kernel-session/015-pcap.result.json` and
  `027-pcap-emit.result.json`: WPP packet output.
* `artifacts/kernel-session/025-wifi-ver.result.json`: reviewed query implementation.

Each REA result retains its Evidence identity, target digest, analysis profile
and limitations. Raw string names are search leads; conclusions above use traced
references and recovered code, with runtime evidence identified separately.
