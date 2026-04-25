# Layer 4 — Face Recognition Privacy & Retention

This file documents the privacy contract for Dotty's face recognition
feature. Biometric data of minors is in scope, so the contract is
deliberately narrow: **face data stays on the LAN, full stop.**

## Architecture (server-side compute)

Recognition compute runs on the RPi bridge (`zeroclaw-bridge`), not on
the ESP32. The ESP32 captures and streams the JPEG; the bridge runs
`face_recognition` (dlib) to detect, embed, and match against a SQLite
store. The ESP32-side `FaceRecognizer` scaffold (in this directory) is
**dormant** — it remains in-tree for forward compatibility but is not
on the runtime path.

This is a deliberate change from the original on-device-only design.
Reason: the project follows a self-hosted philosophy (everything but the
LLM runs on hardware Brett owns), and routing face data to the bridge
keeps the architecture coherent — biometrics traverse the same LAN as
voice audio and never reach the public internet. See
`/home/brett/Repos/dotty-private/audits/2026-04-25-network-audit.md` for
the prior precedent: only the OpenRouter LLM path egresses; everything
else (ASR, TTS, calendar, perception) stays local.

## Summary

| Property                  | Value                                                              |
| ------------------------- | ------------------------------------------------------------------ |
| Storage                   | SQLite at `/root/.zeroclaw/faces.sqlite` on the RPi bridge         |
| File mode                 | 0600 (root-only on the Pi)                                         |
| Encryption at rest        | Filesystem-level only (no disk encryption on DietPi); SQLite-level encryption is a TODO |
| Network egress            | LAN only — JPEGs flow ESP32 → RPi; embeddings + names stay on the RPi |
| Identity in voice path    | Yes — `[Speaker: name]` line injected on identity *transitions* in the voice prompt |
| Embedding dimensionality  | 128-d float32 (`face_recognition` library, dlib backend)           |
| Capacity                  | 50 enrolled identities (`FaceDB.CAPACITY`); raise for friends-of-friends |
| Match threshold           | Cosine similarity ≥ 0.5 (env `FACE_MATCH_THRESHOLD`)               |
| Enrollment auth           | None in v1 (family-only deployment, deliberate trust assumption)   |
| Deletion path             | `forget` MCP tool / voice intent ("forget X" / "forget everyone"); immediate, no soft-delete |
| Reset path                | Delete `/root/.zeroclaw/faces.sqlite`                              |

## What is stored

For each enrolled identity, exactly one row in the `faces` table:

- `name` — UTF-8 string, ≤ 32 bytes, validated and Title-cased on insert,
  unique under `COLLATE NOCASE` (so "Brett" and "brett" collide)
- `embedding` — 128 × float32 = 512 bytes (`numpy.tobytes()`), L2-normalised
- `created_at`, `updated_at` — POSIX timestamps
- `n_samples` — increments on each re-enroll for the same name

The cropped face image and source JPEG are **never** persisted. Embeddings
are not photos and cannot be inverted to reconstruct a face — but they
are still biometric identifiers and are treated accordingly.

## What is sent off-device

- The `face_recognized` perception event sent into the bridge's perception
  bus contains **only** the assigned name (or `"unknown"`). Embedding
  vectors never cross any external boundary — the bridge keeps them in
  SQLite locally.
- The `[Speaker: name]` voice-turn injection is a name only; sent to
  ZeroClaw → OpenRouter as part of the prompt. The user implicitly
  consents to this when they enroll (the LLM is told "you're talking to
  Brett", which is necessary to make the feature useful).
- No frames, crops, or pixel data leave the LAN.

## Network boundaries

- **ESP32 → RPi (LAN)**: JPEG payloads for `/api/face/enroll` and
  `/api/face/recognize`. These are short-lived (< 200 ms each) and the
  RPi does not log the bytes.
- **RPi → internet**: only the OpenRouter LLM call (already established;
  see `audits/2026-04-25-network-audit.md`). The LLM sees names, never
  embeddings or images.

## Encryption at rest

The bridge writes plaintext SQLite to disk with mode 0600. The DietPi
filesystem is unencrypted. Anyone with physical access AND a removed
SD card can read the embeddings. This is acceptable for the dev /
family-only deployment but MUST be closed before the device is loaned,
gifted, or sold:

- [ ] Switch to `pysqlcipher3` or move to LUKS-encrypted home for the
  bridge user
- [ ] Plan a re-enroll path; embeddings don't migrate cleanly across
  encryption boundaries

## Deletion / "right to be forgotten"

- Voice intent `"forget Brett"` or `"forget Brett's face"` →
  `self.camera.face_forget(name="Brett")` → bridge deletes the row.
- Voice intent `"forget everyone"` → `self.camera.face_forget(name="*")`
  → `forgetAll()` → wipes the table.
- Direct API: `POST /api/face/forget {"name": "Brett"}` or
  `{"name": "*"}`.
- Full reset: `rm /root/.zeroclaw/faces.sqlite` and restart the bridge —
  the schema is recreated on next boot.

There is no soft-delete or tombstone; `forget` is destructive and
immediate.

## Cross-reference: privacy-indicator LEDs (Layer 1)

The face-tracking modifier already drives the left LED green while a face
is in frame (see `face_tracking.cpp::setTrackingLed`). Recognition reuses
that same LED — there is no additional indicator for "the robot is
identifying you," because identification only fires when a face is
already detected and the green LED is already lit.

The hardware-guaranteed privacy LED for the camera peripheral remains
a tracked-but-deferred prereq (see `dotty-private/tasks.md`); face
detection shipped without it on 2026-04-25, so face recognition follows
the same posture. The LED gap MUST be closed before the device leaves
Brett's household.

## v1 trust assumptions (family-only)

These are conscious deviations from the safer-by-default design,
acceptable only because this is a single-household deployment:

- **No parental gate on enrollment.** The original design required a
  PIN or long-press; v1 lets any voice say "this is X, take a photo of
  me". The trade-off: kids can enroll friends. Re-enable the gate
  before any non-family deployment.
- **No voice ID.** "Forget my face" cannot be resolved because we don't
  know which speaker is talking. The voice handler asks the user to
  re-phrase as "forget [name]".
- **No multi-frame averaging.** v1 stores one embedding per enrollment.
  Re-enrolling the same name overwrites and bumps `n_samples`; future
  averaging across `n_samples` could improve accuracy.

## Audit trail

Bridge logs INFO-level entries on every enroll, recognize, and forget
call. Logs include the name and confidence; they do **not** include
embeddings or JPEG bytes. They live in `journalctl -u zeroclaw-bridge`
on the RPi (and never leave the box).

## Open follow-ups (server-side variant)

- [ ] Re-enable parental gate before any non-family deployment.
- [ ] Switch SQLite to encrypted-at-rest (sqlcipher or LUKS).
- [ ] Bench accuracy on Hudson + Annabelle; if false-negatives > 10 %,
  swap `face_recognition` for `insightface` (ArcFace 512-d).
- [ ] Wire a `/api/face/last-action/{device_id}` poll endpoint so the
  voice handler can speak real success/failure instead of optimistic
  acks.
- [ ] Hardware-guaranteed camera privacy LED (Layer 1, separate task).

## Open follow-ups (on-device fallback, deferred)

- [ ] Wire ESP-DL `face_recognition.so` in `FaceRecognizer::computeEmbedding`
  if/when we migrate compute back on-device.
- [ ] Enable NVS encryption (menuconfig + partition table) — only
  relevant under the on-device path.
- [ ] Move `kEnrollPIN` from constexpr to menuconfig + hashed compare
  — only relevant under the on-device path.
