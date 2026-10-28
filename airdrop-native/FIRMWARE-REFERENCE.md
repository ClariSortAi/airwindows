# MT7922 reference firmware and regulatory path

The stock Windows firmware remains the only pair accepted by the diagnostic
driver's SHA-256 checks. No downloaded firmware has been loaded on this adapter.
The regulatory and CLC code is kernel-linked but is not called by the BOOT IOCTL.

## Why a second bundle is being evaluated

The installed Windows RAM image has five encrypted download regions and no
non-download CLC region. The mt76 checkout includes an older November 2023
MT7922 image with CLC data. For the native port, the current upstream bundle was
obtained as an additional offline reference rather than mixing old CLC bytes
into the installed Windows firmware.

Pinned linux-firmware commit: `d0039fd797d7776d89f00c22c45754914a96706e`.
Upstream identifies patch version `20260724143257a` and RAM version
`20260724143402` in [WHENCE](https://gitlab.com/kernel-firmware/linux-firmware/-/blob/d0039fd797d7776d89f00c22c45754914a96706e/WHENCE).
These are device firmware files; evaluating them does not introduce a Linux or
WSL runtime requirement into the Windows driver.

| File | SHA-256 |
|---|---|
| `WIFI_MT7922_patch_mcu_1_1_hdr.bin` | `7fc9075d6d54b31e0539734614ca971fa52caf41b642a3000c8a4501591a4d98` |
| `WIFI_RAM_CODE_MT7922_1.bin` | `d100e5e04c324fc8da6302112d1e0b4fac952286447d2419eabcd8fb0a1f5b93` |

Download receipt (local-only evidence: `artifacts/mt7922-linux-firmware-20260724-03/source-receipt.json`)
records source URLs, the pinned commit, Git blob IDs, file hashes, license and
WHENCE. The fetcher checked every downloaded object against its pinned Git blob
ID. That proves content consistency with those objects, not a vendor signature.
Two earlier attempts remain as incomplete receipts: one used the obsolete root
license path; the second encountered a Gitiles HTTP 503. The completed fetch
used the upstream GitLab blob API. Reference checkouts were not modified.

## Implemented regulatory exchange

The RAM image has six regions: five for device download and one unencrypted,
non-download CLC region. Its CLC region contains two index-0 power variants and
two index-3 regulatory blocks. The MT7921/MT7922 source keeps the first index-3
block; its hardware-encapsulation filter applies to power blocks. The MT7925
selector's type-at-byte-6 convention must not be copied here.

`driver/mt7922/regulatory.c` validates image/block/country boundaries, rejects a
missing or empty selected country and builds the 80-byte-prefix CE `0xd1`
request. It preserves the opaque country rule/signature bytes. No host-side
signature validation or power-limit interpretation is claimed.

The PCIe exchange requires observed RAM readiness and an actual NIC capability
reply reporting `REGD_EN`. It accepts only a complete Connac2 rule response,
retains tag/length/EIRP as raw fields where their semantics are unverified, and
commits output only after command DMA drains. Malformed responses and timeouts
fault the transport; no partial domain reaches the caller. A successful query
alone never enables transmission.

## Implemented CLC delivery and hardware selection

`clc_power.c` uses MT7922's flat country records and 76-byte command header.
Index-0 selection requires a fixed read of EFUSE block `0x550`, using only bit 0
of byte `0x55b` to select the encapsulation variant. The internal query has no
caller-selected address, writes, programming or bulk output. Its response
must match the requested address; zero `valid` is conservatively rejected.
Upstream MT7921 does not check that field, so this additional gate needs hardware
qualification. No such read has been performed on this adapter by the new code.

The plan validates every selected block and request capacity before issuing
any command. All matching records are sent, not just the first country match.
Absent country records use upstream's `00`/indoor fallback, including a separately
resolved world MTCL setting. Optional indices select their first block. Unknown
platform ACPI acquisition state is rejected; neither absent ACPI tables nor a
country is assumed. There is no Linux device-tree policy in this Windows path.

CLC replies are requested only when observed NIC capabilities advertise them.
Each command must drain from DMA. Without reply support, successful delivery
leaves channel configuration explicitly unknown. Partial failure faults/disarms
the transport, retains DMA allocations and forbids retry in the same transport
generation; it cannot roll back firmware commands already delivered. No result
enables RF or raw TX. The current single-attempt API is for initial bring-up;
runtime country changes need a separate lifecycle design.

## Platform acquisition now wired

The native BOOT path now acquires fixed `MTCL`, `MTDS`, `MTGS` and `MTFG` methods
before MMIO. `platform_acpi.c` validates Windows V1 integer-array responses,
retains per-method status, distinguishes absence from failed acquisition and
validates every table before marking the collection complete. The policy
builder supplies separate selected-country/world MTCL settings, feature flags
and eleven SAR limits. See [the acquisition contract](DRIVER-BRINGUP.md#platform-power-data).
This implementation has only synthetic response evidence; actual OEM methods
and derived power settings are still unverified.

## Evidence and remaining work

Core tests 34 (local-only evidence: `artifacts/mt7922-core-34/test-report.json`) pass with the downloaded
pair. They check every one of the first regulatory block's 241 nonempty country
records against the source bytes and reject its nine empty records. Those empty
records are data in this bundle, not a conclusion about national law. Tests also
cover malformed/truncated records, unsupported features, stale responses,
timeouts, capability gating and response-before-DMA-completion. Simulated boot
transfers 892,816 bytes in 231 commands, skipping the CLC region as required.
The CLC suite preserves all 20 US/world records across both power variants byte
for byte. Synthetic cases cover optional blocks, independently resolved fallback
MTCL, unknown ACPI state, fixed-address read validation, no-event delivery,
partial timeout, repeat rejection and ACK-before-DMA-completion.

Core tests 18 (local-only evidence: `artifacts/mt7922-core-18/test-report.json`) also pass against the
installed Windows pair and explicitly reject its missing regulatory region.
The first real-bundle test assumed every country had at least one rule and
failed; the corrected test verifies rejection of empty records. Production
rejection was not relaxed.

The previous CLC binary's stock-pair check (local-only evidence: `artifacts/mt7922-core-22/stock-firmware-check.json`)
also passes and rejects its absent CLC power/regulatory records.

Core tests 23 (local-only evidence: `artifacts/mt7922-core-23/test-report.json`) also exercise native
ACPI response decoding, per-method failures, MTCL mappings, SAR layouts,
geographic limits, disabled dynamic tables and invalidation of stale policy.

Kernel build 17 (local-only evidence: `artifacts/mt7922-kernel-17/build-report.json`) compiles and links
the regulatory/CLC/ACPI/channel/SKU/context/raw-TX/session paths with `/kernel /W4 /WX`. It is unsigned and uninstalled.
Diagnostic BOOT keeps its existing pins and ABI; explicit RADIO_START now pins
the reviewed upstream pair. Country selection,
hardware validation of ACPI acquisition, live public control of the composed
[radio session](DRIVER-BRINGUP.md#composed-radio-session-and-bounded-receive-queue),
monitor WCID/FCS qualification and live validation still remain. Tests using US records do
not select US for operation; the user's operating country is pending.
