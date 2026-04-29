/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_tracking.h"
#include "../stackchan.h"
#include "idle_motion.h"
#include "../modes/state_manager.h"  // Phase 4: IDLE <-> TALK transitions
#include "application.h"  // Phase 1.2: server-bound perception events
#include <hal/board/hal_bridge.h>
#include <hal/board/stackchan_camera.h>

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

// Post-capture-release throttle. Bench trace 2026-04-29 caught the
// "take a photo, immediately turn away" symptom as a single
// face_tracking lookAt at speed=500 issued in the same millisecond
// the capture-pending guard released — with a prior idle_motion move
// still in flight (isMoving=1). The fast spring under the prior
// trajectory's velocity reads as a violent snap. We use a softer
// spring (kPostReleaseLookAtSpeed) for the first kPostReleaseThrottle
// commands actually issued after release; the deadband + last_cmd
// re-seed at release time can also suppress the first command
// entirely if the user hasn't moved much during the lock window.
//
// Counter decrements per *issued* command, not per tick — otherwise
// a deadband-skipped tick would burn the throttle budget without
// taking a turn. Soft enough that subsequent normal-speed tracking
// resumes within a fraction of a second.
static constexpr int kPostReleaseLookAtSpeed = 200;
static constexpr uint8_t kPostReleaseThrottle = 2;

// Capture-pending guard ceiling. Bridge's typical first take_photo round-trip
// is 200-800 ms; 5 s gives ~6× headroom and unblocks idle motion before the
// user notices the freeze if take_photo never arrives (bridge container down,
// network blip, etc). Lower this if the per-walk-in head freeze feels too
// long when the bridge is unreachable.
static constexpr uint32_t kCaptureGuardTimeoutMs = 5000;

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

    // ---- Capture-pending guard release check ---------------------------
    // Runs every tick before any state-machine work. Releases the outer
    // motion lock when one of: (a) Capture() ran (its inner MotionPauseGuard
    // tick advances lastCaptureTimestampMs), (b) timeout elapsed.
    // face_lost release is handled inside the GracePeriod expiry branch.
    //
    // On release, if a face is currently detected, we re-seed _smooth_x/y
    // from the live raw position. Otherwise the EMA values that drifted
    // during the lock window (head frozen, but face_detection_result kept
    // updating) become the next servo target — which produces a fast snap
    // toward the EMA-blended position the moment commands resume. The
    // re-seed makes the post-freeze command target the actual current
    // face position with no drift accumulated, eliminating the visible
    // "spin after photo" catch-up.
    if (_capture_guard_held) {
        uint32_t held_ms = now - _capture_guard_acquired_ms;
        auto* cam = hal_bridge::board_get_camera();
        uint32_t cur_capture_ts = cam ? cam->lastCaptureTimestampMs() : 0;
        if (cur_capture_ts != 0 && cur_capture_ts != _capture_guard_baseline_capture_ts) {
            stackchan.motion().setModifyLock(false);
            _capture_guard_held = false;
            if (detected) {
                _smooth_x = raw_x;
                _smooth_y = raw_y;
                // Re-seed last_cmd from the live face position so the
                // deadband can suppress the first post-release command
                // when the user hasn't moved much during the lock window.
                // _last_cmd_valid was reset on Idle→Tracking; without
                // this seed the first command always fires no matter
                // how small the delta. See kPostReleaseLookAtSpeed
                // comment for the wider context.
                _last_cmd_x = raw_x;
                _last_cmd_y = raw_y;
                _last_cmd_valid = true;
            }
            _post_release_throttle = kPostReleaseThrottle;
            ESP_LOGI(TAG, "capture-pending guard released (capture observed dt=%u ms)",
                     (unsigned)held_ms);
        } else if (held_ms > kCaptureGuardTimeoutMs) {
            stackchan.motion().setModifyLock(false);
            _capture_guard_held = false;
            if (detected) {
                _smooth_x = raw_x;
                _smooth_y = raw_y;
                // Same rationale as the observed-capture branch — after
                // a 5 s timeout the head has been frozen even longer,
                // so a softer first command matters more, not less.
                _last_cmd_x = raw_x;
                _last_cmd_y = raw_y;
                _last_cmd_valid = true;
            }
            _post_release_throttle = kPostReleaseThrottle;
            ESP_LOGW(TAG, "capture-pending guard timeout-release (%u ms — take_photo never arrived)",
                     (unsigned)held_ms);
        }
    }
    // ---------------------------------------------------------------------

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
                // Acquire capture-pending guard BEFORE emitting face_detected
                // so the head is already locked by the time the bridge sees
                // the event and dispatches its take_photo MCP call. The guard
                // holds the motion lock until the next tick observes either
                // (a) Capture() ran (lastCaptureTimestampMs advances), (b)
                // face_lost fires (GracePeriod expiry branch below), or (c)
                // kCaptureGuardTimeoutMs elapses. Without this, IdleMotion
                // overlay drifts and FaceTracking lookAt commands move the
                // head between SendEvent and Capture, producing the
                // "no one in view" failure mode.
                if (!_capture_guard_held) {
                    auto* cam = hal_bridge::board_get_camera();
                    _capture_guard_baseline_capture_ts = cam ? cam->lastCaptureTimestampMs() : 0;
                    stackchan.motion().setModifyLock(true);
                    _capture_guard_held = true;
                    _capture_guard_acquired_ms = now;
                    ESP_LOGI(TAG, "capture-pending guard acquired (face_detected baseline_ts=%u)",
                             (unsigned)_capture_guard_baseline_capture_ts);
                } else {
                    // Defensive — refresh the timeout window if a previous
                    // guard somehow leaked through (shouldn't happen given
                    // the state machine de-dupes face_detected).
                    _capture_guard_acquired_ms = now;
                }
                Application::GetInstance().SendEvent("face_detected", "{}");
                // Phase 4 — transition IDLE -> TALK locally so the state pip
                // updates immediately, without waiting on the bridge round-trip.
                // StateManager itself decides whether to act (sticky states like
                // STORY_TIME / SECURITY / SLEEP / DANCE are intentionally
                // unaffected by camera edges).
                if (auto* sm = static_cast<StateManager*>(
                        ::GetStackChan().getModifierByName(StateManager::kName))) {
                    sm->onFaceDetected();
                }
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
                // Gate WakeWordInvoke on the authoritative device state.
                // The earlier _profile.allow_wake_word_invoke gate was IDLE-only
                // by design but lagged: stackchan_display.cc::SetStatus pushes
                // LISTENING/SPEAKING into the profile, and on a flickering
                // walk-in the modifier can cycle Idle->Tracking->GracePeriod->Idle
                // (800 ms grace) faster than the first invoke's state propagates
                // back. Result observed 2026-04-29: two AfeWakeWord encodes per
                // walk-in, two TTS chunks ~280 ms apart, garbled playback.
                // GetDeviceState() reflects the device state machine directly,
                // so the second acquisition while the first session is still
                // SPEAKING/LISTENING is suppressed at the call site.
                if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
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
                // Release capture-pending guard if take_photo never arrived
                // before the user walked away. Capturing a frame of empty
                // wall is the exact failure mode this guard is trying to
                // prevent.
                if (_capture_guard_held) {
                    stackchan.motion().setModifyLock(false);
                    _capture_guard_held = false;
                    ESP_LOGI(TAG, "capture-pending guard released (face_lost)");
                }
                Application::GetInstance().SendEvent("face_lost", "{}");
                // Phase 4 — transition TALK -> IDLE on grace expiry. STORY_TIME /
                // SECURITY / SLEEP / DANCE all stay sticky here; they have their
                // own exit triggers.
                if (auto* sm = static_cast<StateManager*>(
                        ::GetStackChan().getModifierByName(StateManager::kName))) {
                    sm->onFaceLost();
                }
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
    int speed = kLookAtSpeed;
    if (_post_release_throttle > 0) {
        speed = kPostReleaseLookAtSpeed;
        _post_release_throttle--;
    }
    stackchan.motion().lookAtNormalized(_smooth_x, _smooth_y, speed, "face_tracking");
    _last_cmd_x = _smooth_x;
    _last_cmd_y = _smooth_y;
    _last_cmd_valid = true;
    _phase0_cmd++;  // Phase 0 instrumentation — count actually-issued commands.
}

FaceTrackingModifier::~FaceTrackingModifier()
{
    // Drop a held capture-pending guard on teardown so the motion lock
    // doesn't leak across modifier swaps (e.g. sleep entry, which removes
    // both face_tracking and idle_motion). Without this the lock would
    // stay incremented forever and idle_motion would never resume.
    if (_capture_guard_held) {
        ::GetStackChan().motion().setModifyLock(false);
        _capture_guard_held = false;
        ESP_LOGI(TAG, "capture-pending guard released (modifier teardown)");
    }
}

}  // namespace stackchan
