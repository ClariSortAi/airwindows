# MT7922 Windows radio interface, version 1

This is an unsigned, hardware-unqualified candidate. Software readiness means
the command sequence completed; it does not mean an iOS device received a frame
or discovered an AirDrop target. No installation or firmware loading has occurred.

Open `\\.\AirWindowsMt7922Bringup` exclusively, with read/write access. The
device permits administrators and SYSTEM. All requests are METHOD_BUFFERED,
little-endian, contain no pointers, and run under one passive device lock.
Only the handle that attempted boot may service, receive, transmit, retune or
change the address. Closing that handle attempts shutdown. No automatic restart.

| Operation | Exact IOCTL | Input | Output capacity |
|---|---|---|---|
| RADIO_START | `0x22e40c` | 64-byte header and both firmware images | 256 |
| SERVICE | `0x22e410` | Empty | 256 |
| RECEIVE | `0x226414` | Empty | 16392 |
| TRANSMIT | `0x22e418` | 12-byte header and MPDU | 256 |
| CHANNEL | `0x22e41c` | Three u32: primary, center, width | 256 |
| RADIO_STATUS | `0x226420` | Empty | 256 |
| ADDRESS | `0x22e424` | Six MAC bytes | 256 |
| STOP (existing diagnostic ABI) | `0x22a408` | Empty | 128 |

Exact input lengths and encoded access bits matter. Structural errors fail the
IOCTL. Except RECEIVE, a valid radio request returns a complete 256-byte status
record even when its operation failed: inspect the embedded NTSTATUS and core
error. STOP uses the existing 128-byte diagnostic record, not this radio record.
RADIO_STATUS performs no radio I/O and reports the last operation status.

## Start request

| Offset | Field |
|---|---|
| 0, 4 | u32 magic `0x3252544d` (MTR2), version 1 |
| 8, 12 | u32 patch length, RAM length, each 1..8 MiB |
| 16 | Two country bytes: uppercase alpha2 or `00` |
| 18 | Six-byte nonzero unicast monitor MAC |
| 24, 28 | u32 environment 0..2, power type 0..4 |
| 32, 36, 40 | u32 primary channel, center channel, width 20/40/80 MHz |
| 44, 48 | u32 FCS-present and enable-TX booleans |
| 52 | i32 finite cap in half-dBm, -127..44 inclusive |
| 56, 60 | u32 firmware pair 1, reserved zero |
| 64 | Exact patch bytes followed by exact RAM bytes; no trailer |

Pair 1 selects only the [pinned upstream 20260724 images](FIRMWARE-REFERENCE.md).
Both SHA-256 hashes, image layouts, explicit options and country-record presence
are checked before ACPI/MMIO. Diagnostic BOOT retains its separate stock-image
pins. Country choice, actual platform restrictions and FCS behavior require
hardware qualification. A numeric host power cap is not decoded regulatory EIRP.
World `00` is not a substitute for the operator's actual country.

## Runtime requests

TRANSMIT contains u32 no-ACK (0/1), rate in 500-kbps units, and MPDU length,
then the exact 24..4095-byte MPDU. Allowed rates are 12,18,24,36,48,72,96,108
(6,9,12,18,24,36,48,54 Mbps). No radiotap or FCS is included. The source MAC
must match the committed monitor address. A returned token indicates local
queue acceptance, not over-the-air delivery or peer acknowledgment.

CHANNEL validates complete geometry and current restrictions. ADDRESS accepts
only a nonzero unicast MAC. Both return PENDING while outstanding TX ownership
prevents the change. ADDRESS commits only after the DEV_INFO command is
acknowledged and its DMA completes. A failed hardware change revokes readiness.

SERVICE polls MCU/raw DMA and routes completed frames into the bounded queue.
RECEIVE only dequeues; it does not service hardware. An empty queue returns
`STATUS_NO_MORE_ENTRIES` (Win32 `ERROR_NO_MORE_ITEMS`, 259), with no output.
A successful receive returns one complete radio-link v3 RX message: 8-byte
header (u16 type 2, u16 reserved zero, u32 payload length), then radiotap+MPDU.
It never returns a partial record. The queue has 16 slots and drops new frames
when full; counters expose the loss.

## Radio status record

| Offset | Field |
|---|---|
| 0, 4, 8 | u32 magic `0x3253524d` (MRS2), version 1, size 256 |
| 12 | u32 flags, defined below |
| 16 | u32 session stage, 0..12; 12 is software-ready |
| 20, 24, 28 | i32 core error, i32 stop error, operation NTSTATUS |
| 32 | u32 hardware generation |
| 36, 40, 44 | u32 primary, center, width |
| 48, 50 | Country[2], committed MAC[6] |
| 56, 60, 64, 68, 72 | i32 cap, u32 FCS, environment, power type, firmware pair |
| 76, 80, 84 | u32 TX queued, TX inflight, RX queued |
| 88, 96, 104, 112 | u64 RX accepted, taken, overflow, inactive |
| 120..191 | Nine u64 receive-decoder outcome counters |
| 192, 200 | u64 ignored MCU records, matched responses |
| 208, 212 | u32 TX token, token-valid boolean (only successful TRANSMIT) |
| 216..255 | Reserved zero |

Flags: 1 prepared; 2 session attempted; 4 software sequence ready; 8 stopped;
16 TX active; 32 RX active; 64 firmware ready; 128 DMA quiescent; 256 removed;
512 DMA published; 1024 boot attempted. FULL (-2) and PENDING (-3) can occur
while the session remains healthy. INVALID (-1) may reject a request without
faulting the session. Check readiness and operation result separately.

STOP's 128-byte record starts with magic `0x3253544d`, version 1, size 128.
Flags at offset 12 use the *diagnostic* layout: DMA-published 64, quiescent 128,
boot-attempted 8, removed 4. Operation NTSTATUS is at 16, prior core error at
20, stop error at 24. A prior core error does not itself mean STOP failed.
DMA storage remains retained until the driver's hardware-release boundary.

Source: [parser](driver/mt7922/radio_abi.c),
[Windows dispatch](driver/mt7922/windows_device.c),
offline tests (local-only evidence: `artifacts/mt7922-core-34/test-report.json`),
unsigned build (local-only evidence: `artifacts/mt7922-kernel-17/build-report.json`).
