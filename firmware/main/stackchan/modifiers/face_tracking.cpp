/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_tracking.h"
#include "../stackchan.h"
#include "idle_motion.h"

namespace stackchan {

FaceTrackingModifier::FaceTrackingModifier(int idle_motion_modifier_id)
    : _idle_motion_id(idle_motion_modifier_id)
{
}

void FaceTrackingModifier::_update(Modifiable& stackchan)
{
    uint32_t now = GetHAL().millis();
    auto& result = GetFaceDetectionResult();

    bool detected = false;
    float raw_x = 0, raw_y = 0, size = 0;
    uint32_t ts = 0;

    if (!result.read(detected, raw_x, raw_y, size, ts)) return;

    switch (_state) {
        case State::Idle:
            if (detected) {
                _state = State::Tracking;
                _smooth_x = raw_x;
                _smooth_y = raw_y;
                pauseIdleMotion();
                setTrackingLed(stackchan, true);
            }
            break;

        case State::Tracking:
            if (detected) {
                _smooth_x += _alpha * (raw_x - _smooth_x);
                _smooth_y += _alpha * (raw_y - _smooth_y);
                stackchan.motion().lookAtNormalized(_smooth_x, _smooth_y, 350);
                _last_face_time = now;
            } else {
                _state = State::GracePeriod;
                _grace_start = now;
            }
            break;

        case State::GracePeriod:
            if (detected) {
                _state = State::Tracking;
                _smooth_x += _alpha * (raw_x - _smooth_x);
                _smooth_y += _alpha * (raw_y - _smooth_y);
                stackchan.motion().lookAtNormalized(_smooth_x, _smooth_y, 350);
                _last_face_time = now;
            } else if (now - _grace_start > _grace_period_ms) {
                _state = State::Idle;
                resumeIdleMotion();
                setTrackingLed(stackchan, false);
            }
            break;
    }
}

void FaceTrackingModifier::pauseIdleMotion()
{
    auto* idle = static_cast<IdleMotionModifier*>(
        ::GetStackChan().getModifier(_idle_motion_id));
    if (idle) idle->pause();
}

void FaceTrackingModifier::resumeIdleMotion()
{
    auto* idle = static_cast<IdleMotionModifier*>(
        ::GetStackChan().getModifier(_idle_motion_id));
    if (idle) idle->resume();
}

void FaceTrackingModifier::setTrackingLed(Modifiable& stackchan, bool on)
{
    // Left LED green = face currently detected.
    // Right LED cyan (mode-active) is managed by stackchan_display, not here.
    if (on) {
        stackchan.leftNeonLight().setColor(0, 168, 0);
    } else {
        stackchan.leftNeonLight().setColor(0, 0, 0);
    }
}

}  // namespace stackchan
