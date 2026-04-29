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

// Phase 3 — chat-state profile pushed by stackchan_display.cc::SetStatus
// on each chat-state transition. Drives EMA smoothing, idle-overlay
// cadence/amplitude, and whether face acquisition fires WakeWordInvoke.
struct ChatProfile {
    float    alpha;                  // EMA smoothing factor for face center
    uint32_t overlay_min_ms;         // tracking-overlay cadence (idle_motion)
    uint32_t overlay_max_ms;
    float    amplitude_scale;        // scale on overlay deltas (1.0 = Phase 2 default)
    bool     allow_wake_word_invoke; // only true for the IDLE profile
};

// Per-chat-state profile table. `inline constexpr` so the definitions are
// visible across translation units (stackchan_display.cc picks the right
// one to push, face_tracking.cpp uses kChatProfileIdle as the constructor
// default). amplitude_scale is relative to Phase 2's halved tracking-
// overlay range (±75°/±40°). allow_wake_word_invoke is IDLE-only so a
// face re-acquired mid-chat doesn't stomp on the running session.
inline constexpr ChatProfile kChatProfileIdle      { 0.7f, 12000, 24000, 1.0f, true  };
inline constexpr ChatProfile kChatProfileListening { 0.8f,  8000, 12000, 1.0f, false };
inline constexpr ChatProfile kChatProfileSpeaking  { 0.4f, 30000, 45000, 0.5f, false };

class FaceTrackingModifier : public Modifier {
public:
    static constexpr const char* kName = "face_tracking";

    FaceTrackingModifier();
    ~FaceTrackingModifier() override;
    void _update(Modifiable& stackchan) override;
    const char* name() const override { return kName; }

    // Phase 3 — push a new chat profile. Updates _alpha + _profile, hands
    // the overlay tunables through to IdleMotionModifier, and stores the
    // wake-word gate. Safe to call from the main task on chat-state
    // transitions; reads on Core 1 (the modifier loop) are non-atomic but
    // tearing here is benign (worst case: one tick uses a partially-
    // updated profile, self-corrects on the next tick).
    void setChatProfile(const ChatProfile& profile);

private:
    enum class State { Idle, Tracking, GracePeriod };

    // IdleMotionModifier is resolved by stable name on each call rather
    // than cached at construction. Caching the pool ID was unsound: the
    // pool's free-list reuses slots, so if IdleMotionModifier was
    // destroyed and recreated (e.g. round-tripping through a non-idle
    // status), the ID held here would no longer point at the live
    // instance and the call would silently no-op.
    //
    // Phase 2 — single setter replaces the prior pauseIdleMotion /
    // resumeIdleMotion pair. true → idle modifier emits the reduced
    // overlay action set; false → full idle motion resumes.
    void setIdleTrackingMode(bool tracking);
    // Issues a servo command toward (_smooth_x, _smooth_y) at kLookAtSpeed,
    // but only if the move clears the deadband vs. last commanded target.
    void _maybeIssueLookAt(Modifiable& stackchan);

    State _state        = State::Idle;
    float _smooth_x     = 0;
    float _smooth_y     = 0;
    // EMA smoothing factor. Initialized from kEmaAlpha (face_tracking.cpp)
    // in the constructor — the constexpr at the top of the .cpp is the
    // single source of truth. Default here is a conservative fallback only
    // in case someone constructs this struct without running the ctor body.
    float _alpha        = 0.5f;
    // Last servo command target — used to apply a deadband and skip
    // sub-pixel jitter updates that just chatter the gearbox.
    float _last_cmd_x        = 0;
    float _last_cmd_y        = 0;
    bool  _last_cmd_valid    = false;
    uint32_t _last_face_time = 0;
    uint32_t _grace_start    = 0;
    // Shortened from 2000 ms so face_lost fires quickly after the
    // user leaves frame — the bridge's perception bus listens for
    // that event and aborts any in-flight TTS so Dotty doesn't talk
    // to empty space. 800 ms still gives ~2-3 frames at ~3 fps face
    // detection to re-acquire during small head movements before
    // flipping back to idle.
    uint32_t _grace_period_ms = 800;

    // Phase 3 — active chat profile. Initialised from kChatProfileIdle
    // (face_tracking.cpp) in the constructor; updated by setChatProfile.
    // _alpha is also a member because the EMA expression in _update reads
    // it inline; setChatProfile keeps both _alpha and _profile.alpha in sync.
    ChatProfile _profile;

    // Capture-pending guard — held across the face_detected → take_photo
    // round-trip so the head doesn't drift between detection and the
    // server-driven still capture. Acquired on Idle→Tracking emit, released
    // on (a) the next observed Capture (lastCaptureTimestampMs ticks),
    // (b) face_lost, (c) kCaptureGuardTimeoutMs after acquire (defensive
    // ceiling when take_photo never arrives — bridge container down, etc.),
    // or (d) modifier teardown via destructor. Refresh-only on overlapping
    // face_detected (no double-acquire — Motion::setModifyLock is refcounted
    // but our outer guard still wants single-ownership semantics so we
    // don't lose the inner Capture's release).
    bool     _capture_guard_held              = false;
    uint32_t _capture_guard_acquired_ms       = 0;
    uint32_t _capture_guard_baseline_capture_ts = 0;

    // Decremented per actually-issued lookAt command (not per tick) so
    // deadband-skipped ticks don't burn the throttle budget. While > 0
    // _maybeIssueLookAt uses kPostReleaseLookAtSpeed instead of
    // kLookAtSpeed. See face_tracking.cpp constants block for the
    // bench history that motivated this.
    uint8_t _post_release_throttle = 0;

    // Phase 0 instrumentation — counters reset every kPhase0WindowMs.
    // See probes/face-tracking-naturalness.md for the bench procedure.
    // Cheap to leave in place; one ESP_LOGI line per 5 s window.
    uint32_t _phase0_window_start_ms = 0;
    uint32_t _phase0_last_tick_ms    = 0;
    uint32_t _phase0_last_seen_ts    = 0;
    uint32_t _phase0_samples         = 0;
    uint32_t _phase0_det             = 0;
    uint32_t _phase0_unique_frames   = 0;
    uint32_t _phase0_track_ms        = 0;
    uint32_t _phase0_cmd             = 0;
};

}  // namespace stackchan
