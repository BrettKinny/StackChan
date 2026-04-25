/*
 * SPDX-FileCopyrightText: 2026 Brett Kinny / squarewavesystems
 *
 * SPDX-License-Identifier: MIT
 *
 * RAII guard for the camera peripheral.
 *
 * IMPORTANT — current scope is INTENT-LAYER ONLY:
 *
 *   In an ideal world this guard would call VIDIOC_STREAMON in its
 *   constructor and VIDIOC_STREAMOFF in its destructor, and the LED
 *   would only light when the V4L2 driver is actually feeding DMA
 *   buffers. That is the L362 fix in tasks.md.
 *
 *   The current ESP-DL face-detect pipeline (FaceDetector::processFrame)
 *   assumes the camera streams permanently after init — it just
 *   dequeues / requeues mmap buffers via VIDIOC_DQBUF / VIDIOC_QBUF.
 *   Adding STREAMON/STREAMOFF around every detection frame would
 *   require either:
 *     (a) restarting the V4L2 stream every ~200 ms (frame rate hit +
 *         re-init of ISP autoexpose),
 *     (b) switching face-detect to an explicit "begin / end stream"
 *         lifecycle so STREAMON is asserted once when detection is
 *         enabled and STREAMOFF is asserted once when it is disabled.
 *
 *   Option (b) is the right answer but is DEFERRED — see PRIVACY_LEDS.md.
 *
 *   For now, this guard only updates PrivacyLeds::CameraState. It is
 *   wired into:
 *     - StackChanCamera::Capture (the take_photo path) — guard lives
 *       for the duration of the capture
 *     - StackChanCamera::StreamCaptures (the face-detect path) — guard
 *       lives for the duration of one capture cycle
 *
 *   Once option (b) lands, the constructor here will issue the
 *   STREAMON ioctl directly and the destructor will issue STREAMOFF.
 *   At that point the LED becomes hardware-guaranteed.
 */
#pragma once

#include "privacy_leds.h"

namespace stackchan::privacy {

class CameraPeripheralGuard {
public:
    CameraPeripheralGuard()
    {
        PrivacyLeds::getInstance().setCameraState(CameraState::Active);
    }

    ~CameraPeripheralGuard()
    {
        PrivacyLeds::getInstance().setCameraState(CameraState::Off);
    }

    CameraPeripheralGuard(const CameraPeripheralGuard&)            = delete;
    CameraPeripheralGuard& operator=(const CameraPeripheralGuard&) = delete;
    CameraPeripheralGuard(CameraPeripheralGuard&&)                 = delete;
    CameraPeripheralGuard& operator=(CameraPeripheralGuard&&)      = delete;
};

}  // namespace stackchan::privacy
