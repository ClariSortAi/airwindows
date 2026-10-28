# Verification boundary

## Publication checkout checks

The curated checkout was independently tested before the initial push: core
tests passed without optional firmware-image cases; bridge passed 533 contract
checks, native pipe tests and the actual AWDL child-process test against a modeled
device (82 frames, no RF or host adapter); receiver passed 32 IPv6-loopback checks
and verified three files. The AWDL child executable came from the existing pinned
AWDL build; it was not rebuilt for publication. AirPlay was source-archived without
repeating its earlier build or device tests. Pinned reference sources were fetched
successfully by the new bootstrap script. The source scan found no credential
pattern matches or excluded binary/capture files. These are bounded checks, not
a guarantee that arbitrary source text contains no sensitive information.

The only runtime-helper changes for publication create the LLVM cache parent on
a fresh clone and resolve the REA MCP helper inside this repository. The helper
is included; REA remains an optional, separately installed analysis dependency.

## Earlier local results

Recorded on 2026-10-28 in the original Windows development workspace. This is a
summary, not a substitute for the excluded raw receipts. No hardware test is
implied by a successful build, model or loopback result.

| Check | Recorded result | Local receipt |
|---|---|---|
| MT7922 core | Build and hardware-free tests passed | `mt7922-core-34/test-report.json` |
| Kernel candidate | Compiled/linked; unsigned, never loaded | `mt7922-kernel-17/build-report.json` |
| Native radio bridge | 533 contract checks, real Windows pipe tests, modeled AWDL child-process test passed | `mt7922-bridge-03/verification.json` |
| Native AWDL engine | Build and 10 offline test executables passed; 17 upstream compiler warnings remain | `awdl-01/verification.json` |
| Receiver | 32 checks; real HTTPS over IPv6 loopback; three received files matched hashes | `receiver-02/verification.json` |
| Unsigned package | InfVerif `/w` and `/h` passed; Inf2Cat zero errors/warnings | `mt7922-package-01/package-report.json` |
| Recovery preparation | 15 stock files copied and hashed; 14 catalog members passed kernel-policy signature verification; live restore untested | `stock-driver-restore-01/` |

The kernel SYS SHA-256 is
`9921d09062c9eb93d0081facbb28534113cf8a67ccc07fba1b1879c07043c0da`.
The unsigned package INF SHA-256 is
`648efd4a71e4a3982270ab02598653170c5ef7281d006aa7ff90d18892f08386`.

Outstanding: hardware boot, platform power/regulatory behavior, raw RX/TX, AWDL
interoperability, native iOS picker visibility, desktop consent workflow and actual
iOS file receipt. Package validation supplies no trusted driver signature.
