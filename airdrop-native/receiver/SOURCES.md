# AirWindows native Windows file engine

These Python modules derive from `airdrop/` at Airdrop-for-Windows commit
`74606871b001bdba5e447f84a43bb96124fb495a`. [reference-source.json](reference-source.json)
records the exact original file hashes; [LICENSE](LICENSE) and file notices retain
the MIT terms and attribution, including Brent Kearney's omdrop receiver work.
The pinned reference checkout has not been edited.

Local changes:

* `receiver.py`: bind only the selected scoped IPv6 link-local address or `::1`;
  prohibit wildcard/global binding; disable IPv4 mapping; use exclusive Windows
  address binding; verify both peer address class and scope. Malformed or excessive
  Ask structures return an HTTP rejection before transfer approval.
* `engine.py`: pass the selected bind address and publish the approval slot before
  the `asked` event, so a synchronous answer cannot be lost.

Other copied modules remain unchanged. `loopback_check.py` is the reference's
single-file test entry. [The expanded local verifier](../tools/verify-receiver.py)
exercises actual HTTPS with accepted/declined transfers, hashes, duplicate names,
listener isolation and shutdown. Receiver 02 (local-only evidence: `../artifacts/receiver-02/verification.json`)
records 32 passing checks. No iOS, Wi-Fi, Bluetooth or external network was used.

`engine.py` is the integration entry: JSON commands on stdin, events on stdout,
diagnostics on stderr, and approval required by default. It waits for the named
AWDL interface in normal mode. `--loopback` is exclusively a local test mode.
Do not use the legacy standalone `receiver.py` CLI as the desktop approval flow:
its base Watcher accepts transfers automatically. The engine supplies the approval
watcher used in the verified path. Received files are stored, never executed.

Native Windows Python 3.10+ and OpenSSL are required; no WSL or pip packages.
See [the runtime evidence and remaining work](../NATIVE-RUNTIME.md).
