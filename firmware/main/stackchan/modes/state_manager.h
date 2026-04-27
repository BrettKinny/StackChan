/*
 * SPDX-FileCopyrightText: 2026 Dotty
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
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
//   - State arc on left ring 0-5      (all 6 pixels paint the state colour).
//   - Toggle pips on right ring 8 / 9 (kid_mode warm pink, smart_mode orange).
//   - IdleMotionModifier::IdleProfile (NORMAL / SURVEILLANCE / SLEEPY).
//   - "state_changed" perception event for bridge consumers.
//
// The 5 Hz tick drives the SECURITY 1 Hz flash and acts as defense-in-depth
// re-assert. Chat-state writes (set_left_leds in stackchan_display.cc) no
// longer touch the left ring — that hook now drives only the right-ring
// listening pixel at index 6.
//
// face_tracking calls onFaceDetected / onFaceLost on detection edges so the
// IDLE <-> TALK transitions happen at the camera, not via the bridge round-trip.
class StateManager : public Modifier {
public:
    static constexpr const char* kName = "state_manager";

    // LED indices in the GLOBAL 12-pixel ring (left 0-5, right 6-11).
    // The state arc paints all 6 left pixels, so no left-ring index constant
    // is needed. RightNeonLight uses LOCAL 0-5 internally and adds 6 — see
    // neon_light.cpp:103-112 — so the right-ring writes use local indices.
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

    // Security mode — methodical slow pan loop, modelled on the sleep
    // pattern (motion-lock + dedicated worker). The pan task is spawned
    // on entry so the move loop runs off the main task; the lock is held
    // for the entire SECURITY tenure so idle_motion's SURVEILLANCE
    // micro-jitters don't fight the deliberate sweep.
    void onEnterSecurity();
    void onExitSecurity();
    static void securityPanTaskEntry(void* arg);
    void runSecurityPanLoop();

    State    _state            = State::IDLE;
    bool     _kid_mode         = false;
    bool     _smart_mode       = false;
    uint32_t _state_change_ms  = 0;
    uint32_t _last_assert_ms   = 0;
    // Phase 5 — true while we've taken the sleep pose (yaw=0 pitch=450) but
    // motion is still settling. _update releases servo torque once the move
    // completes so the head can droop under gravity.
    bool     _sleep_torque_release_pending = false;

    // Security pan task lifecycle. _security_running flips false on exit;
    // the worker checks it between every step + dwell and drops out cleanly.
    // _security_stop_sem signals the worker has exited so onExitSecurity
    // can release the motion lock without racing the final move command.
    TaskHandle_t      _security_task_handle = nullptr;
    std::atomic<bool> _security_running{false};
    SemaphoreHandle_t _security_stop_sem    = nullptr;
};

}  // namespace stackchan
