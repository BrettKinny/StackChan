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
    FaceTrackingModifier(int idle_motion_modifier_id);
    void _update(Modifiable& stackchan) override;

private:
    enum class State { Idle, Tracking, GracePeriod };

    void pauseIdleMotion();
    void resumeIdleMotion();
    void setTrackingLed(Modifiable& stackchan, bool on);

    State _state        = State::Idle;
    int _idle_motion_id = -1;
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
