# Claude Code review before the first control release

Reviewed on 2026-10-30: the receiver source, Claude Code's local `iosctl` helper and the relevant conversation transcript. This is a summary of development evidence. Raw transcripts, phone captures, device identifiers, personal targets and demo recipients remain local.

## What Claude added

Claude built a C# desktop helper for taps, paced drags, text, Home, screenshots and screen-change sampling. It added a named target map, a connection preflight and a Camera demo runbook. Those files were in the research workspace, outside the publication checkout.

The transcript reports successful app navigation, list scrolling, Button 2 Home and repeated Camera captures. It also records missed taps, inconsistent paging and a Home action that left the screen unchanged. These are useful device observations from the running integration build, not a separate qualification of the packaged executable.

Claude traced repeated Bluetooth reconnects to a second previously paired phone subscribing to the HID service. Disconnecting that phone stabilized the session. The user asked Claude to leave receiver code unchanged, and it did.

## Findings and release decisions

| Finding | Consequence | Action |
|---|---|---|
| Agent instructions and frame reader were only in source | A binary download was insufficient for agent setup | Bundle both, a launcher and the reader dependency file in the Windows ZIP |
| Helper assumes a 498 × 1080 portrait phone and a personal icon map | Coordinates can target the wrong control on another phone or layout | Keep personal maps out; document measured picture bounds and fresh coordinates |
| Helper takes the first receiver and ignores focus/input return values | Input can be missed or reach the wrong foreground window | Keep it experimental; use a supported desktop tool for the release |
| `tapv`, `gov` and `page` retry based on pixel changes | A repeat may toggle, submit twice or act on a changed screen; changed pixels do not establish the intended result | Require observe, act once, verify in the published agent instructions |
| Helper returns success for some failed or unknown commands and lacks guaranteed cleanup after an exception | An agent can misread failure or leave a button held | Require nonzero failures and guaranteed release before packaging this helper |
| Preflight infers readiness from a disabled Connect button and peer count | It can overstate readiness without proving identity, freshness or successful input | Use current receiver status and a visible input test |
| A second HID subscriber triggers provider teardown and reconnect | Another paired phone can interrupt control | Document the one-phone workaround; keep the runtime behavior unchanged in this packaging release |

## Follow-up engineering

Before shipping `iosctl`, add explicit receiver selection, current frame dimensions, checked focus and input delivery, bounded arguments, guaranteed button release and failures that stop a command batch. Remove automatic input replay. Use the decoded-frame pipe for phone observations; desktop captures can include occlusion or another window. Store target maps per user and derive them from the current screen.

For multi-phone handling, distinguish an extra subscriber from loss of the selected phone. Any change must preserve device-specific notification routing and be tested with two bonded phones, including disconnect/reconnect. Do not simply remove subscriber checks.

Home is now supported by session evidence, but still needs its one-time mapping and a visible test on each setup. Paste, wheel input, other adapters and other iOS versions remain separate test work.

## Package boundary

The preview includes the receiver, launcher, setup and operating guides, decoded-frame reader, license notices, matching source and checksums. It uses ordinary focused desktop input. The personal demo helper, raw session evidence and unrelated AirDrop research are excluded from the application archives.
