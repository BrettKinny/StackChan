/*
 * SPDX-FileCopyrightText: 2026 Brett Kinny / squarewavesystems
 *
 * SPDX-License-Identifier: MIT
 *
 * RAII refcount guard for the camera V4L2 stream lifecycle.
 *
 * Acquiring a guard tells the camera subsystem "a consumer is reading
 * frames" — the V4L2 stream is asserted (VIDIOC_STREAMON via
 * StackChanCamera::startStreaming() on the 0→1 refcount transition).
 * Releasing the guard decrements the refcount; on 1→0 the stream is
 * torn down (VIDIOC_STREAMOFF).
 *
 * Two concurrent consumers (face detector + MCP take_photo) compose
 * cleanly: the second-in finds a hot stream and just bumps the count;
 * the second-out leaves the stream up for the first-in.
 *
 * Design invariants:
 *
 *   - Construction may block for up to ~5 s on the first acquire after
 *     boot while the ISP autoexposure warmup runs. Subsequent acquires
 *     return immediately.
 *   - Non-movable, non-copyable. Guard must be constructed and destroyed
 *     on the same FreeRTOS task (recursive mutex enforces this).
 *
 * Wired into:
 *   - StackChanCamera::Capture (MCP take_photo path) — guard scoped to
 *     the entire capture.
 *   - FaceDetector::taskEntry — guard scoped to the entire _enabled
 *     window so the stream stays up across many processFrame cycles.
 */
#pragma once

namespace stackchan::camera {

class CameraStreamGuard {
public:
    CameraStreamGuard();
    ~CameraStreamGuard();

    CameraStreamGuard(const CameraStreamGuard&)            = delete;
    CameraStreamGuard& operator=(const CameraStreamGuard&) = delete;
    CameraStreamGuard(CameraStreamGuard&&)                 = delete;
    CameraStreamGuard& operator=(CameraStreamGuard&&)      = delete;
};

}  // namespace stackchan::camera
