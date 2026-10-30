# Decoded frame interface

The receiver provides opt-in, local frame access for a supervised iPhone controller. The packaged `Start-AirWindows.cmd` launcher enables it, or start the executable with `--frames`. Launching the executable without that option exposes no frame interface. `AirWindowsNative.exe` and the `AirWindows.exe` alias include focused-window Bluetooth mouse and keyboard control; see [native control](NATIVE-CONTROL.md) and [the agent operating guide](CLAUDE-CONTROL.md). The frame pipe itself remains read-only.

Connect to `\\.\pipe\AirWindows.Frames.<receiver PID>`. Windows access control restricts the pipe to the receiver process's user, and remote pipe clients are rejected. There is one connection at a time. Connecting requests one image; no command or authentication token is sent.

The response is a four-byte little-endian JSON length, that many UTF-8 JSON bytes, then `image_bytes` bytes of binary PPM. The client acknowledges complete receipt with one byte before closing. Slow or stalled clients are disconnected with bounded I/O waits; no screenshot queue accumulates. The receiver retains one reference to its latest decoded frame, downloads GPU pixels only when requested and exports native visible dimensions without window chrome or letterboxing. It does not save images to disk.

Successful JSON fields:

| Field | Meaning |
|---|---|
| `protocol`, `source`, `format` | Version 1; `airplay` or explicitly marked `selftest`; `ppm` |
| `receiver_pid`, `instance` | Process ID and instance token; bind both |
| `generation`, `sequence` | Lifecycle/geometry epoch and decoded frame sequence |
| `received_tick_ms`, `decoded_tick_ms` | Receiver `GetTickCount64` timestamps |
| `source_pts_ns` | Decoder PTS mapped by the existing AirPlay path; not an independently verified phone render time |
| `width`, `height`, `image_bytes` | Exact image geometry and payload length |
| `control_ready`, `control_focused`, `control_busy` | Current receiver binding, focused-input and queue/worker state |
| `control_tick_ms`, `control_epoch` | Time the UI refreshed control state; epoch changes when readiness or the selected binding changes |
| `control_peer`, `control_sender` | Selected Bluetooth/AirPlay fingerprints; empty when control is not ready |

Errors return `ok:false`, an error string and `image_bytes:0`. Pause/resume, flush/reset, codec changes, mirror loss and geometry changes invalidate retained frames and advance the generation. Queued packets carry their original generation and receipt time through FFmpeg's `AV_CODEC_FLAG_COPY_OPAQUE`; decoding after a state change cannot make an old packet valid. A request fails if packet receipt or decode age exceeds one second. Consumers must check age again after full receipt and before acting, bind the intended phone separately, and reject changed generations or geometry. The API cannot establish when iOS rendered a picture or identify the BLE target from AirPlay alone.

The Python client is `scripts/observe-phone.py`, backed by `scripts/airwindows_frames.py`. It checks the pipe's actual server PID, validates source/metadata/size/timestamps, and saves a PNG with adjacent JSON only when requested. The command rejects synthetic frames. It sends no phone input; desktop agents use the focused receiver window for input.

The compiled `iosctl` client uses the same read-only pipe. Before input it requires fresh control metadata and an unchanged receiver, frame generation, dimensions and binding epoch. After an action it waits for post-action focused/idle status before returning focus. These fields describe receiver state, not a phone action acknowledgement. Existing readers can ignore the additional fields.

## Verification

`scripts\build.ps1 -Target FrameServerCheck`, then `build\Release\bin\FrameServerCheck.exe`, checks fresh pixels, idle state, pause/resume, stale queued generations, one-second expiry, disconnect/reconnect, geometry invalidation and server restart through the actual pipe.

On 2026-10-30, both D3D11 and software H.264 self-tests exported visually inspected 640x360 color-pattern frames. Self-test runs no longer start the AirPlay network listener. The real iPhone then mirrored 498x1080 H.264; exported images guided ESP32 BLE pointer movement and a click that opened Settings. Sample packet ages at client receipt were 15-47 ms; that measurement excludes phone rendering and earlier network transit. H.265 export, audio and automated coordinate control are not covered by this acceptance test.
