# Layer 4 — Face Recognition Privacy & Retention

This file documents the privacy contract for Dotty's on-device face
recognition feature. Biometric data of minors is in scope, so the contract
is deliberately narrow: **embeddings live on the robot and only on the
robot.** This file is the source of truth for that promise — anything that
deviates from what's documented here is a bug.

## Summary

| Property                  | Value                                                       |
| ------------------------- | ----------------------------------------------------------- |
| Storage                   | ESP32-S3 NVS, namespace `face_recog`                        |
| Encryption at rest        | Planned (NVS encryption); plaintext in scaffold             |
| Network egress            | None — embeddings are NEVER sent to the bridge or upstream  |
| Identity sent to bridge   | Yes — only the assigned name string (`"Hudson"`, `"unknown"`) |
| Capacity                  | 10 enrolled identities (`FaceRecognizer::kMaxEnrolled`)     |
| Enrollment auth           | Parental gate (PIN or long-press) — single-shot, 30 s window |
| Deletion path             | `self.robot.face_forget` MCP tool, or `name="*"` to wipe all |
| Reset path                | NVS erase wipes `face_recog` namespace along with everything else |

## What is stored

For each enrolled identity, exactly one record:

- `name` (UTF-8 string, ≤ 32 bytes, validated at enrollment)
- `embedding` (512 × float32 — fixed-length feature vector from the
  on-device ESP-DL face-recognition model)

The cropped face image and any intermediate frames are **not** persisted.
Embeddings are not photos and cannot be inverted to reconstruct a face,
but they are still biometric identifiers and are treated accordingly.

## What is sent off-device

- The `face_recognized` perception event sent to the bridge contains
  **only** the assigned name (or the literal `"unknown"`). The embedding
  vector never crosses the WS protocol boundary.
- No frames, crops, or pixel data leave the device as part of recognition.
- The bridge / server side has no way to retrieve embeddings via any MCP
  tool — there is no `face_export`, `face_dump`, etc., and there will not
  be one.

## Encryption at rest

The scaffold writes plaintext blobs to NVS. NVS encryption on ESP32-S3 is
a 2-key XTS-AES scheme with eFuse-pinned keys; enabling it requires:

1. `CONFIG_NVS_ENCRYPTION=y` in menuconfig
2. A custom partition table including an `nvs_keys` partition
3. Flash-time provisioning of the encryption keys (one-shot)

These are tracked as a follow-up to the scaffold commit. The on-disk blob
format (length-prefixed name + fixed-length embedding) is self-describing,
so the encrypt-in-place migration is a single re-write pass at boot.

Until encryption is enabled, anyone with physical access AND an esptool
flasher can read the NVS partition. This is acceptable for the dev /
family-only deployment but MUST be closed before the device is loaned,
gifted, or sold.

## Parental gate

`enroll` and `forget` require the parental gate to be unlocked first. The
gate is single-shot:

- A successful `face_unlock` sets a 30-second window.
- The next sensitive op (enroll OR forget) consumes the window.
- Window expiry without consumption clears the unlock.

Two unlock methods:

- **PIN** (`method="pin"`, `secret="…"`): hardcoded constexpr placeholder
  in `parental_gate.h` (`kEnrollPIN = "1234"`) for development. Production
  will move to menuconfig + hashed compare. The PIN path SHOULD NOT be the
  primary path long-term — kids can shoulder-surf it.
- **Long-press** (`method="long_press"`): the head-pet capacitive sensor
  held for 5 s arms a flag that this method consumes. The actual long-
  press detector is not yet wired (TODO in `parental_gate.cpp`); the flag
  exists so the unlock plumbing is testable today.

`face_list` (read-only) does NOT require the gate — it returns names only,
and the names are already known to anyone who's set the device up.

## Deletion / "right to be forgotten"

- `self.robot.face_forget` with `name="Hudson"` removes that one record
  and re-persists the slot table.
- `self.robot.face_forget` with `name="*"` calls `forgetAll()` which wipes
  the in-memory cache and re-writes a zero-count NVS table.
- Full device reset (factory NVS erase) removes the `face_recog` namespace
  along with everything else.

There is no soft-delete or tombstone — `forget` is destructive and
immediate.

## Cross-reference: privacy-indicator LEDs (Layer 1)

The face-tracking modifier already drives the left LED green while a face
is in frame (see `face_tracking.cpp::setTrackingLed`). The recognition
event reuses that same LED — there is no additional indicator for "the
robot is identifying you," because identification only fires when a face
is already detected and the green LED is already lit.

If a future revision adds an LED state specifically for "identification
in progress" or "enrollment in progress", document it here and in
`stackchan-fw/docs/modes.md` (the LED contract).

## Audit trail

The recognizer logs at INFO level on every enrollment, forget, and successful
recognition match (only on identity transitions, not every frame). Logs are
local to the device (UART + ring buffer); they are not shipped off-box.
Bridge-side logs see only the `face_recognized` event payloads.

## Open follow-ups (post-scaffold)

- [ ] Wire ESP-DL `face_recognition.so` in `FaceRecognizer::computeEmbedding`.
- [ ] Enable NVS encryption (menuconfig + partition table).
- [ ] Move `kEnrollPIN` from constexpr to menuconfig + hashed compare.
- [ ] Wire the head-pet long-press detector into `ParentalGate::setLongPressFlag`.
- [ ] Add a "forget all" voice intent in the bridge and document it in
  `tasks.md` Layer 4 section.
