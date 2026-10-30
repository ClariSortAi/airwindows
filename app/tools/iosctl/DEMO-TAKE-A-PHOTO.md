# Demo: take a photo with the user's iPhone

Use this only when the user asks to take a photo. Follow [the helper guide](README.md) first. Keep captures local and confirm the intended phone. The demo contains no fixed app coordinates or recipient addresses.

1. Run `iosctl --pid <receiver-pid> "preflight"`. Read a fresh full-size image with `shot`.
2. Invoke `home`, wait briefly, then capture another image. Confirm the Home Screen. If a sheet or overlay remains, use its visible dismiss control and observe again.
3. Find Camera in the current image. Tap that observed location and use `wait 600; shot ...`. A live viewfinder may never satisfy `settle`.
4. Inspect which camera is active. Flip is a toggle, not “set front camera.” Flip once only if the user's requested lens is not already active, then inspect the result.
5. Find the shutter in the current image. Tap once, wait, and inspect the new thumbnail. A changed-pixels result alone does not prove that a photo was taken.
6. Report the observed result. If the result is uncertain, inspect before taking another photo.

For a named map, derive `camera`, `flip` and `shutter` from this phone's current screens. Confirm the matching screen before each `go` command. Home Screen pages can place different apps at the same coordinates.

Sending or sharing the photo is a separate action. Do it only when the user explicitly requests it, with their intended destination. Never infer the recipient from an account signed in on the phone or from a previous demo.
