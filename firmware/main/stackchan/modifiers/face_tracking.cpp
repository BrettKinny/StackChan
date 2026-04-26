/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_tracking.h"
#include "../stackchan.h"
#include "idle_motion.h"
#include "application.h"  // Phase 1.2: server-bound perception events

#include <cmath>

namespace stackchan {

// ---------------------------------------------------------------------------
// Tracking tuning knobs — keep all magic numbers here so reverts are one-line.
//
// kEmaAlpha           : EMA smoothing factor for face center.
//                       Previous value 0.3f produced visibly laggy tracking
//                       at ~3 fps detection. 0.5f reacts faster while still
//                       filtering single-frame jitter. Range [0.0..1.0],
//                       higher = more responsive, less smoothing.
//
// kLookAtSpeed        : Servo move speed (deg/sec) handed to
//                       Motion::lookAtNormalized(). Previous 350 was slow
//                       enough to lag a moving face; 500 keeps up better.
//                       The motion layer clamps so values aren't unbounded.
//
// kDeadbandFrac       : Minimum fractional change in normalized x/y before
//                       we re-issue a servo command. Frame-normalized coords
//                       are in [-1..1], so a "frame-width" delta = 2.0; this
//                       constant is fraction of that span. 0.06f ≈ 3% of
//                       frame width on each axis. Below this, micro-jitter
//                       from the detector chatters the gearbox for no gain.
// ---------------------------------------------------------------------------
static constexpr float kEmaAlpha     = 0.5f;   // was 0.3f
static constexpr int   kLookAtSpeed  = 500;    // was 350
static constexpr float kDeadbandFrac = 0.06f;  // was effectively 0 (no deadband)

FaceTrackingModifier::FaceTrackingModifier()
{
    // Pull EMA alpha from the constexpr at top of file so reverts only
    // touch one location.
    _alpha = kEmaAlpha;
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
                // Force first command to fire regardless of deadband when
                // we (re-)acquire a face after an idle gap.
                _last_cmd_valid = false;
                pauseIdleMotion();
                setTrackingLed(stackchan, true);
                Application::GetInstance().SendEvent("face_detected", "{}");
                // Open the mic on face acquisition — same path as a
                // wake-word detection. The device transitions to
                // Listening (auto-stop / VAD-driven), so a short
                // window of silence returns to idle naturally; if the
                // user speaks within the window, the normal chat
                // flow takes over. The bridge's inject-text greeting
                // then interrupts with "Hi!" and listening resumes
                // post-TTS. Tag with "face" so server logs can tell
                // this trigger from a real wake-word detection.
                Application::GetInstance().WakeWordInvoke("face");
            }
            break;

        case State::Tracking:
            if (detected) {
                _smooth_x += _alpha * (raw_x - _smooth_x);
                _smooth_y += _alpha * (raw_y - _smooth_y);
                _maybeIssueLookAt(stackchan);
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
                _maybeIssueLookAt(stackchan);
                _last_face_time = now;
            } else if (now - _grace_start > _grace_period_ms) {
                _state = State::Idle;
                _last_cmd_valid = false;
                resumeIdleMotion();
                setTrackingLed(stackchan, false);
                Application::GetInstance().SendEvent("face_lost", "{}");
            }
            break;
    }
}

void FaceTrackingModifier::pauseIdleMotion()
{
    auto* idle = static_cast<IdleMotionModifier*>(
        ::GetStackChan().getModifierByName(IdleMotionModifier::kName));
    if (idle) idle->pause();
}

void FaceTrackingModifier::resumeIdleMotion()
{
    auto* idle = static_cast<IdleMotionModifier*>(
        ::GetStackChan().getModifierByName(IdleMotionModifier::kName));
    if (idle) idle->resume();
}

void FaceTrackingModifier::_maybeIssueLookAt(Modifiable& stackchan)
{
    // Deadband: skip the servo command if the smoothed target moved less
    // than kDeadbandFrac of the (full) normalized span on each axis since
    // the last command. Prevents detector jitter (~1-2 px bbox shimmer)
    // from chattering the gearbox while the user holds still.
    if (_last_cmd_valid) {
        float dx = std::fabs(_smooth_x - _last_cmd_x);
        float dy = std::fabs(_smooth_y - _last_cmd_y);
        if (dx < kDeadbandFrac && dy < kDeadbandFrac) {
            return;  // inside deadband — keep current servo target
        }
    }
    stackchan.motion().lookAtNormalized(_smooth_x, _smooth_y, kLookAtSpeed);
    _last_cmd_x = _smooth_x;
    _last_cmd_y = _smooth_y;
    _last_cmd_valid = true;
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
