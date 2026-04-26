/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_tracking.h"
#include "../stackchan.h"
#include "idle_motion.h"
#include "application.h"  // Phase 1.2: server-bound perception events

#include "esp_log.h"

#include <cmath>

namespace stackchan {

static const char* TAG = "face_tracking";

// Phase 0 instrumentation window (see probes/face-tracking-naturalness.md).
static constexpr uint32_t kPhase0WindowMs = 5000;

// Phase 3 chat profiles (kChatProfileIdle / Listening / Speaking) live in
// face_tracking.h as inline constexpr so stackchan_display.cc::SetStatus
// can pick the right one and pass it via setChatProfile().

// ---------------------------------------------------------------------------
// Tracking tuning knobs — keep all magic numbers here so reverts are one-line.
//
// kEmaAlpha           : EMA smoothing factor for face center.
//                       History: 0.3f → 0.5f → 0.7f. 0.3f was visibly laggy
//                       at ~3 fps detection; 0.5f filtered too much of the
//                       small natural sway we want to track; 0.7f keeps the
//                       single-frame-jitter rejection but reacts to slow
//                       drifts after only one or two frames. Range
//                       [0.0..1.0], higher = more responsive.
//                       Revert to 0.5f if servo motion looks twitchy.
//
// kLookAtSpeed        : Servo move speed (deg/sec) handed to
//                       Motion::lookAtNormalized(). 500 keeps up with a
//                       moving face without visible overshoot at close
//                       range. The motion layer clamps so values aren't
//                       unbounded.
//
// kDeadbandFrac       : Minimum fractional change in normalized x/y before
//                       we re-issue a servo command. Frame-normalized coords
//                       are in [-1..1], so a "frame-width" delta = 2.0; this
//                       constant is fraction of that span.
//                       History: 0.06f (≈ 3% of full span on each axis) was
//                       large enough that breathing / millimetre head sway
//                       never cleared it, producing the "head freezes once
//                       locked" feel. 0.02f (≈ 1% per axis) lets natural
//                       small drifts move the servos while still rejecting
//                       sub-pixel detector chatter.
//                       Revert to 0.06f if gearbox audibly chatters.
// ---------------------------------------------------------------------------
static constexpr float kEmaAlpha     = 0.7f;   // history: 0.3f → 0.5f → 0.7f (Phase 1)
static constexpr int   kLookAtSpeed  = 500;    // unchanged in Phase 1
static constexpr float kDeadbandFrac = 0.02f;  // history: 0.06f → 0.02f (Phase 1)

FaceTrackingModifier::FaceTrackingModifier()
{
    // Phase 3 — initial profile is IDLE (matches the state at construction;
    // stackchan_display.cc::SetStatus pushes the live profile on first call).
    // _alpha tracks _profile.alpha for the inline EMA expression in _update.
    _profile = kChatProfileIdle;
    _alpha   = _profile.alpha;
    // Phase 1 constexpr kEmaAlpha is now the IDLE-profile alpha; if the
    // profile API ever has to be backed out, restore _alpha = kEmaAlpha here.
}

void FaceTrackingModifier::setChatProfile(const ChatProfile& profile)
{
    _profile = profile;
    _alpha   = profile.alpha;
    // Push overlay tunables into IdleMotionModifier — kept in sync so the
    // overlay cadence/amplitude tracks the chat state without face_tracking
    // having to reach into idle_motion's internals on every tick.
    auto* idle = static_cast<IdleMotionModifier*>(
        ::GetStackChan().getModifierByName(IdleMotionModifier::kName));
    if (idle) {
        idle->setIntervalRange(profile.overlay_min_ms, profile.overlay_max_ms);
        idle->setAmplitudeScale(profile.amplitude_scale);
    }
}

void FaceTrackingModifier::_update(Modifiable& stackchan)
{
    uint32_t now = GetHAL().millis();
    auto& result = GetFaceDetectionResult();

    bool detected = false;
    float raw_x = 0, raw_y = 0, size = 0;
    uint32_t ts = 0;

    if (!result.read(detected, raw_x, raw_y, size, ts)) return;

    // ---- Phase 0 instrumentation: counters update -----------------------
    // Lazy-init window start on first call so the first window is full-length.
    if (_phase0_window_start_ms == 0) _phase0_window_start_ms = now;

    // Time-in-Tracking accumulator: skip the very first tick (no prior tick
    // to delta against), then add dt to track_ms only while in Tracking.
    if (_phase0_last_tick_ms != 0) {
        uint32_t dt = now - _phase0_last_tick_ms;
        if (_state == State::Tracking) _phase0_track_ms += dt;
    }
    _phase0_last_tick_ms = now;

    _phase0_samples++;
    if (detected) _phase0_det++;
    // Detector FPS = unique-frame count / window. Rising ts means the
    // detector wrote a new frame between this tick and the last.
    if (ts != _phase0_last_seen_ts) {
        _phase0_unique_frames++;
        _phase0_last_seen_ts = ts;
    }
    // ---------------------------------------------------------------------

    switch (_state) {
        case State::Idle:
            if (detected) {
                _state = State::Tracking;
                _smooth_x = raw_x;
                _smooth_y = raw_y;
                // Force first command to fire regardless of deadband when
                // we (re-)acquire a face after an idle gap.
                _last_cmd_valid = false;
                setIdleTrackingMode(true);
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
                //
                // Phase 3 — gated on the IDLE profile only. With the
                // modifier now alive across LISTENING/SPEAKING, a face
                // re-acquired mid-chat must NOT re-trigger wake-word —
                // that would interrupt the running session.
                if (_profile.allow_wake_word_invoke) {
                    Application::GetInstance().WakeWordInvoke("face");
                }
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
                setIdleTrackingMode(false);
                setTrackingLed(stackchan, false);
                Application::GetInstance().SendEvent("face_lost", "{}");
            }
            break;
    }

    // ---- Phase 0 instrumentation: window emit ---------------------------
    // One ESP_LOGI line per ~5 s window, then reset counters. Format
    // documented in probes/face-tracking-naturalness.md §3.
    uint32_t window_age = now - _phase0_window_start_ms;
    if (window_age >= kPhase0WindowMs) {
        float det_pct   = _phase0_samples ? (100.0f * _phase0_det / _phase0_samples) : 0.0f;
        float fps       = window_age      ? (1000.0f * _phase0_unique_frames / window_age) : 0.0f;
        float track_pct = window_age      ? (100.0f * _phase0_track_ms / window_age) : 0.0f;
        ESP_LOGI(TAG,
            "phase0 win_ms=%u samples=%u det=%u (%.1f%%) fps=%.1f track_ms=%u (%.1f%%) cmd=%u",
            (unsigned)window_age,
            (unsigned)_phase0_samples,
            (unsigned)_phase0_det, det_pct,
            fps,
            (unsigned)_phase0_track_ms, track_pct,
            (unsigned)_phase0_cmd);

        _phase0_window_start_ms = now;
        _phase0_samples         = 0;
        _phase0_det             = 0;
        _phase0_unique_frames   = 0;
        _phase0_track_ms        = 0;
        _phase0_cmd             = 0;
    }
    // ---------------------------------------------------------------------
}

void FaceTrackingModifier::setIdleTrackingMode(bool tracking)
{
    auto* idle = static_cast<IdleMotionModifier*>(
        ::GetStackChan().getModifierByName(IdleMotionModifier::kName));
    if (idle) idle->setTrackingMode(tracking);
}

void FaceTrackingModifier::_maybeIssueLookAt(Modifiable& stackchan)
{
    // Cooperative motion lock — held by StackChanCamera::Capture() during
    // a still so the head stays put for the shutter. EMA / state machine
    // continue updating in _update() so the next post-lock command targets
    // the fresh face position rather than a stale pre-shutter one.
    if (stackchan.motion().isModifyLocked()) {
        return;
    }
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
    _phase0_cmd++;  // Phase 0 instrumentation — count actually-issued commands.
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
