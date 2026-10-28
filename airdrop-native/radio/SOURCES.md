# Native MT7922 AWDL bridge

The executable is a radio backend, not an AirDrop receiver. It joins the pinned
native AWDL engine's radio-link v3 protocol to the MT7922 Windows candidate's
[explicit ABI](../RADIO-ABI.md). It has not run on the adapter.

Source reference: `references/airdrop-for-windows`, commit
`74606871b001bdba5e447f84a43bb96124fb495a`. That checkout remains unmodified.

| Source | Use | License |
|---|---|---|
| Reference `radio/mt7925_codec.h` | Semantic operation IDs, Reply, bounded radiotap parsing | BSD-3-Clause-Clear |
| Reference `radio/mt7925_device.h` | Bounded file loader and SHA-256 verification only; its MT7925 Device is never constructed | BSD-3-Clause-Clear |
| Reference `radio/mt7925_bridge.h` | Generic bridge, source/BSSID policy, retry and polling behavior | GPL-2.0-only |
| Reference `radio/mt7925_rx_diagnostics.h`, `frame_capture.h` | Bridge dependencies; capture stays disabled | GPL-2.0-only |
| Reference `radio/radio_link.h` | Shared wire protocol | MIT |
| Local `secure_pipe.h` | Derived from reference `pipe_server.h`; renamed class, required explicit security attributes, missing-ACL rejection | GPL-2.0-only |
| Local `serve.h` | Derived from reference bridge's serve loop; uses the restricted pipe class | GPL-2.0-only |
| Local `mt7922_adapter.h`, `mt7922_device.h` | MT7922 translation, status decoding, Windows transport and explicit pipe ACL | BSD-3-Clause-Clear |
| Local executable and tests | Composition and verification | GPL-2.0-only |

License notices and terms remain in the [reference license inventory](../references/airdrop-for-windows/LICENSE)
and its referenced files. The linked executable is GPL-2.0-only. Names containing
MT7925 identify reused user-mode protocol/policy code; no Connac3 descriptor,
MT7925 firmware or MT7925 device interface is used by this backend.

## Contract and limits

The adapter translates channel/address requests and all eight legacy OFDM rates
to the MT7922 ABI. Its production path is the same templated adapter exercised
by a fake DeviceIoControl transport. Polling services hardware first, then takes
one complete RX message. Empty receive queues are normal. Backpressure permits
bounded retries; Windows I/O errors and uncertain responses are never replayed.
Explicit STOP uses its own operation status instead of confusing a retained
prior core error with shutdown failure. Closing the kernel handle is a fallback,
not proof that DMA quiesced.

The pipe allows the process's Windows user, administrators and SYSTEM. It rejects
remote clients, uses one instance and requires first-instance ownership. No raw
capture is enabled. The kernel endpoint remains administrator/SYSTEM-only.

The CLI requires `--run`, firmware path, actual country, channel, finite power cap,
measured FCS setting and a 1..600-second trial duration. It creates an initial
random local unicast MAC, then waits for the AWDL client's acknowledged address
update before accepting that client's TX. The CLI never installs or elevates.
Kernel calls are synchronous; the outer trial deadline does not guarantee a
hard cancellation bound inside OEM ACPI/lower-stack work.

Build from the workspace root into a new directory:

```powershell
python .\airdrop-native\tools\build-mt7922-bridge.py .\airdrop-native\artifacts\mt7922-bridge-new
```

Bridge 02 verification (local-only evidence: `../artifacts/mt7922-bridge-02/verification.json`) records
MSVC C++20 `/W4 /WX` builds, 533 contract assertions, real local pipe tests and
CLI rejection tests. Only `--help` and invalid arguments ran on the executable;
the tests never open a device. The source/binary hashes are in the receipt.
Bridge 01 stopped on a test-only integer-narrowing warning; the explicit byte
cast fixed it without lowering the warning level.

Still required: signed driver deployment and recovery, live platform/FCS/radio
qualification, AWDL engine startup and peer traffic, AirDrop receiver integration,
native iOS picker visibility and file-hash verification. Offline assertions and
a local pipe handshake establish none of those hardware or iOS outcomes.

## Actual AWDL executable integration

Bridge 03 (local-only evidence: `../artifacts/mt7922-bridge-03/verification.json`) adds a bounded test
launching the native AWDL executable with `-n` (no virtual adapter) against the
real bridge/adapter and a fake device transport. It programmed one MAC and
submitted 82 discovery frames at 12 Mbps, then exited cleanly after pipe closure.
No driver or RF operation occurred. `test_awdl_process.cpp` is GPL-2.0-only.
The builder accepts `--awdld PATH` and records the tested executable hash.
