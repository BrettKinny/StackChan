/*
 * SPDX-FileCopyrightText: 2026 Dotty
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include <cstdint>

namespace stackchan {

// Mutually exclusive top-level state. The conversation, motion profile, and
// avatar emotion are derived from this. Toggles (kid_mode, smart_mode)
// compose orthogonally and are tracked separately.
enum class State {
    IDLE,        // ambient awareness, no face locked
    TALK,        // conversation engaged
    STORY_TIME,  // long-running interactive story
    SECURITY,    // wide deliberate scan, serious face
    SLEEP,       // servos parked, ambient awareness paused
    DANCE,       // transient performance
};

// StateManager owns the high-level mode for the device:
//   - State pip on left ring index 0   (mutually exclusive state colour).
//   - Toggle pips on right ring 8 / 9  (kid_mode warm pink, smart_mode orange).
//   - IdleMotionModifier::IdleProfile  (NORMAL / SURVEILLANCE / SLEEPY).
//   - "state_changed" perception event for bridge consumers.
//
// The pips are re-asserted at 5 Hz so chat-state full-ring writes (LISTENING /
// SPEAKING / STANDBY in stackchan_display.cc) don't permanently clobber them.
//
// face_tracking calls onFaceDetected / onFaceLost on detection edges so the
// IDLE <-> TALK transitions happen at the camera, not via the bridge round-trip.
class StateManager : public Modifier {
public:
    static constexpr const char* kName = "state_manager";

    // LED indices in the GLOBAL 12-pixel ring (left 0-5, right 6-11).
    // RightNeonLight uses LOCAL 0-5 internally and adds 6 — see
    // neon_light.cpp:103-112 — so the right-ring writes use local indices.
    static constexpr uint8_t kStatePipLeftIndex      = 0;  // global 0
    static constexpr uint8_t kKidModePipRightLocal   = 2;  // global 8
    static constexpr uint8_t kSmartModePipRightLocal = 3;  // global 9

    // 5 Hz re-assertion. Chat-state writes (set_left_leds in
    // stackchan_display.cc) repaint the whole left ring including index 0,
    // so the pip is restored within ~200 ms of any clobber.
    static constexpr uint32_t kReassertIntervalMs = 200;
    // 1 Hz flash for SECURITY: 500 ms on, 500 ms off.
    static constexpr uint32_t kSecurityFlashHalfMs = 500;

    const char* name() const override { return kName; }

    State currentState() const { return _state; }
    bool kidMode() const { return _kid_mode; }
    bool smartMode() const { return _smart_mode; }

    static const char* stateName(State s);
    static bool parseState(const char* s, State& out);

    // Idempotent — repeat-with-same-value is a no-op.
    void setState(State next);
    void setKidMode(bool enabled);
    void setSmartMode(bool enabled);

    // Camera-edge hooks. Called from face_tracking.cpp inside the same
    // tick as the SendEvent("face_detected"|"face_lost", ...) emissions.
    // STORY_TIME / SECURITY / DANCE are sticky and intentionally unaffected
    // by camera edges; SLEEP wakes on face_detected (Phase 5).
    void onFaceDetected();
    void onFaceLost();

    // Phase 5 — capacitive head-pet wake. Called from head_pet.cpp when a
    // press fires. No-op outside SLEEP; in SLEEP transitions to IDLE
    // (head-pet is non-conversational, so we don't auto-engage TALK).
    void onHeadPet();

    void _update(Modifiable& stackchan) override;

private:
    void applyIdleProfile();
    void emitStateChanged();
    void writePips(Modifiable& stackchan, uint32_t now);

    // Phase 5 — sleep state side-effects.
    void onEnterSleep();
    void onExitSleep();

    State    _state            = State::IDLE;
    bool     _kid_mode         = false;
    bool     _smart_mode       = false;
    uint32_t _state_change_ms  = 0;
    uint32_t _last_assert_ms   = 0;
    // Phase 5 — true while we've taken the sleep pose (yaw=0 pitch=450) but
    // motion is still settling. _update releases servo torque once the move
    // completes so the head can droop under gravity.
    bool     _sleep_torque_release_pending = false;
};

}  // namespace stackchan
