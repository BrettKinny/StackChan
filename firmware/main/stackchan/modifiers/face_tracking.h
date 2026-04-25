/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../face/face_detection_result.h"
#include <hal/hal.h>
#include <cstdint>

namespace stackchan {

class FaceTrackingModifier : public Modifier {
public:
    FaceTrackingModifier();
    void _update(Modifiable& stackchan) override;

private:
    enum class State { Idle, Tracking, GracePeriod };

    // IdleMotionModifier is resolved by stable name on each call rather
    // than cached at construction. Caching the pool ID was unsound: the
    // pool's free-list reuses slots, so if IdleMotionModifier was
    // destroyed and recreated (e.g. round-tripping through a non-idle
    // status), the ID held here would no longer point at the live
    // instance and pause/resume would silently no-op.
    void pauseIdleMotion();
    void resumeIdleMotion();
    void setTrackingLed(Modifiable& stackchan, bool on);

    State _state        = State::Idle;
    float _smooth_x     = 0;
    float _smooth_y     = 0;
    float _alpha        = 0.3f;
    uint32_t _last_face_time = 0;
    uint32_t _grace_start    = 0;
    // Shortened from 2000 ms so face_lost fires quickly after the
    // user leaves frame — the bridge's perception bus listens for
    // that event and aborts any in-flight TTS so Dotty doesn't talk
    // to empty space. 800 ms still gives ~2-3 frames at ~3 fps face
    // detection to re-acquire during small head movements before
    // flipping back to idle.
    uint32_t _grace_period_ms = 800;
};

}  // namespace stackchan
