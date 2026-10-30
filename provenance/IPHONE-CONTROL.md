# iPhone control integration: 2026-10-30

The 0.3.0 source snapshot brings the AirPlay frame interface and native Windows BLE HID backend into the published repository. The application, backend and frame reader were synchronized from the local development projects. Personal captures, device identities, pairing settings, bench logs, compiled probes and caches were excluded.

The original local checkout and running receiver were preserved. Packaging changes were built in a separate checkout. The new executable was not substituted into the active phone session.

## Device evidence

On the earlier integration build, a real iPhone received native Bluetooth movement, absolute pointing, clicks, text and a vertical list drag. Three measured absolute positions were within 0.56 mirrored pixels on each axis. The user confirmed clicking and typing through the receiver window and returning keyboard focus to the PC.

The new portable reader successfully read a real 498x1080 AirPlay frame from the running receiver. Its reported receiver age was 63 ms; this excludes phone rendering and earlier network transit.

Right-click Button 2 submissions appear in the local control log. They do not establish that Button 2 was assigned to Home or that Home completed. Home mapping, paste, wheel, other adapters, long-running reconnect reliability and agent-driven input remain unqualified.

## Software checks

- Built `AirWindowsNative` from the isolated source tree with the existing pinned dependency cache.
- Passed 191 control queue and coordinate checks and 19 discovery/battery policy assertions.
- Passed 12 Python frame-metadata checks.
- Passed frame-pipe checks for fresh pixels, idle, pause/resume, stale packets, timeout, disconnect/reconnect, geometry, invalidation and restart.
- Checked portable paths, documentation links and exclusion of generated artifacts.
- Checked the binary/source archive pair, upstream sources, descriptor notice and checksums. Extracted the corrected source archive, ran bootstrap and rebuilt the receiver with the pinned dependency cache.

These results do not qualify the new packaged binary on a phone. GitHub Actions supplies separate evidence of a build from the repository on a hosted Windows runner.
