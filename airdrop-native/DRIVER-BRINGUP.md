# MT7922 firmware bring-up candidate

**Unsigned, not installed, not yet an AirDrop receiver.** The current candidate
connects KMDF lifecycle callbacks to the MT7922 core. Installing it would replace
the stock adapter's Wi-Fi networking with an administrator-only diagnostic/radio
device. Ethernet configuration is outside this driver's operations.

Kernel build 17 (local-only evidence: `artifacts/mt7922-kernel-17/build-report.json`) compiles and links
the wired callbacks, common MAC initialization, regulatory exchange, fixed
hardware-type query, CLC power-table delivery, ACPI/SAR policy acquisition and
channel/monitor control, SKU power delivery, initial monitor device/BSS setup
gated raw-TX submission/completion, composed radio sessions and bounded RX delivery.
BOOT acquires the four fixed platform tables before MMIO and still stops after MAC
initialization. Regulatory exchange, hardware-type query, CLC delivery, channel control, SKU delivery, monitor context,
raw-TX activation and policy construction are joined by an internal session API.
Explicit RADIO_START now invokes that API using the separately pinned upstream
pair; [the radio ABI](RADIO-ABI.md) also exposes service, RX, TX, channel and address operations.
Core tests 34 (local-only evidence: `artifacts/mt7922-core-34/test-report.json`)
include complete simulated radio sessions, eight startup-failure stages,
bounded receive delivery, both raw-TX completion orders, failure ownership, SKU pacing,
monitor-context lifecycle, channel/monitor, ACPI, regulatory,
CLC, request-boundary and MAC failure tests. Neither result validates live KMDF lifecycle,
DMA, firmware boot or radio operation. Driver signing, controlled hardware
qualification and radio initialization remain required.

## Build and offline checks

From the repository root; select a fresh artifact directory:

```powershell
python .\airdrop-native\tools\build-mt7922-kernel.py .\airdrop-native\artifacts\mt7922-kernel-new --entry bringup
python .\airdrop-native\tools\mt7922-bringup.py self-test
python .\airdrop-native\tools\mt7922-bringup.py validate --firmware-dir 'C:\Windows\System32\DriverStore\FileRepository\mtkwl6ex.inf_amd64_385620a2f6f13ea1'
```

The default kernel entry remains the identity-only harness. `--entry bringup`
selects `windows_device.c` and outputs `airwindows_mt7922_bringup.sys`.
The candidate INF is [driver/airwindows_mt7922_bringup.inf](driver/airwindows_mt7922_bringup.inf).
Package 01 (local-only evidence: `artifacts/mt7922-package-01/package-report.json`) now passes Microsoft
InfVerif Windows-driver and signature-requirement checks, and Inf2Cat generates
an unsigned catalog with no errors or warnings. Actual installation, signing
and hardware behavior remain unverified. See [the proposed trial and recovery](HARDWARE-TRIAL.md).

## Diagnostic interface

Device: `\\.\AirWindowsMt7922Bringup`. The device is exclusive, restricted to
Administrators/SYSTEM, and rejects paths below its root. All requests are
buffered; no user-provided pointer is dereferenced.

| Operation | IOCTL | Access | Behavior |
|---|---|---|---|
| Status | `0x226400` | Read | Returns a 128-byte status record; no radio/register operations |
| Boot | `0x22e404` | Read/write | Validates input and pinned hashes, acquires/validates fixed ACPI tables, selects normal firmware mode, boots firmware, queries capabilities and initializes common MAC registers |
| Stop | `0x22a408` | Write | Disarms queues, attempts verified DMA shutdown, retains all allocations |

The [ABI header](driver/mt7922/device_abi.h) defines the 16-byte boot header and
status flags. The [client](tools/mt7922-bringup.py) decodes the status layout.
The response carries the operation's NTSTATUS separately from successful IOCTL
transport. It always reports `radio_ready=false` and `airdrop_ready=false`.
MAC-address/capability fields are valid only when their corresponding flag is set.
ABI v1 has no separate MAC-initialization field: successful BOOT operation status
in build 06 requires that sequence to complete. Failure is reported in core/MCU
error fields; detailed step/register diagnostics currently remain internal.
Build 10 additionally requires successful ACPI acquisition/validation before
MMIO. Per-method status and table bytes remain internal; there is no public
ACPI query or caller-supplied policy override.

`status` opens an already-installed device only. The current
client checks (local-only evidence: `artifacts/mt7922-kernel-04/client-checks.json`) record Win32 error 2
because the device does not exist. The same receipt records 138 malformed-status
cases, four cleanup/error cases, and successful local firmware-file validation.

After separate installation and hardware qualification, the client's explicit
`boot --firmware-dir ...` operation reports boot status and then requests stop
before closing. Closing a handle that owns a boot also invokes shutdown as a
fallback. A status-only handle does not disarm prepared queues. The client
retains both boot and shutdown errors if both fail.

## Lifetime and current limits

One passive device lock serializes requests and PnP operations. Surprise removal
revokes MMIO before acquiring that lock. Boot has a 30-second aggregate deadline
in addition to per-command deadlines; cleanup has its own bounded core shutdown.
No DMA allocation is released merely because boot/stop fails. Release occurs
at failed preparation or KMDF's powered-down ReleaseHardware boundary.

Boot is explicit and permitted once per prepared generation. Sleep/resume does
not automatically restart firmware or create a new generation. Full recovery,
live session-control validation, monitor WCID qualification, sustained RX/TX,
AWDL and file-transfer acceptance remain unfinished. No live boot request has
been issued to this candidate.

## Platform power data

After both firmware hashes and layouts pass, BOOT evaluates only `MTCL`, `MTDS`,
`MTGS` and `MTFG` on this adapter's lower device stack. It explicitly uses ACPI
V1 output structures, with WDK layout assertions. A fixed 2,116-byte buffer fits
263 byte-valued integer arguments. Unknown status, overflow, nested/wrong types,
non-byte values, inconsistent lengths or malformed tables fail before MMIO.
Only `STATUS_OBJECT_NAME_NOT_FOUND` with no output is treated as an absent method.
No alternate namespace or retry is used.

Requests carry a two-second WDF cancellation timeout, limited by the remaining
30-second boot deadline. This is not a hard wall-clock guarantee: synchronous
completion still depends on the lower stack completing cancellation. The input
and output remain alive until the synchronous call returns. Removal is checked
before and after evaluation. ACPI executes OEM AML; these fixed table requests
are not a guarantee that firmware methods have no side effects.

The platform policy builder translates MTCL versions 1â€“3, bounds MTFG flag
counts and derives eleven SAR range limits from the first MTDS row and the
FCC/ETSI/world MTGS row. It selects SAR layout by actual length/count, including
tables whose layout differs from the MTCL version. `0xff` means no SAR limit;
other values above 127 are rejected rather than interpreted as signed power.
The `127` no-limit sentinel is not permission to transmit at that power.
Radio integration must still combine these limits with the channel, antenna,
hardware and requested-power constraints. No real tables have been acquired by
this candidate and no derived policy has been applied to the radio.

## Internal channel and monitor control

The kernel-linked controller builds the MT7922 CE channel-domain command,
enables its MAC and configures RX-path/channel/sniffer/filter commands through
the existing polling MCU queues. This is not called by BOOT and exposes no new
IOCTL. It requires the same generation's capability, MAC, CLC and decoded-domain
results. All objects must remain alive and serialized with transmission.

The current radio-link v3 format identifies 2.4/5 GHz channels numerically and
has no 6 GHz band discriminator. This controller therefore supports 20/40/80 MHz
on 2.4/5 GHz, checking the primary, center, constituent channels and occupied
spectrum. Unknown CLC channel information excludes UNII-4. Rule overlaps retain
restrictions; gaps and unsupported widths fail before commands are published.
Listening on a restricted channel does not permit transmission. DFS, no-IR,
point-to-point-only, PSD without a derived power cap, unknown flags and mismatched
indoor/outdoor requirements deny the transmit-eligibility check.

Queued or firmware-owned raw TX blocks a channel change. Every control command
must drain DMA, including domain/filter setters that request no reply. State
changes commit only after the complete sequence succeeds. Partial failure clears
the reported channel, faults/disarms MCU and raw TX, and retains allocations;
firmware commands already applied are not rolled back. A successful extended
ACK does not measure actual tuning. Sniffer uses a UNI request but, unlike
DEV/BSS, its reply has no verified status payload in the pinned MT7921 parser.
Live session-control validation, monitor WCID qualification, firmware/FCS qualification
and AWDL integration remain required before usable reception.

## Internal SKU power delivery

`tx_power.c` combines an explicit finite requested cap with all eleven decoded
SAR limits and emits the pinned Connac2 SKU layout: a 44-byte header and up to
eight 162-byte channel records per batch. All observed bands are programmed,
including the 6 GHz table even though the current channel API selects only
2.4/5 GHz. All three bands require 22 batches containing 171 channel records.
Unused rate fields retain upstream's `127` sentinel; actual rate fields receive
the smaller of the requested cap and the matching SAR cap. Zero means zero dBm.
The accepted requested range is -127 through 44 half-dBm; 44 follows upstream's
22 dBm host ceiling, not a decoded national EIRP limit or measured RF limit.

Each no-ACK SKU command must drain DMA, followed by a fixed firmware read of
`0x820c8000`, the pacing operation upstream uses to avoid PSE buffer underflow.
The returned value is an unsigned register value, not a firmware error code.
Wrong addresses, malformed replies, timeouts and incomplete DMA prevent success.
Partial failure clears channel/context/power readiness and disarms queues while
retaining storage. This is a single initial attempt, with raw TX still inactive.

The code does not decode opaque regulatory EIRP fields or independently measure
antenna gain, calibration or actual radiated power. Correct CLC/EFUSE selection,
actual platform tables, country selection and firmware enforcement require
hardware qualification. RADIO_START calls this API; diagnostic BOOT does not.

## Initial monitor device and BSS

`monitor_context.c` implements one initial interface, with BSS, OMAC, band, WMM
and link indices zero. It sends DEV then BSS on start, BSS then DEV on stop;
both commands require matching zero UNI status and completed DMA. Its 16-byte
DEV and 36-byte BSS payloads follow `mt76_connac_mcu_uni_add_dev`.

The initial BSS carries WCID zero, following `mt7921_add_interface` before that
function assigns reserved interface slot 19. Deactivation carries slot 19,
matching removal after assignment. That logical slot choice does not initialize
a station record or qualify transmission. Initial link index zero follows the
MT7921 path, not the MT7925 reference's link conventions.

Start requires channel-domain setup and inactive sniffer/raw TX; stop requires
sniffer stopped and no queued/firmware-owned TX. Only one start attempt is allowed
per generation. A partial failure sends no rollback commands and clears readiness,
disarms queues and retains DMA storage. RADIO_START invokes it after firmware and policy checks.
These are initial context commands, not an operational AWDL link or receiver.

## Raw transmit submission and completion

Windows preparation now binds its prepared TX0 queue to the MCU transport.
Binding checks used DMA/virtual spans against MCU banks, assembly and queue
metadata. Chip activation checks the matching ring address/count. Binding is
software-only and does not activate raw TX; BOOT still stops after common MAC.

The internal `radio_tx.c` entry points require observed firmware/NIC state,
common MAC initialization, completed CLC and finite SKU delivery, monitor
context and a permitted channel. Start also checks zero producer/consumer
indices before activating the fresh queue. RADIO_START and TRANSMIT invoke this path after request/ownership checks.

Submissions accept raw action or plain/QoS data frames whose source matches the
monitor MAC. They fix OMAC 0 and interface WCID 19, mapping explicit 6–54 Mbps
legacy OFDM rates from 500 kbps units to Connac2 encodings. One observed
chain selects SPE index 0; two chains select 24. This follows the pinned
MT7921 monitor path: its add-interface operation assigns slot 19 and clears
admission counters, without adding an associated peer station. That source
mapping replaces the earlier assumption that a separate associated-station
command necessarily had to precede raw monitor TX. Actual firmware acceptance
and radio behavior still require hardware qualification.

MCU polling now reclaims TX0 DMA and routes Connac2 token-release records to
the bound queue, including during command exchanges. A payload remains owned
until both completions arrive, in either order. Token release does not establish
successful transmission or an iOS ACK. Fatal MCU or TX errors disarm both paths
and retain memory. A failed doorbell write may already have reached hardware;
it never frees the submitted frame or triggers an automatic replay.

Each submission rechecks channel permission. An empty queue may follow a channel
change, but a listen-only channel cannot accept new TX. Terminal software stop
returns pending while either ownership remains, then disarms permanently for
this generation. Chip shutdown remains necessary before memory release.

## Composed radio session and bounded receive queue

`radio_session.c` joins the previously separate radio stages after a successful
firmware/NIC/common-MAC boot. It requires explicit country, environment, power
type, finite cap, channel geometry, monitor address, FCS policy and TX enable.
It checks the image's country records and actual acquired platform tables before
its first command. DFS zero during that preflight is validation-only; firmware's
decoded DFS region supplies the policy used for CLC/SAR programming.

Startup then queries the regulatory domain and hardware type, builds policy,
delivers CLC, validates channel geometry/permission, applies the domain, creates
monitor context, enables/configures monitoring, sends SKU limits and activates
the receive bridge and optional raw TX. A transmit-enabled start rejects a
listen-only initial channel before MAC/domain activation. Receive-only sessions
leave raw TX inactive. No RAM-image pointer survives the start call; the owner
must verify that the supplied image is the exact pair booted on the device.

One hardware attempt is allowed per generation. Every post-attempt failure
records its stage, revokes the session, disarms MCU/TX, clears channel readiness
and discards queued receive messages. It does not replay commands, free DMA or
claim hardware quiescence. The platform still owns chip shutdown and its error.
Channel changes wait for both TX ownerships; invalid geometry leaves the current
session intact. Every transmit rechecks current channel policy.

Windows preparation allocates a separate nonpaged output queue with 16 slots of
16,392 bytes each and installs its receive callback. Completed raw frames become
radio_link v3 RX messages copied out of the recycled DMA assembly. Full queues
drop the newest frame; short reads retain the oldest complete message. Malformed
on-air frames are counted drops. Corrupt outer receive/DMA metadata still faults
the transport. Successful reads and stop wipe queued bytes. MCU control replies
and TX releases continue to be handled when this output queue is full.

FCS presence remains an explicit platform configuration, not inferred from a
frame's trailing bytes. Live FCS format and firmware behavior need qualification.
The session's `ready` field means its software sequence completed; it does not
prove an iOS peer saw the radio. The existing diagnostic status continues to
report radio/AirDrop readiness as false. The new radio status separately reports
software sequence readiness. Diagnostic BOOT does not start RF. RADIO_START
uses the newly pinned upstream pair. The [native AWDL bridge](radio/SOURCES.md)
now builds and passes offline tests; radio hardware and iOS acceptance remain unverified.

## Common MAC initialization

The new stage selects on-device EFUSE calibration using the fixed four-byte
buffer-mode command. It does not read or program EFUSE. It sets receive length,
enables hardware deaggregation, explicitly disables header translation for raw
802.11, clears admission counters for 20 station-table slots, initializes both
MAC bands and sends the fixed RTS threshold command. Those counter clears do
not create valid transmit station entries.

The two extended-command replies follow the pinned MT7921 parser's solicited
sequence matching. No firmware-status payload or specific event ID is assumed;
a reply is acknowledgment evidence only. Command DMA must also finish. Stable
register fields are read back; volatile counter-enable bits are only flushed.
WTBL busy polling has a five-millisecond bound and an iteration backstop.
Any partial failure disarms the transport and retains DMA storage for shutdown.
Raw TX remains inactive throughout. This sequence does not authorize or verify
transmission on any channel.

The [upstream firmware evaluation](FIRMWARE-REFERENCE.md) documents both pairs.
Diagnostic BOOT retains its stock Windows pins. RADIO_START explicitly selects
only the pinned 20260724 upstream pair, with no fallback or mixed images.

## Native user-mode bridge

Bridge 02 (local-only evidence: `artifacts/mt7922-bridge-02/verification.json`) builds the native C++20
radio backend and tests request translation, response validation, source identity,
backpressure, RX delivery and STOP through the actual adapter class with a mock
device. Separate Windows tests exercise a restricted local byte-stream pipe.
See [source basis and operating limits](radio/SOURCES.md). No driver was opened.
