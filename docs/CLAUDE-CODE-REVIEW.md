# Claude Code integration and release review

Updated for **0.3.1-rc.2** on 2026-10-30. Claude Code's local `iosctl` prototype and the relevant session transcript informed this integration. Raw transcripts, phone captures, device identifiers, personal maps and demo recipients remain local. The next candidate's [three first-day fixes](DAY-ONE-RC2.md) add wheel normalization, idle-picture inspection and setup diagnosis.

## What was folded in

The Windows package now includes `iosctl.exe`: taps, text, Home, paced swipes/flicks, page movement, named targets, screenshots, crops, brightness-grid inspection, screen-change sampling and preflight. The source is in [app/tools/iosctl](../app/tools/iosctl/README.md), with a portable photo-demo guide.

Claude's transcript reports app navigation, list scrolling, Button 2 Home and repeated Camera captures on the earlier running integration build. It also records missed taps, inconsistent paging and a Home action that left the screen unchanged. Those observations shaped this candidate; they do not qualify the revised executable on a phone.

## Improvements made before packaging

| Original issue | Integrated behavior |
|---|---|
| First process chosen automatically; fixed 498 × 1080 geometry | Explicit receiver PID, verified pipe-server PID, actual source dimensions and DPI-aware picture mapping |
| Personal icon locations and recipient embedded in demo files | Explicit per-user map, empty example, dimension checks and a generic photo workflow |
| Desktop screenshots could include occlusion or another window | Decoded phone-only frames with receiver identity and freshness checks |
| Focus and `SendInput` results ignored | Checked foreground/video focus, target visibility, geometry and input return values; held modifiers stop typing |
| Failed or unknown commands could report success | Whole-batch parsing before input, bounded arguments, structured output and nonzero failure/timeout codes |
| Automatic retries inferred success from pixel differences | One input attempt; `changed` reports pixels and `settle` reports stability, with no automatic replay |
| Exceptions could leave buttons held or focus displaced | Release-on-error cleanup, paired down/up within one batch, restore attempts and a distinct cleanup/restore failure code |
| Concurrent helpers could interleave input | One input lease per receiver; competing batches fail before input |
| Preflight inferred readiness from toolbar state | Receiver-exported control readiness, selected identities, binding epoch, focus and queue/worker activity |
| Another HID subscriber caused provider teardown and reconnect | Exact selected-peer routing and reconnect; extra subscribers do not cancel that peer or withdraw its service; unrelated control writes are rejected |
| Source checkout needed for agent tools | Compiled helper, guides, preflight and example map included in the Windows ZIP; Python is optional |

The new tests caught and fixed an ArrayList parsing error and an asynchronous timeout cleanup crash. Source and binary versions now come from the same CMake version settings, and packaging rejects mismatched helper/receiver versions.

## Validation and remaining work

Local checks cover control/coordinate assertions, discovery/battery policy, both frame clients, helper commands and frame-pipe lifecycle/metadata. Helper tests include actual local-pipe reads and timeouts, malformed commands, target bounds, metadata changes, concurrency and release-on-error logic. They send no desktop or Bluetooth input. The [candidate verification record](DAY-ONE-RC2.md) covers the new behavior.

Complete a live-device run of the packaged candidate before calling it physically accepted: click, exact text, drag, mapped Home, return to Windows, and a second bonded phone joining/leaving. Other Bluetooth adapters, iOS versions, paste and wheel input remain separate test work. A static picture can exceed the normal one-second limit; standalone `snapshot` now permits age-labelled inspection. Input and verification still require a fresh observation.

The active development receiver was kept separate from the package build. This release includes the revised helper and its complete source. It excludes raw session evidence and unrelated AirDrop research from the application archives.
