# MT7922 native radio core

Status: Connac2 core with hardware-free tests and a kernel-compiled Windows
resource module. It is not an installed driver or an operational radio backend.
The core includes firmware-boot sequencing,
register access, chip startup/shutdown, command/receive DMA queues and a polling
PCIe MCU transport connecting those queues to firmware loading, NIC capability
query/decoding, raw receive-record conversion, common MAC initialization and a
typed regulatory-domain exchange, fixed hardware-type query and CLC power-table
delivery, native ACPI table acquisition, SAR/country policy decoding and channel/
monitor control, SKU power delivery, initial monitor device/BSS setup and gated
raw-TX submission/completion, composed sessions and bounded receive delivery. Their
only executed platform is a test simulation. No actual MMIO, DMA allocation,
hardware firmware download, device binding, network API or driver IOCTL occurs
in these tests.

License: BSD-3-Clause-Clear; see [LICENSE](LICENSE). MediaTek's wire definitions
and the Windows reference's queue-lifetime approach retain their attribution.

## Pinned sources

Both reference checkouts remain unmodified.

| Source | Revision | Applied here |
|---|---|---|
| `references/mt76/mt76_connac_mac.c` | `467ae2e3f190baf737e15e318906da0dc7d0ab91` | Connac2 `mt76_connac2_mac_write_txwi`, raw 802.11 fields and hardware TXP |
| `references/mt76/mt76_connac2_mac.h` | same | Connac2 TXD bit positions, token-release record fields |
| `references/mt76/mt76_connac.h` | same | 32-byte TXD plus 32-byte hardware TXP, valid token and final-buffer flags |
| `references/mt76/mt7921/pci_mac.c` and `mt7921/mac.c` | same | MT7921/MT7922 PCIe transmit preparation and Connac2 firmware release semantics |
| `references/mt76/mac80211.c` | same | Legacy OFDM rate encodings; 6 Mbps becomes `0x4b`, not an MT7925 rate-table index |
| `references/airdrop-for-windows/driver/mt7925/raw_tx.c` and `core.c` | `74606871b001bdba5e447f84a43bb96124fb495a` | Fixed pools, descriptor reserve, serialized callbacks, and separate DMA/firmware completion ownership |
| `references/mt76/mt76_connac_mcu.c`, `mt76_connac_mcu.h`, `mt7921/mcu.c`, `mt7921/pci_mcu.c` | `467ae2e3f190baf737e15e318906da0dc7d0ab91` | Connac2 command encoding, 36-byte response header, command-specific status locations, download modes and patch semaphore |
| `references/airdrop-for-windows/driver/mt7925/firmware.c` | `74606871b001bdba5e447f84a43bb96124fb495a` | Bounded image parsing and boot sequencing adapted to MT7922 addresses; MT7925 response parser and bundle ABI excluded |
| `references/mt76/mt7921/pci.c`, `mt7921/regs.h`, `mt7921/mt7921.h`, `mt7921/pci_mcu.c`, `mt792x_regs.h`, `mt792x_core.c`, `mt792x_dma.c` | `467ae2e3f190baf737e15e318906da0dc7d0ab91` | MT7922 fixed register maps, L1 remapping, ownership, reset, DMA rings and Connac2 prefetch configuration |
| `references/mt76/dma.c`, `dma.h` | same | Command descriptor fields, two-descriptor TX reserve, RX completion/length/last-segment flags and receive recycling convention |
| `references/airdrop-for-windows/driver/mt7925/pcie_mcu.c` | `74606871b001bdba5e447f84a43bb96124fb495a` | Bounded polling and terminal fault approach; port uses its own Connac2 queues, status decoder and complete RX-record assembly |
| `references/mt76/mt7921/mcu.c`, `mt7921/mcu.h`, `mt76_connac_mcu.h` | `467ae2e3f190baf737e15e318906da0dc7d0ab91` | CE GET_NIC_CAPAB command, Connac2 capability TLVs and solicited/unsolicited routing |
| `references/mt76/mt7921/mac.c`, `mt76_connac2_mac.h`, `mt792x.h`; `references/airdrop-for-windows/driver/mt7925/raw_rx.c` | respective pinned revisions above | Connac2 receive fields/group sizes/channel encoding; radio-link framing adapted from the Windows port |
| `references/airdrop-for-windows/driver/mt7925/device.c` | `74606871b001bdba5e447f84a43bb96124fb495a` | BAR0 resource matching, KMDF common-buffer allocation, removal revocation and PnP release boundary; adapted for the exact MT7922 and new core |
| `references/mt76/mt7921/init.c`, `mt7921/mac.c`, `mt7921/regs.h`, `mt7921/mcu.c`, `mt792x_mac.c`, `mt792x_regs.h`, `mt76_connac_mcu.c`; `references/airdrop-for-windows/driver/mt7925/mac_init.c` | respective pinned revisions above | MT7922 normal-mode selection, calibration command, common MAC settings, 20 WTBL admission-counter clears and RTS thresholds; bounded waits/readback approach adapted from Windows reference, with MT7922 addresses and command format |
| `references/mt76/mt7921/regd.c`, `regd.h`, `mt7921.h`, `mcu.c`, `mt76_connac_mcu.h`; `references/airdrop-for-windows/driver/mt7925/regulatory.c` | respective pinned revisions above | First-index-3 CLC selection, 80-byte CE regulatory request and 36-byte Connac2 response; bounds/atomic-output approach adapted from Windows reference, without its Connac3 selectors or observed event constants |

## Implemented behavior

The CLC additions use the same pinned mt76 revision: `mt7921/mcu.c`
(`mt7921_mcu_read_eeprom`, `mt7921_load_clc`, `__mt7921_mcu_set_clc` and
`mt7921_mcu_set_clc`), `mt7921/mcu.h`, `mt7921/mt7921.h`, `mt792x.h` and
`mt792x_acpi_sar.c`. They use the MT7921/MT7922 flat records and 76-byte header,
not MT7925's segmented format. `clc_power.c` adds all-record preflight, strict
response bounds and a conservative nonzero-valid check for the fixed EFUSE read.
The latter check is a port addition requiring hardware qualification.

`mt2_pcie_query_hw_type` reads only block `0x550` and returns the selection bit
from byte `0x55b`; it has no arbitrary-address, write or programming interface.
`mt2_pcie_apply_clc` uses observed NIC reply capability, requires known platform
policy, forwards every selected record, and drains each command. World fallback
uses its own platform MTCL value. Partial failure disarms and retains allocations;
completion without firmware replies leaves channel configuration unknown. No
new IOCTL calls these APIs, and they do not authorize transmission.

`tx.c` serializes unencrypted, unfragmented action, plain-data and QoS-data
frames without DS translation. It uses explicit Connac2 fields: a 10-bit WCID,
header length in DWORD1 bits 15:11, multicast in DWORD2, a direct legacy OFDM
rate in DWORD6, and frame type/subtype in DWORD7. The platform must supply an
initialized WCID, OMAC and antenna mapping; the codec does not invent them.

The queue publishes only after a caller-provided barrier. Firmware release and
DMA completion may arrive in either order. Neither alone frees the payload.
Invalid notification batches fault the queue before changing token ownership.
Publication failure retains the descriptor and payload because the hardware
may already have observed the write. Disarming never asserts DMA quiescence.

Connac2 release messages contain one token per non-pair DWORD. Connac3's packed
two-token format is not reused. TX status parsing and recipient ACK evidence
are not implemented; a firmware release does not establish delivery.

`mcu.c` encodes Connac2 legacy and unified SET commands, advances the 4-bit
nonzero sequence, and keeps firmware scatter headerless for the FWDL ring.
The TX header uses `LONG_FORMAT | HDR_FORMAT_CMD` at Connac2 bit positions.
Response matching uses the 24-byte hardware prefix and 36-byte full header:
patch status at byte 32, unified command identity at byte 36 and status at byte
40. Unsolicited, stale-sequence and wrong-command responses cannot complete a
request. Unknown response semantics and unsupported UNI query combinations
are rejected. NIC capabilities now have a separate decoder below; other
variable-payload responses still require their own decoders. The fixed extended
MAC-init/channel commands (`0x21ed`, `0x3eed`, `0x46ed`, `0x4eed`, `0x08ed`) follow upstream's sequence-only
solicited ACK semantics. Their full header is required, but no undocumented
event ID or status payload is invented. Such an ACK is not RF-readiness proof.

`mac_init.c` selects on-device EFUSE calibration without reading/programming it,
sets the common receive/MAC registers, disables raw-header translation, clears
20 admission counters and sets RTS length/packet thresholds. It uses the MT7922
WTBL register/masks rather than Connac3's. Counter clears do not initialize
transmit station entries. Stable fields are read back; volatile counter enables
receive an I/O flush without a stability assertion. Polling is bounded to five
milliseconds with an iteration backstop; command DMA is drained after each ACK.
Every partial failure faults/disarms the transport and preserves DMA allocations.
`chip.c` also explicitly selects normal firmware mode before download. The wired
Windows BOOT path now requires this MAC sequence, while raw TX stays inactive.

`regulatory.c` selects the first index-3 CLC block like MT7921, validates the
complete country-record sequence and forwards the selected opaque bytes to
CE `SET_REGD_CH` (`0xd1`). It rejects missing/empty countries. The response parser
validates outer length, rule count, DFS enum and geometry; tag, TLV length and
EIRP remain raw fields because their semantics have not been observed here.
The typed PCIe query requires an observed capability reply with REGD_EN, and
only commits its output after command DMA completes. It is not called by the
current BOOT diagnostic and cannot activate TX. Firmware authentication and
country/power/channel policy remain separate requirements.

`firmware.c` validates both complete image layouts before invoking any transport
callback, then sequences restart, ROM readiness, patch semaphore, bounded
downloads, FWDL drain, RAM start and RAM readiness. It attempts semaphore release
after acquisition even if download fails, preserving the primary error separately
from release failure. A start acknowledgment alone does not set `ready`.

Layout validation does not authenticate the images. A real platform must check
a trusted manifest before calling this code. The encrypted region bytes are
passed unchanged with the established device-download flags; no decryption or
key extraction occurs on the host.

`chip.c` translates the 44 fixed register windows and the Connac2 L1 remap
window. Dynamic access preserves the old selector, including after a write
reports failure. Startup validates descriptor spans before access, obtains
ownership, resets the Wi-Fi subsystem, disables DMA, configures Connac2
prefetch/rings and enables DMA. Command traffic uses TX17; firmware download
uses TX16. The three receive rings are RX0, RX2 and RX4. Interrupts remain
masked for initial polling. `active` means DMA configuration succeeded,
not firmware or radio readiness.

Once a new DMA address might have been published, shutdown must verify both
enable and busy bits clear, then clear the ring registers before reporting
quiescence. Failure retains allocations. There is no PCI bus-reset fallback.
The caller must own the powered device and serialize all register operations;
this core must never run alongside the stock driver on the same device.

`dma.c` provides separate fixed-buffer command and firmware-download queues.
It invokes the Connac2 encoder and rejects scatter on the command queue or
ordinary commands on the download queue. DMA completion permits command-buffer
reuse but does not substitute for a matched firmware response. Failed doorbell
publication faults the queue and retains ownership.

The receive queue acquires completed descriptors, checks their addresses and
lengths, assembles records across descriptors, and rebuilds each descriptor
before publishing its recycled index. Hardware-drop and oversized records
are discarded through their final segment. Impossible lengths, unexpected
pointers and unsupported WED descriptors stop processing. The record callback
receives a complete DMA record including RXD; it is not yet an 802.11 decoder.
Callback or publication failure stops the queue without replaying a record.

`pcie_mcu.c` connects these queues to `mt2_fw_transport`: ordinary commands go
to TX17, scatter data to TX16, and receive records are polled on RX0/RX2/RX4.
Preparation checks disjoint physical and virtual descriptor/payload spans and
RX assembly storage before mutation. Activation verifies that the chip's ring
bindings match. The Windows platform must also check the separate raw-TX pool
against every allocation before starting the chip.

The transport drains pre-existing records before publication and accepts only
a matching Connac2 status response for an awaited command. Stale sequences,
unsolicited events, wrong event IDs and duplicate responses cannot complete
another exchange. Unsupported response formats are rejected before command
publication. Other records can be routed to a caller-supplied consumer; the
default counts and discards them. No raw-frame decoder is implied.

Firmware state is read at physical register `0x7c0600f0`: ROM-ready requires
`state & 7 == 1`, and RAM-ready requires `state & 3 == 3`. The respective
deadlines are 1,000 and 1,500 ms, matching the pinned MT7922 path. Command
and drain waits are bounded at 3,000 ms, with an iteration limit as a backstop
for a stalled clock. Any timeout or transport fault disarms all queues and
prevents reuse of the transport. It does not claim the hardware stopped;
the platform still must call chip shutdown before releasing allocations.

`nic_caps.c` parses Connac2 capability entries with 32-bit type and payload-size
fields, excluding their 8-byte headers. The MT7925 parser's 16-bit headers are
not reused. MAC and PHY entries are mandatory; missing optional entries remain
absent. Known duplicates, truncated/oversized entries, invalid MAC addresses
and NSS outside the MT7922's 1..2 range are rejected without changing output.
Band/path information comes from the reply; it does not authorize channels or
transmit power. The PCIe query sends CE command `0x8a` with no payload only
after observed RAM readiness. It follows the pinned MT7921 sequence/event
routing, rather than inventing a fixed capability event ID. Synthetic test
event IDs are fixtures, not live adapter observations.

`raw_rx.c` converts complete Connac2 receive records into the existing
radio-link v3 RX format. The base RXD is 24 bytes; optional groups appear in
4/1/2/3/5 order with lengths 16/16/8/8/72. Group 5 requires group 3. Header
padding is removed using the Connac2 field, and split A-MSDU padding is removed
after the actual MAC header. Translated, protected, cipher-stripped, erroneous
and control frames are discarded. Accepted records contain radiotap FLAGS and
CHANNEL plus the raw MPDU. No rate, signal or timestamp is invented. FCS
presence must be supplied from verified platform configuration. Frame-level
parse failures should be counted/discarded by the platform consumer, not
reported as fatal MCU transport faults.

`windows_platform.c` supplies real kernel MMIO, barriers, monotonic timing,
bounded delays and KMDF allocation adapters. Prepare checks PCI vendor/device,
subsystem and revision, matches bus-relative BAR0 to its translated memory
resource, and maps the first 1 MiB. It allocates descriptor/payload buffers with
the 32-bit DMA profile, checks logical and virtual non-overlap (including the
raw-TX pool), and prepares queues without accessing registers. These functions
are now wired by the separate `windows_device.c` bring-up entry point; the
identity harness retains its original identity-only callbacks.

The internal boot entry checks both immutable kernel input buffers against the
exact installed-image SHA-256 pins using CNG, then validates both layouts before
any MMIO. It starts the chip, boots firmware and queries capabilities. Raw TX
remains disarmed: successful firmware/capability setup is not radio readiness.
Each prepared generation permits one boot attempt. D0 exit disarms the queues
and attempts chip shutdown; allocations remain owned even if shutdown fails.
Surprise removal revokes register access. Release performs no MMIO and is
restricted to failed preparation or the framework's powered-down
[ReleaseHardware boundary](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nc-wdfdevice-evt_wdf_device_release_hardware).
`windows_device.c` supplies a passive device lock, PnP callbacks and an
exclusive administrator/SYSTEM request interface. Buffered request parsing
rejects invalid lengths before boot; both firmware hashes are still checked
in kernel mode. Status-only opens do not disarm prepared queues. Closing a
boot-owned handle stops its session. The aggregate boot deadline is 30 seconds,
separate from cleanup. Full power/reconnect recovery remains unfinished.
See [the bring-up interface and limits](../../DRIVER-BRINGUP.md).

## Offline verification

Build with the already installed MSVC/SDK; choose a fresh output folder:

```powershell
python .\airdrop-native\tools\build-mt7922-core.py .\airdrop-native\artifacts\mt7922-tx-new
```

The first passing receipt (local-only evidence: `../../artifacts/mt7922-tx-01/test-report.json`) records
compiler flags, exact source hashes, binary hash and test exit status. `/W4 /WX`
passes. Tests check full action-frame descriptor bytes, QoS field placement,
rejection without output mutation, both completion orders, token exhaustion,
40 ring-wrap/reuse iterations, malformed and duplicate notifications,
Connac3-format rejection, DMA overrun, publication failure and disarm behavior.

These are byte-format and ownership tests in ordinary process memory. The
callbacks are test functions, not device writes. They do not prove DMA
coherency, interrupt behavior, firmware acceptance, radio transmission or AirDrop.

The expanded receipt (local-only evidence: `../../artifacts/mt7922-core-03/test-report.json`) covers
the transmit tests plus MCU encoding/response and firmware-lifecycle tests.
It also records SHA-256 hashes of this machine's two installed firmware files.
The C parser found one patch region and five RAM regions. The simulated boot
issued 230 commands covering 889,744 region bytes, with entry `0x91b000`.
These counts describe the simulation, not hardware activity. The RAM parser
does not interpret the 120 extra bytes before the region table; CRC/signature
verification is not implemented here.

Failure tests cover invalid second images before any callback, invalid region
bounds, a busy semaphore, download/acknowledgment/drain failures, simultaneous
download and semaphore-release failures, and ROM/RAM readiness timeouts. MCU
tests include exact legacy/UNI headers, sequence wrap, unsupported flags,
out-of-sequence and unsolicited responses, command mismatches, and every
truncation of a unified status packet. Both builds used `/W4 /WX` successfully.

The chip and queue receipt (local-only evidence: `../../artifacts/mt7922-core-08/test-report.json`) adds chip
and DMA-queue suites, with all earlier suites retained and `/W4 /WX` passing.
Chip tests cover remap restoration, all-ones reads, invalid/overlapping ring
spans, correct command-ring selection, failures after address publication,
ignored DMA-disable writes, busy timeouts and subsequent shutdown recovery.
The initial busy-recovery test exposed a simulator error: it latched read-only
busy bits on a register write. The corrected simulator keeps those bits owned
by the simulated device; production shutdown checks were not relaxed.

DMA tests cover command encoding through actual queue submission, queue reserve,
40 wrap/reclaim cycles, scatter routing, rejection without sequence advancement,
completion overrun and uncertain publication. Receive tests cover short and
split records, ring wrap and budget boundaries, oversize/drop recovery,
zero/oversized DMA lengths, altered pointers, unsupported WED flags, failed
publication and callback failure without replay. Every callback operates on
process memory; none of these tests proves hardware behavior.

The integrated transport receipt (local-only evidence: `../../artifacts/mt7922-core-10/test-report.json`)
retains those suites and adds firmware loading through the actual chip,
command/receive queue, encoder and response-decoder code against simulated
registers and DMA memory. The installed images traverse 230 commands and
889,744 scatter bytes; all scatter bytes are checked in order against the
input region data, and 10 status responses match. No image is loaded onto
the real adapter.

Additional cases cover physical/virtual allocation aliasing, activation before
chip readiness, sequence and ring wrap, stale/duplicate/unsolicited replies,
incorrect event IDs, malformed replies, reentrant callbacks, a stalled clock,
absent DMA completion, post-publication write failure, all-ones register reads,
and ROM/RAM readiness timeouts. An early firmware acknowledgment does not
permit command-buffer reuse while DMA completion is absent. The final build
passes `/W4 /WX`; its receipt identifies the exact source and input hashes.

The receive receipt (local-only evidence: `../../artifacts/mt7922-core-13/test-report.json`) adds NIC
capability and raw-receive suites while retaining all earlier tests. NIC cases
include every truncation, length overflow, duplicate/unknown TLVs, 16-bit-format
rejection, optional fields, invalid addresses/NSS, unsolicited/stale replies,
query-before-readiness rejection and output preservation on failed exchanges.
Receive tests cover every optional-group combination and padding size, exact
radio-link bytes, 2.4/5/6 GHz metadata, FCS policy, translated/encrypted/error
drops, aggregation padding and output limits. An integrated test splits an
action frame across two RX2 DMA descriptors, assembles it, and produces the
expected radio-link record through the PCIe receive callback. These synthetic
frames are not evidence of discovery by an iOS device.

## Integration still required

Validate the now-wired platform's locking, PnP and request handling, then
validate the integrated transport and allocation/lifetime behavior on hardware,
verify live capability/receive formats and FCS policy, qualify the common MAC
sequence and monitor WCID/context behavior,
configure permitted channel/power settings, process RX and completion events,
and establish DMA quiescence before teardown. Then the AWDL engine must pass
discovery, synchronization and actual iOS file-transfer tests. None of those
requirements is waived by the offline tests. No driver is installed or rebound.

## Kernel build evidence

```powershell
python .\airdrop-native\tools\build-mt7922-kernel.py .\airdrop-native\artifacts\mt7922-kernel-new
```

The builder caches pinned Microsoft WDK/SDK 10.0.26100.1 headers/libraries under
`airdrop-native/.cache/wdk-10.0.26100.1`. Package SHA-512 values match the earlier
probe receipt; cached files are SHA-256 checked on reuse. No installer runs.
The default download cap is 8 MiB/s and can be lowered with
`--download-mib-per-second`.

Kernel receipt 02 (local-only evidence: `../../artifacts/mt7922-kernel-02/build-report.json`) records
successful `/kernel /W4 /WX /GS /guard:cf` compilation and native x64 linking of
all core modules plus the Windows resource adapter. `/OPT:NOREF` and map-symbol
checks prevent unused-code removal from disguising missing core dependencies.
Imports (local-only evidence: `../../artifacts/mt7922-kernel-02/imports.txt`) are limited to
`ntoskrnl.exe`, `WDFLDR.SYS` and `cng.sys`; no user-mode runtime is required.
The firmware pin constants were independently checked during the build.

The resulting `mt7922_core_identity_harness.sys` remains **unsigned and not
installed**. Its active callbacks still come from `driver/probe.c` and expose
only PCI identity. Linking the resource module does not activate it. No INF,
catalog, signing, verifier, boot, radio or iOS compatibility result is supplied
by this kernel build. Do not replace the working Wi-Fi driver with this harness.

Kernel receipt 05 (local-only evidence: `../../artifacts/mt7922-kernel-05/build-report.json`) instead
builds the wired bring-up entry with `--entry bringup`, including its PnP,
file-cleanup and three control callbacks. It remains unsigned/uninstalled and
contains no operational RF/AWDL path. Core receipt 15 (local-only evidence: `../../artifacts/mt7922-core-15/test-report.json`)
adds request-length, overflow, trailing-data and unchanged-output rejection
tests. Those are ordinary process-memory tests, not a live driver IOCTL test.
The client receipt (local-only evidence: `../../artifacts/mt7922-kernel-04/client-checks.json`) covers
offline status parsing, cleanup/error preservation and pinned input files;
its read-only open confirms that the bring-up device is absent.

Core receipt 17 (local-only evidence: `../../artifacts/mt7922-core-17/test-report.json`) adds common MAC
initialization to the simulated boot/capability path. The tests check exact
extended-command bytes, preserved unrelated register bits, 20 MT7922 WTBL clears,
raw-header policy, five-millisecond busy waits, a frozen clock, all-ones reads,
ignored stable writes, volatile counter bits, ACK-before-DMA-completion and
failure at every one of 119 register accesses. A failed first test run (receipt
16) exposed a fixture whose ignored write already had the desired value. The
fixture now starts those fields at the opposite value; production checks were
not weakened. All earlier suites still pass with `/W4 /WX`.

Kernel receipt 06 (local-only evidence: `../../artifacts/mt7922-kernel-06/build-report.json`) links this
stage into the Windows BOOT path, still unsigned/uninstalled. Its map explicitly
requires `mt2_mac_initialize`; dead-code removal is disabled. No live hardware
or iOS acceptance is implied by either receipt.

Core receipt 19 (local-only evidence: `../../artifacts/mt7922-core-19/test-report.json`) adds regulatory
encoding/response tests and the typed exchange through simulated PCIe queues.
It tests capability/preflight gates without I/O, malformed/stale responses,
timeout, output preservation and response-before-DMA-completion. Against the
pinned upstream RAM image, every nonempty country record in the selected block
is forwarded byte-exact (241); nine empty records are rejected. The first file
test wrongly assumed all countries had rules; that assertion was corrected,
not the production empty-record rejection. Receipt 18 (local-only evidence: `../../artifacts/mt7922-core-18/test-report.json`)
also validates the installed pair and rejects its absent CLC/regulatory data.

Kernel receipt 07 (local-only evidence: `../../artifacts/mt7922-kernel-07/build-report.json`) includes
the typed query and verifies its linked symbol. At that stage the driver pinned
only the installed pair. Kernel 17 now separately pins the
[upstream bundle](../../FIRMWARE-REFERENCE.md) for explicit RADIO_START;
no upstream image has been loaded on hardware. No source license or origin
claim is inferred from a matching hash; the bundle retains its upstream license
and acquisition receipt.

Core receipt 22 (local-only evidence: `../../artifacts/mt7922-core-22/test-report.json`) adds CLC and
fixed hardware-type tests: exact command bytes, both power variants, all 20
US/world records in the reference bundle, optional blocks, separate fallback
MTCL, capacity/truncation rejection, unknown ACPI state, wrong EFUSE address,
zero-valid reply, partial timeout, no-event delivery, repeat rejection and early
ACK with held DMA. Stock-pair receipt (local-only evidence: `../../artifacts/mt7922-core-22/stock-firmware-check.json`)
records the same binary's successful run against installed firmware, with absent
CLC rejected. Receipt 20 preserved an MSVC uninitialized-local warning; the reply
temporary now initializes to zero. Source review after receipt 21 added separate
world MTCL selection and its regression case.

Kernel receipt 09 (local-only evidence: `../../artifacts/mt7922-kernel-09/build-report.json`) links the
new APIs and checks their symbols with dead-code removal disabled. It remains
unsigned/uninstalled, with unchanged firmware pins and BOOT behavior. These
receipts establish offline behavior and native compilation, not live firmware
acceptance, RF behavior, discovery or file transfer.

## Platform ACPI and SAR policy

`platform_acpi.c/h` uses the pinned mt76 `mt792x_acpi_sar.c/h` country maps,
MTCL versions 1–3, MTDS/MTGS layouts, first-dynamic-row selection, geographic
rows and feature-flag translation. Frequency-range order is the eleven entries
in `mac80211.c`. The fixed-method Windows acquisition/absence distinction is
adapted from the pinned `airdrop-for-windows/driver/mt7925/platform_acpi.c` and
`device.c`, expanded to four methods and 263 table bytes. No MT7925 radio command
format is imported with it.

The kernel explicitly uses WDK V1 ACPI structures, confirmed with compile-time
assertions. Microsoft documents the [output array and variable argument lengths](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/acpiioct/ns-acpiioct-_acpi_eval_output_buffer_v1)
and [synchronous evaluation/status contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/acpiioct/ni-acpiioct-ioctl_acpi_eval_method).
Only byte-valued integer arrays fit this port's table contract. Partial output,
unsupported operations, timeout and malformed tables cannot become an absent
table or a prepared policy. Count/length bounds, rejection of unknown MTCL
versions and rejection of ambiguous signed power values are additional port
checks; OEM compatibility remains to be qualified.

Core receipt 23 (local-only evidence: `../../artifacts/mt7922-core-23/test-report.json`) covers response
truncations/types/statuses/maximum counts, errors at each of four acquisitions,
MTCL selected-country/world/EU mappings, flags, both SAR layouts, first-row
selection, all eleven frequency ranges, FCC/ETSI/world rows, disabled dynamic
SAR, no-limit sentinels, saturated offsets and stale-policy invalidation. All
ACPI input is synthetic. Kernel receipt 10 (local-only evidence: `../../artifacts/mt7922-kernel-10/build-report.json`)
links the module and wires acquisition before MMIO in explicit BOOT. The fixed
OEM AML methods have not been evaluated by this candidate; compilation is not
proof of actual lower-stack response formats or cancellation behavior.

## MT7922 channel and monitor control

`channel_control.c/h` derives command layouts from pinned mt76 `mt7921/mcu.c`,
`mt76_connac_mcu.c/h`, `mt76_connac.h` and `mt7921/mt7921.h`. It adapts occupied-
spectrum checks and flag mapping from the pinned Windows
`driver/mt7925/channel_control.c`, replacing its command sequence with MT7922
CE/extended formats. The 12-byte domain header is not MT7925's 16-byte UNI/TLV
header. RX-path setup uses an antenna mask; channel switching uses a stream count.
The two 76-byte payloads otherwise share the MT7921 layout. Sniffer uses UNI
`0x24`; RX filter uses CE `0x0a` with 68 bytes and no requested ACK.

Flags were checked against Linux v6.18's [regulatory definitions](https://github.com/torvalds/linux/blob/v6.18/include/uapi/linux/nl80211.h)
and [channel definitions](https://github.com/torvalds/linux/blob/v6.18/include/net/cfg80211.h).
The port does not relax DFS/no-IR restrictions using concurrency exceptions.
PSD rules additionally deny TX until a PSD power cap exists. Unknown CLC channel
information excludes UNII-4 rather than manufacturing a permissive mask. Radio-
link v3's numeric channel representation limits this path to 2.4/5 GHz.

Core receipt 34 (local-only evidence: `../../artifacts/mt7922-core-34/test-report.json`) exercises the
controller through actual core command/DMA code against modeled hardware. It
checks full RX-path/channel/sniffer/filter payloads, domain layout, 20/40/80 MHz
changes, both secondary-channel directions, gaps, contiguous AUTO_BW coverage,
overlapping bandwidth restrictions, indoor/outdoor policy, no-IR/DFS/PSD/unknown
flag denial, missing CLC channel information, outstanding TX and repeat guards,
truncated sniffer ACK, partial timeout and no-ACK command DMA timeout. The model
does not tune a radio or implement RF acknowledgment. Kernel receipt 17 (local-only evidence: `../../artifacts/mt7922-kernel-17/build-report.json`)
compiles/links the controller and checks its symbols; it is not called by BOOT.
Live channel switching, sustained receive, firmware power enforcement and AWDL
timing remain unproven.

**Reply-format correction:** Core 24/25 incorrectly expected a UNI status payload
for SNIFFER. The pinned `mt7921_mcu_parse_response` recognizes six UNI status
commands; SNIFFER is not among them and takes the full-header, sequence-only ACK
path. Core 26 onward corrects that assumption and checks a 36-byte sniffer ACK,
stale sequence and truncation. Extra payload bytes are not interpreted as status.
The older receipts remain historical evidence, not the current contract.

## Connac2 SKU and firmware pacing

`tx_power.c/h` derives the 44-byte header, 161 rate bytes plus channel byte,
channel arrays, eight-record batches and final-message marker from pinned
`mt76_connac_mcu.c/h` (`mt76_connac_mcu_rate_txpower_band` and its callers).
Requested-cap/SAR combination follows the ranges in `mac80211.c`, the default
rate-array behavior in `eeprom.c`, and MT7921's firmware-derived-domain host cap.
The Windows port requires an explicit finite cap and completed platform policy;
it does not adopt the upstream zero-to-unlimited default. It does not invent an
EIRP conversion, antenna correction or Linux device-tree policy.

The source explicitly reads `MT_PSE_BASE` after every SKU batch to avoid buffer
underflow. `mt2_pcie_query_pse` permits only that fixed address (`0x820c8000`,
from `mt792x_regs.h`) with CE query `0xc0`. The response's echoed address must
match; the final four bytes remain a full unsigned value. No arbitrary-register
access, writes or user IOCTL is exposed. DMA completion and firmware read must
both complete before the next batch. Failure retains DMA and disarms the path.

Core 34 (local-only evidence: `../../artifacts/mt7922-core-34/test-report.json`) checks all band masks,
171 channel records, reserved rate bytes, all short output capacities, finite
positive/zero/negative caps, SAR combination, 22 batch/read pairs, a high-bit
register value, wrong echoed address, short reply, partial timeout and held DMA.
Kernel 17 (local-only evidence: `../../artifacts/mt7922-kernel-17/build-report.json`) links both APIs.
Neither receipt proves applied RF limits; BOOT does not call them.

## Initial monitor device/BSS lifecycle

`monitor_context.c/h` uses pinned `mt76_connac_mcu_uni_add_dev`,
`mt76_connac_bss_basic_tlv`, `mt7921_add_interface`, `mt792x_mac_link_bss_remove`
and `mt792x_tx`. It fixes one initial interface at BSS/OMAC/band/WMM/link zero,
with monitor connection type `CONNECTION_INFRA_AP` (`0x00010002`). Start sends
16-byte DEV then 36-byte BSS requests; stop reverses their order. Both commands
use the six-command UNI status decoder, unlike the sniffer command above.

The initial BSS WCIDs are zero because upstream issues add_dev before assigning
the interface WCID. Subsequent removal carries reserved slot 19. The port mirrors
those initial/removal bytes and does not infer a valid TX station from either.
MT7921 has no link-index assignment in its add-interface path; the initial zero
comes from interface allocation, not an MT7925 link-index convention. Recovery,
re-addition and multi-interface operation are outside this initial lifecycle.

Core 34 checks full start/stop payloads and order, valid unicast addresses,
short output capacities, no-I/O rejection, all four command status failures,
both start-stage timeouts, ACK-before-DMA-drain failure, stale sequence,
truncation, single-attempt state and context invalidation on channel failure.
Kernel 17 links start/stop and the newer explicit radio ABI below; diagnostic BOOT remains unchanged. A successful
context sequence does not activate raw TX, initialize a transmit WTBL entry or
prove AirDrop discovery.

## Fixed-rate monitor TX and transport integration

`radio_tx.c/h` follows `mt792x_tx` for monitor-interface WCID selection and
`mt7921_add_interface` for reserved slot 19. The inspected monitor add path
initializes software WCID state and clears admission counters; it does not send
the associated-peer STA_REC command used by AP/station paths. Common MAC already
clears all 20 admission-counter slots. This supports an initial monitor TX path
without inventing an associated peer or importing MT7925 station commands.
It is source evidence, not proof of firmware acceptance on this adapter.

`mt76_connac2_mac_write_txwi`, `mt76_connac_spe_idx` and the Connac2 TXD bit
definitions supply fixed-rate and antenna selection: the default 6 Mbps OFDM is `0x4b`, SPE 0
for one chain and SPE 24 for two chains. `MT_TXD6_SPE_ID_IDX` stays clear; setting
it would choose the WTBL antenna index instead of the explicit descriptor index.
The wrapper fixes OMAC 0/WCID 19, requires the monitor source address, and checks
current channel permission, completed power/context and actual observed state
before activation/submission. No caller-supplied WCID or raw hardware rate/antenna index is exposed. The radio ABI
accepts eight validated legacy OFDM rates in 500-kbps units.

`mt2_pcie_bind_tx` validates used raw-TX spans against all MCU DMA banks and
metadata. Windows calls it during preparation. Activation verifies the chip's
TX0 ring binding; raw activation independently verifies zero producer/consumer
indices. The MCU poll loop reads TX0's consumer at `0xd430c` and routes type-6
Connac2 releases before optional data/event callbacks. This also runs during MCU
exchanges. MCU disarm now disarms the borrowed raw queue; neither frees memory.
Unbound/inactive queues retain the previous event-consumer behavior.

Core 34 (local-only evidence: `../../artifacts/mt7922-core-34/test-report.json`) exercises binding before
chip activation, ring mismatch, virtual/physical/metadata aliases, readiness
rejection, actual descriptor publication through modeled registers, both
completion orders, completion during MCU commands, ring/token wrap, ring-full
and token-full states, restricted-channel retuning, malformed release batches,
post-publication write failure, invalid consumer indices and MCU failure.
The tests check retained ownership and no automatic replay after failure.
Kernel 17 (local-only evidence: `../../artifacts/mt7922-kernel-17/build-report.json`) links all raw-TX
entry points and wires preparation-time binding. The newer session and explicit
radio ABI below extend this path; diagnostic BOOT keeps its stock-image pins.
FCS qualification, live RF and iOS discovery/transfer tests remain required.

## Session composition and bounded RX delivery

`radio_session.c/h` composes the MT7922 APIs above. Its single-attempt lifecycle,
borrowed immutable firmware and retained DMA approach are informed by pinned
`airdrop-for-windows/driver/mt7925/radio_session.c/h`; command formats, firmware
parsers and readiness stages remain the MT7922 implementation. Inputs carry no
default country or unlimited power setting. A validation-only DFS-zero policy
is replaced by the actual regulatory response before CLC/SAR commands.
`mt2_channel_validate` supplies pure geometry/permission checks before domain
activation, preserving the existing channel restrictions.

`rx_bridge.c/h` owns complete radio_link RX messages in caller-supplied nonpaged
storage. The MCU callback copies before its borrowed RX assembly can be reused.
It bounds output slots, drops newest on full, counts parser outcomes, retains a
message on insufficient output capacity and clears message bytes on consumption
or terminal stop. Frame errors do not poison a healthy MCU transport; malformed
outer RXD/DMA remains a transport error. FCS policy is explicit and needs live
platform qualification. No raw capture is written to disk by this code.

Windows preparation now allocates 16 output slots, prepares the bridge/session
and installs the bridge callback. It leaves both RX delivery and raw TX inactive.
Stop/release revoke the session and clear output before releasing that storage;
the existing chip/WDF boundary still governs DMA-memory release. The diagnostic
ABI and BOOT endpoint keep their stock-image pins; RADIO_START uses its own pair.

Core 34 (local-only evidence: `../../artifacts/mt7922-core-34/test-report.json`) covers complete
regulatory-to-TX session startup, the actual DFS policy, receive-only startup,
retuning, TX-drain deferral, receive ownership after DMA-buffer overwrite,
overflow with continuing MCU commands, malformed on-air versus outer metadata,
eight startup failure stages, terminal fault/stop and rejection of repeated
start. Bridge tests cover span aliases, capacity, ordering, wraparound, FCS flags,
inactive delivery, bounded overflow and byte clearing. All device state is modeled.

Core 31's first session test incorrectly changed the outer RXD length while
expecting a harmless frame drop. It correctly failed. Core 34 instead verifies
both distinct contracts: invalid MPDU version is a counted drop; invalid outer
length faults the session. The transport rejection was not relaxed.
Kernel 17 (local-only evidence: `../../artifacts/mt7922-kernel-17/build-report.json`) links the composed
session and receive callback and wires Windows preparation/stop. The public
session ABI, separate firmware pins and native user-mode adapter are now
implemented; hardware and live picker/file-transfer acceptance remain unfinished.

## Explicit radio ABI and AWDL adapter

`radio_abi.c/h` parses exact METHOD_BUFFERED requests into a temporary view,
rejecting altered access bits, truncated/oversize/trailing input and invalid
options before effects. `windows_device.c` exposes start, service, receive,
transmit, channel, status and address under the existing exclusive admin/SYSTEM
endpoint. Shared input is fully consumed before output overwrites it.
[The wire contract](../../RADIO-ABI.md) documents byte offsets and ownership.

RADIO_START uses only the pinned upstream 20260724 firmware pair; diagnostic
BOOT retains the installed Windows pair. Both hashes and image layouts are
checked before ACPI/MMIO. Status distinguishes boot attempt, software readiness,
DMA publication/quiescence, operation errors and receive counters. It never
asserts peer receipt or AirDrop readiness. Core 34 and Kernel 17 verify these
additions offline; neither performed hardware loading.

`mt2_monitor_context_address` waits for both outstanding TX ownership classes
to clear, sends DEV_INFO_ACTIVE, then commits the encoded address only after
zero status and command-DMA completion. Failures revoke readiness. Tests cover
retained old identity, pending ownership and the source-address change.
`mt2_radio_tx_submit_rate` maps 500-kbps values 12,18,24,36,48,72,96,108 to
Connac2 codes 0x4b,0x4f,0x4a,0x4e,0x49,0x4d,0x48,0x4c from pinned
`mt76/mac80211.c`. All eight descriptors are checked through modeled queues.

The [native bridge](../../radio/SOURCES.md) translates the pinned AWDL engine's
user-mode protocol to this interface. It does not transplant MT7925 hardware
formats. Its Windows pipe is restricted to the process user, admins and SYSTEM.
