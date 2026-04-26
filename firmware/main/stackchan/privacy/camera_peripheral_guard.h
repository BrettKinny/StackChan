/*
 * SPDX-FileCopyrightText: 2026 Brett Kinny / squarewavesystems
 *
 * SPDX-License-Identifier: MIT
 *
 * RAII refcount guard for the camera peripheral.
 *
 * Acquiring a guard tells the privacy subsystem "a consumer is reading
 * camera frames" — the V4L2 stream is asserted (VIDIOC_STREAMON via
 * StackChanCamera::startStreaming() on the 0→1 refcount transition) and
 * the red privacy LED lights. Releasing the guard decrements the
 * refcount; on 1→0 the stream is torn down (VIDIOC_STREAMOFF) and the
 * LED extinguishes.
 *
 * Two concurrent consumers (face detector + MCP take_photo) compose
 * cleanly: the second-in finds a hot stream and just bumps the count;
 * the second-out leaves the stream up for the first-in. The privacy
 * indicator therefore tracks "any consumer is active" rather than any
 * single code path.
 *
 * Design invariants:
 *
 *   - Construction may block for up to ~5 s on the first acquire after
 *     boot while the ISP autoexposure warmup runs. Subsequent acquires
 *     return immediately.
 *   - Non-movable, non-copyable. Guard must be constructed and destroyed
 *     on the same FreeRTOS task (recursive mutex enforces this).
 *   - The mutators on PrivacyLeds (setCameraState) are friend-restricted
 *     to this class — a compromised MCP server cannot drive the LED
 *     without also driving the V4L2 stream.
 *
 * Wired into:
 *   - StackChanCamera::Capture (MCP take_photo path) — guard scoped to
 *     the entire capture.
 *   - FaceDetector::taskEntry — guard scoped to the entire _enabled
 *     window so the LED stays steady across many processFrame cycles.
 */
#pragma once

#include "privacy_leds.h"

namespace stackchan::privacy {

class CameraPeripheralGuard {
public:
    CameraPeripheralGuard();
    ~CameraPeripheralGuard();

    CameraPeripheralGuard(const CameraPeripheralGuard&)            = delete;
    CameraPeripheralGuard& operator=(const CameraPeripheralGuard&) = delete;
    CameraPeripheralGuard(CameraPeripheralGuard&&)                 = delete;
    CameraPeripheralGuard& operator=(CameraPeripheralGuard&&)      = delete;
};

}  // namespace stackchan::privacy
