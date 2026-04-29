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

// StateManager owns the high-level mode for the device AND the entire right
// LED ring (status indicators):
//   - State arc on left ring 0-5      (all 6 pixels paint the state colour).
//   - Face state pixel  on right ring 6  (yellow=detected, green=identified).
//   - Reserved pixels   on right ring 7 / 10 (locked off, defense-in-depth).
//   - Toggle pips       on right ring 8 / 9  (kid_mode warm pink, smart_mode orange).
//   - Listening pixel   on right ring 11     (red while xiaozhi LISTENING).
//   - IdleMotionModifier::IdleProfile (NORMAL / SURVEILLANCE / SLEEPY).
//   - "state_changed" perception event for bridge consumers.
//
// The 5 Hz tick drives the SECURITY 1 Hz flash, the face-identified timeout,
// and acts as defense-in-depth re-assert across the entire right ring so MCP
// writes / dance keyframes / future writers can't permanently clobber the
// owned indicators.
//
// face_tracking calls onFaceDetected / onFaceLost on detection edges so the
// IDLE <-> TALK transitions happen at the camera, not via the bridge round-trip;
// the same hooks also drive the face-state LED. The bridge calls
// setFaceIdentified() via the self.robot.set_face_identified MCP tool after
// the room-view VLM matches a household roster entry.
class StateManager : public Modifier {
public:
    static constexpr const char* kName = "state_manager";

    // LED indices in the GLOBAL 12-pixel ring (left 0-5, right 6-11).
    // The state arc paints all 6 left pixels, so no left-ring index constant
    // is needed. RightNeonLight uses LOCAL 0-5 internally and adds 6 — see
    // neon_light.cpp:103-112 — so the right-ring writes use local indices.
    static constexpr uint8_t kFacePipRightLocal           = 0;  // global 6
    static constexpr uint8_t kReservedPipRightLocal_7     = 1;  // global 7
    static constexpr uint8_t kKidModePipRightLocal        = 2;  // global 8
    static constexpr uint8_t kSmartModePipRightLocal      = 3;  // global 9
    static constexpr uint8_t kReservedPipRightLocal_10    = 4;  // global 10
    static constexpr uint8_t kListeningPipRightLocal      = 5;  // global 11

    // 5 Hz tick. Drives the SECURITY 1 Hz flash phase, the face-identified
    // timeout, and acts as defense-in-depth re-assert across the entire
    // right ring (face / reserved / kid / smart / listening).
    static constexpr uint32_t kReassertIntervalMs = 200;
    // 1 Hz flash for SECURITY: 500 ms on, 500 ms off.
    static constexpr uint32_t kSecurityFlashHalfMs = 500;
    // Face-identified auto-timeout. Bridge refreshes by calling
    // self.robot.set_face_identified again after each successful room-view
    // identification. If the bridge goes silent, the green pip reverts to
    // yellow (face still in frame) or off (face gone) after this window.
    static constexpr uint32_t kFaceIdentifiedTimeoutMs = 4000;
    // setFaceIdentified() will accept the call even when no face is currently
    // detected, provided face_lost fired within this window. Bridges the
    // 800 ms detector-flicker the HuMan model exhibits at typical lighting +
    // pose so a brief drop between bridge VLM completion and MCP delivery
    // doesn't no-op the green pip. Set tighter than kFaceIdentifiedTimeoutMs
    // so a long-departed face can't accidentally re-light it.
    static constexpr uint32_t kFaceIdentifiedFlickerGraceMs = 1500;
    // Privacy sleep — bound on how long the servos stay torqued after sleep
    // entry. The preferred path (motion.isMoving() reporting settled) fires
    // first when the goHome pose converges cleanly; this is the fallback
    // for cases where the SCS-bus busy poll keeps reporting moving even at
    // rest. 3 s is generous vs the ~1 s goHome at speed 80 takes to converge.
    static constexpr uint32_t kSleepTorqueReleaseTimeoutMs = 3000;

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
    // by camera edges. SLEEP disables the face detector outright (privacy
    // sleep), so these hooks shouldn't fire from SLEEP — wake mechanisms
    // are head-pet (capacitive) and dashboard set_state.
    void onFaceDetected();
    void onFaceLost();

    // xiaozhi chat-state hooks. Called from stackchan_display.cc when
    // xiaozhi enters LISTENING (user has the turn) or STANDBY (no chat
    // in flight). Functionally parallel to face_detected/face_lost: voice
    // activity is the second presence signal that drives idle ↔ talk,
    // so the talk arc still lights even when face detection is unavailable.
    // Sticky states (STORY_TIME/SECURITY/DANCE) ignore voice edges.
    void onVoiceListening();
    void onVoiceStandby();

    // Phase 5 — capacitive head-pet wake. Called from head_pet.cpp when a
    // press fires. No-op outside SLEEP; in SLEEP transitions to IDLE
    // (head-pet is non-conversational, so we don't auto-engage TALK).
    void onHeadPet();

    // Right-ring listening pixel (global 11). Called from stackchan_display.cc
    // SetStatus() at LISTENING / STANDBY / SPEAKING transitions. Pure flag
    // flip — no state-machine side effects. The state-transition logic lives
    // in onVoiceListening / onVoiceStandby above.
    void setListening(bool on);

    // Right-ring face-state pixel (global 6). Called from the
    // self.robot.set_face_identified MCP tool when the bridge's room-view
    // VLM matches a household roster entry. No-op if no face is currently
    // detected (no point lighting green for a face that isn't there).
    // The 5 Hz tick auto-reverts to Detected (or Off) after
    // kFaceIdentifiedTimeoutMs of no refresh.
    void setFaceIdentified();

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

    // Dance mode — instantiate a DanceModifier on entry so the keyframe
    // sequence drives the head and the left ring's animation. The modifier
    // self-destroys when the timeline finishes; onExitDance() forces removal
    // if the user transitions out of DANCE before the choreography ends.
    void onEnterDance();
    void onExitDance();

    // Face-state pixel (global 6) tri-state. Detected wins on face_detected;
    // Identified wins on the MCP tool call but only while a face is in frame
    // and only for kFaceIdentifiedTimeoutMs after each refresh.
    enum class FaceState : uint8_t { Off, Detected, Identified };

    State     _state            = State::IDLE;
    bool      _kid_mode         = false;
    bool      _smart_mode       = false;
    bool      _listening        = false;
    bool      _face_detected    = false;
    FaceState _face_state       = FaceState::Off;
    uint32_t  _face_state_set_ms = 0;
    // Last onFaceLost() timestamp (millis). Used by setFaceIdentified() to
    // accept the MCP across the brief detector flicker (see
    // kFaceIdentifiedFlickerGraceMs).
    uint32_t  _last_face_lost_ms = 0;
    uint32_t  _state_change_ms  = 0;
    uint32_t  _last_assert_ms   = 0;
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

    // Pool id of the active DanceModifier, or -1 if no dance is in flight.
    // The modifier self-destroys when its timeline finishes; the id then
    // points at a free pool slot, which removeModifier handles as a benign
    // no-op. A fresh entry to DANCE overwrites it with a new id.
    int _dance_modifier_id = -1;
};

}  // namespace stackchan
