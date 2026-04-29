/*
 * SPDX-FileCopyrightText: 2026 Dotty
 *
 * SPDX-License-Identifier: MIT
 */
#include "state_manager.h"
#include "../stackchan.h"
#include "../face/face_detector.h"
#include "../modifiers/idle_motion.h"
#include "../modifiers/dance.h"
#include "../avatar/avatar/elements/emotion.h"
#include "application.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace stackchan {

static const std::string_view _tag = "StateManager";

const char* StateManager::stateName(State s)
{
    switch (s) {
        case State::IDLE:       return "idle";
        case State::TALK:       return "talk";
        case State::STORY_TIME: return "story_time";
        case State::SECURITY:   return "security";
        case State::SLEEP:      return "sleep";
        case State::DANCE:      return "dance";
    }
    return "unknown";
}

bool StateManager::parseState(const char* s, State& out)
{
    if (!s) return false;
    if (std::strcmp(s, "idle") == 0)       { out = State::IDLE;       return true; }
    if (std::strcmp(s, "talk") == 0)       { out = State::TALK;       return true; }
    if (std::strcmp(s, "story_time") == 0) { out = State::STORY_TIME; return true; }
    if (std::strcmp(s, "security") == 0)   { out = State::SECURITY;   return true; }
    if (std::strcmp(s, "sleep") == 0)      { out = State::SLEEP;      return true; }
    if (std::strcmp(s, "dance") == 0)      { out = State::DANCE;      return true; }
    return false;
}

void StateManager::setState(State next)
{
    if (next != _state) {
        State prev        = _state;
        _state            = next;
        _state_change_ms  = GetHAL().millis();
        _last_assert_ms   = 0;  // forces a pip repaint on the next _update tick
        mclog::tagInfo(_tag, "state {} -> {}", stateName(prev), stateName(next));
        // Exit hook for the OUTGOING state runs FIRST, before we touch the
        // new state's profile or entry hook. Security needs to tear down its
        // pan task and release the motion lock before any successor state
        // takes over. Dance tears down its choreography modifier here too so
        // the next state's left-ring writes don't fight a still-running
        // keyframe timeline.
        if (prev == State::SECURITY) {
            onExitSecurity();
        }
        if (prev == State::DANCE) {
            onExitDance();
        }
        applyIdleProfile();
        // Phase 5 — sleep entry/exit edge hooks. Run AFTER applyIdleProfile
        // so the SLEEPY profile is already in place when we lock idle_motion
        // out, and BEFORE emitStateChanged so the bridge sees the event
        // after the firmware is fully in its new pose.
        if (next == State::SLEEP) {
            onEnterSleep();
        } else if (prev == State::SLEEP) {
            onExitSleep();
        }
        // Security entry hook also runs after applyIdleProfile so the
        // SURVEILLANCE profile is in place (the underlying default) before
        // we take the lock; idle_motion stays locked out for the whole
        // tenure.
        if (next == State::SECURITY) {
            onEnterSecurity();
        }
        // Dance edge events — give the bridge an explicit dance_active
        // flag (separate from generic state_changed) so it can suppress
        // autonomous photos / TTS for the duration of a dance without
        // parsing state names. Entry hook runs alongside so the choreography
        // timeline is in the modifier pool by the time the bridge sees
        // dance_started.
        if (next == State::DANCE) {
            onEnterDance();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{\"from\":\"%s\"}", stateName(prev));
            Application::GetInstance().SendEvent("dance_started", buf);
        } else if (prev == State::DANCE) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{\"to\":\"%s\"}", stateName(next));
            Application::GetInstance().SendEvent("dance_ended", buf);
        }
    }
    // Always emit state_changed — even when next == _state — so callers
    // can use `setState(currentState())` as a deliberate re-sync. The
    // bridge caches state across firmware reboots, and there's no other
    // way for it to learn the firmware is back in IDLE if the firmware
    // happens to already be in IDLE when the dashboard click arrives.
    emitStateChanged();
}

void StateManager::setKidMode(bool enabled)
{
    if (_kid_mode == enabled) return;
    _kid_mode       = enabled;
    _last_assert_ms = 0;
    mclog::tagInfo(_tag, "kid_mode = {}", enabled);
}

void StateManager::setSmartMode(bool enabled)
{
    if (_smart_mode == enabled) return;
    _smart_mode     = enabled;
    _last_assert_ms = 0;
    mclog::tagInfo(_tag, "smart_mode = {}", enabled);
}

void StateManager::onFaceDetected()
{
    // Right-ring face pixel (global 6). Promote Off -> Detected, but never
    // downgrade Identified -> Detected — Identified holds for its timeout
    // window even if face_tracking re-fires the detection edge.
    _face_detected = true;
    if (_face_state == FaceState::Off) {
        _face_state = FaceState::Detected;
    }
    _last_assert_ms = 0;

    // IDLE -> TALK on face_detected. Sticky states (STORY_TIME, SECURITY,
    // DANCE) own their own exits — a face appearing mid-story shouldn't
    // bump us out of story_time, and a face appearing during security mode is
    // exactly what security mode is watching for (the bridge handles that
    // transition explicitly via set_state MCP, not here).
    //
    // Privacy sleep: face_detector is disabled on enter-sleep, so this
    // path shouldn't fire from SLEEP at all. The explicit IDLE-only guard
    // is defense-in-depth in case a stale frame races the detector teardown.
    // Wake from SLEEP is touch-or-dashboard only.
    if (_state == State::IDLE) {
        setState(State::TALK);
    }
}

void StateManager::onFaceLost()
{
    // Right-ring face pixel: face is gone — extinguish regardless of whether
    // we were Detected or Identified.
    _face_detected = false;
    _face_state = FaceState::Off;
    _last_assert_ms = 0;
    _last_face_lost_ms = GetHAL().millis();

    // Only TALK -> IDLE on face_lost. The grace-period check has already
    // happened in face_tracking before this call site, so by the time we land
    // here the face is genuinely gone.
    if (_state == State::TALK) {
        setState(State::IDLE);
    }
}

void StateManager::setFaceIdentified()
{
    // Bridge calls this on every successful room-view VLM match. The HuMan
    // detector flickers (face_detected/face_lost pairs ~1 s apart even when
    // a person is plainly in frame), so a strict `_face_detected` guard
    // would no-op the call any time the bbox briefly drops out between the
    // bridge logging name-greet and the MCP arriving (~150-300 ms). We
    // accept the call if a face is currently detected OR was lost within
    // kFaceIdentifiedFlickerGraceMs.
    //
    // Both outcomes emit a perception event so the bridge dashboard can
    // see whether the firmware actually lit the green pip — `applied`
    // means it did, `rejected` means the gap exceeded the grace window
    // and the call was a no-op (person genuinely left the frame).
    uint32_t now = GetHAL().millis();
    bool fresh_loss = _last_face_lost_ms != 0
        && (now - _last_face_lost_ms) <= kFaceIdentifiedFlickerGraceMs;
    if (!_face_detected && !fresh_loss) {
        mclog::tagInfo(_tag,
            "set_face_identified rejected: no face detected, last loss {} ms ago",
            _last_face_lost_ms ? (now - _last_face_lost_ms) : 0);
        Application::GetInstance().SendEvent("face_identified_rejected", "{}");
        return;
    }
    _face_state = FaceState::Identified;
    _face_state_set_ms = now;
    _last_assert_ms = 0;
    mclog::tagInfo(_tag,
        "face identified ({}) — green pip lit for {} ms",
        _face_detected ? "live" : "flicker-grace", kFaceIdentifiedTimeoutMs);
    Application::GetInstance().SendEvent("face_identified_applied", "{}");
}

void StateManager::setListening(bool on)
{
    if (_listening == on) return;
    _listening = on;
    _last_assert_ms = 0;
}

void StateManager::onVoiceListening()
{
    // xiaozhi entered LISTENING — user is taking a conversational turn.
    // IDLE -> TALK. Sticky states (STORY_TIME/SECURITY/DANCE) own their
    // own exits.
    //
    // Privacy sleep: wake-word detection and voice processing are disabled
    // on enter-sleep, so xiaozhi shouldn't reach LISTENING from SLEEP at
    // all. The explicit IDLE-only guard is defense-in-depth — wake from
    // SLEEP is touch-or-dashboard only.
    if (_state == State::IDLE) {
        setState(State::TALK);
    }
}

void StateManager::onVoiceStandby()
{
    // xiaozhi returned to STANDBY — no chat in flight. Drop back to IDLE
    // if we entered TALK from a voice trigger. Mirrors onFaceLost.
    if (_state == State::TALK) {
        setState(State::IDLE);
    }
}

void StateManager::onHeadPet()
{
    // Phase 5 — capacitive head-pet wakes from SLEEP. No-op outside SLEEP.
    // Goes to IDLE rather than TALK because there's no face yet — the user
    // touched the head, they may or may not be in front of the camera.
    // face_tracking's normal IDLE -> TALK on detection takes over from there.
    if (_state == State::SLEEP) {
        setState(State::IDLE);
    }
}

void StateManager::onEnterSleep()
{
    auto& sc = ::GetStackChan();
    // 1. Avatar to sleepy + Zzz speech bubble. Direct API — bypasses
    //    StackChanAvatarDisplay::SetEmotion("sleepy"), which has its own
    //    legacy hard-sleep path that this hook now mirrors deliberately
    //    (privacy sleep: camera + mic off, wake = touch or dashboard only).
    if (sc.hasAvatar()) {
        sc.avatar().setEmotion(avatar::Emotion::Sleepy);
        sc.avatar().setSpeech("Zzz…");
    }
    // 2. Take the motion modify lock so idle_motion (now on the SLEEPY
    //    profile via applyIdleProfile) doesn't issue micro-moves while we
    //    pose. Released in onExitSleep.
    sc.motion().setModifyLock(true);
    // 3. Pose to home position (centre, neutral) at slow speed. goHome()
    //    moves both servos to true home (yaw=0, pitch=0) — the same neutral
    //    pose the device boots into. Reads as "Dotty has gone still" rather
    //    than "Dotty is drooping", which is the deliberate feel for sleep.
    sc.motion().goHome(80, "state_manager_sleep_pose");
    // 4. Defer torque-release until the move settles — _update polls
    //    motion.isMoving() each tick and releases torque the moment it's
    //    safe. Torque-off mid-move would freeze the head wherever it
    //    happens to be, which is uglier than the brief drop after settling.
    _sleep_torque_release_pending = true;
    // 5. Privacy sleep — disable the camera-driven face detector. The
    //    refcounted CameraStreamGuard inside FaceDetector::taskEntry releases
    //    on the next tick once `_enabled` flips false, which runs
    //    VIDIOC_STREAMOFF on the V4L2 device when no other consumer is active.
    //    Wake mechanisms that survive: head-pet (capacitive) and dashboard
    //    state-change (set_state MCP). Face-wake and wake-word-wake are
    //    intentionally suppressed.
    FaceDetector::getInstance().setEnabled(false);
    // 6. Clear the listening LED edge. If sleep was triggered by voice
    //    ("go to sleep dotty"), xiaozhi was in LISTENING when this hook
    //    fires and the red pip is currently lit. The privacy gate's
    //    forced SetDeviceState(kDeviceStateIdle) is gated to skip display
    //    side-effects (so it doesn't clobber Sleepy + Zzz), so we have
    //    to flip the listening flag ourselves — next 5 Hz pip repaint
    //    extinguishes the red pip.
    setListening(false);
    // 7. Privacy sleep — flip xiaozhi's privacy gate. The gate forces
    //    xiaozhi to kDeviceStateIdle, disables wake-word + voice
    //    processing, and pins those disables across subsequent display-
    //    state transitions (kDeviceStateIdle / Listening / Speaking
    //    cases all skip their display+audio entry while gated) so
    //    xiaozhi's HandleStateChangedEvent can't re-arm or re-paint
    //    mid-sleep. Single write — no tick-loop re-assert. Cleared on
    //    onExitSleep.
    Application::GetInstance().SetPrivacyGate(true);
    mclog::tagInfo(_tag, "sleep: pose initiated, lock taken, torque release deferred, "
                         "camera + mic disabled, listening pip cleared (privacy sleep)");
}

void StateManager::onExitSleep()
{
    auto& sc = ::GetStackChan();
    // 1. Re-enable torque BEFORE any motion command — servos can't move
    //    while torque is off. Cancels the deferred release if onEnterSleep's
    //    pose hadn't settled yet (wake came in mid-pose).
    sc.motion().setTorqueEnabled(true);
    _sleep_torque_release_pending = false;
    // 2. Wake-up tilt: gentle bow lift to ~70 pitch, yaw centred.
    Application::GetInstance().SendEvent("sleep_pose", "{\"phase\":\"wake_tilt\"}");
    sc.motion().moveWithSpeed(0, 70, 80, "state_manager_wake_tilt");
    // 3. Release the modify lock so idle_motion (now back on the NORMAL
    //    profile via applyIdleProfile) can resume.
    sc.motion().setModifyLock(false);
    // 4. Avatar back to neutral + clear the Zzz bubble.
    if (sc.hasAvatar()) {
        sc.avatar().setEmotion(avatar::Emotion::Neutral);
        sc.avatar().setSpeech("");
    }
    // 5. Privacy sleep teardown — re-enable camera + lift xiaozhi's
    //    privacy gate. Lifting the gate restores xiaozhi's normal idle-
    //    state defaults (wake-word on, voice processing off until
    //    LISTENING).
    FaceDetector::getInstance().setEnabled(true);
    Application::GetInstance().SetPrivacyGate(false);
    mclog::tagInfo(_tag, "sleep: woke; torque on, lock released, wake-pose initiated, "
                         "camera + mic re-enabled");
}

void StateManager::onEnterSecurity()
{
    auto& sc = ::GetStackChan();
    // Avatar to angry — latches until onExitSecurity restores Neutral.
    // Mirrors the sleep-mode pattern (Sleepy on enter, Neutral on exit).
    if (sc.hasAvatar()) {
        sc.avatar().setEmotion(avatar::Emotion::Angry);
    }
    // 1. Take the motion modify lock so idle_motion (now on SURVEILLANCE
    //    profile) doesn't issue random ±500 yaw nudges over our deliberate
    //    pan. Released in onExitSecurity. Refcounted lock — face_tracking /
    //    StackChanCamera capture guards still nest inside us cleanly.
    sc.motion().setModifyLock(true);
    // 2. Spawn the pan worker. Idempotent: if a stale handle exists from a
    //    racey re-entry, we tear it down first. Mirror of FaceDetector's
    //    start/stop pattern (atomic _running flag + binary stop semaphore).
    if (_security_task_handle != nullptr) {
        mclog::tagWarn(_tag, "security: stale task handle on entry, tearing down");
        _security_running.store(false, std::memory_order_release);
        if (_security_stop_sem) {
            xSemaphoreTake(_security_stop_sem, pdMS_TO_TICKS(2000));
            vSemaphoreDelete(_security_stop_sem);
            _security_stop_sem = nullptr;
        }
        _security_task_handle = nullptr;
    }
    _security_running.store(true, std::memory_order_release);
    _security_stop_sem = xSemaphoreCreateBinary();
    Application::GetInstance().SendEvent("security_pose", "{\"phase\":\"scan_start\"}");
    // 4 KB stack is plenty — the loop just calls motion APIs and sleeps.
    // Pinned to Core 1 (same as the main task) to keep Core 0 free for the
    // detector / audio pipelines. Priority 1 = same as face_det.
    xTaskCreatePinnedToCore(securityPanTaskEntry, "sec_pan", 4096, this, 1,
                             &_security_task_handle, 1);
    mclog::tagInfo(_tag, "security: lock taken, pan task started");
}

void StateManager::onExitSecurity()
{
    auto& sc = ::GetStackChan();
    // 1. Signal the worker to drop out, then wait up to 2 s for it to clear.
    //    The worker checks _security_running between every sleep tick, so
    //    typical exit latency is sub-second — but a long pan move in flight
    //    (3 s settle) means the slow path can take up to ~4 s. 2 s is a
    //    reasonable cap; if it expires we proceed anyway and rely on the
    //    task's own vTaskDelete to land before the next entry.
    _security_running.store(false, std::memory_order_release);
    if (_security_stop_sem) {
        xSemaphoreTake(_security_stop_sem, pdMS_TO_TICKS(2000));
        vSemaphoreDelete(_security_stop_sem);
        _security_stop_sem = nullptr;
    }
    _security_task_handle = nullptr;
    // 2. Release the motion modify lock so the successor state's idle /
    //    chat / dance overlays can drive the head again.
    sc.motion().setModifyLock(false);
    // 3. Return the head to neutral. goHome() rather than a deliberate
    //    moveWithSpeed so the next state takes over from a known pose.
    sc.motion().goHome(80, "state_manager_security_exit");
    // 4. Restore neutral expression (was latched Angry on entry).
    if (sc.hasAvatar()) {
        sc.avatar().setEmotion(avatar::Emotion::Neutral);
    }
    mclog::tagInfo(_tag, "security: pan task stopped, lock released, head to home");
}

void StateManager::onEnterDance()
{
    // Pick the Happy sequence as the default — short (~4 s), clearly
    // dance-y (sway + happy eyes + open mouth). The other sequences in
    // dance.h (Robot, Panic, LookAround) are reserved for voice-driven or
    // bridge-driven choreography that wants a specific feel.
    auto modifier = std::make_unique<DanceModifier>(DanceModifier::Happy);
    _dance_modifier_id = ::GetStackChan().addModifier(std::move(modifier));
    mclog::tagInfo(_tag, "dance: choreography started (modifier id={})",
                   _dance_modifier_id);
}

void StateManager::onExitDance()
{
    // Tear down the timeline if it's still running. removeModifier is a
    // benign no-op if the slot has already been freed by the modifier's own
    // requestDestroy() (timeline finished naturally), so we always call it.
    if (_dance_modifier_id >= 0) {
        ::GetStackChan().removeModifier(_dance_modifier_id);
        _dance_modifier_id = -1;
    }
    mclog::tagInfo(_tag, "dance: choreography stopped");
}

void StateManager::securityPanTaskEntry(void* arg)
{
    auto* self = static_cast<StateManager*>(arg);
    self->runSecurityPanLoop();
    if (self->_security_stop_sem) {
        xSemaphoreGive(self->_security_stop_sem);
    }
    vTaskDelete(nullptr);
}

void StateManager::runSecurityPanLoop()
{
    // Methodical surveillance sweep. Each leg moves at speed 50 (slow) and
    // sleeps ~3 s for the move to settle, then dwells 1 s at the extreme
    // before the next leg. Full cycle: -500 → +500 → 0 → pause = ~14 s.
    // We re-check _security_running between every step so an exit during
    // a pan or dwell drops out within at most one settle (3 s).
    //
    // Magnitudes: ±500 matches the SURVEILLANCE profile's yaw amplitude
    // (idle_motion uses ±500 in this same band, so we stay within the
    // visually sensible scan range for the pose).
    auto& sc = ::GetStackChan();
    auto check_running = [this]() {
        return _security_running.load(std::memory_order_acquire);
    };
    // Sleep helper that yields in ~50 ms slices so we drop out promptly.
    auto sleep_ms = [&check_running](uint32_t total_ms) {
        const uint32_t slice = 50;
        uint32_t elapsed = 0;
        while (elapsed < total_ms && check_running()) {
            uint32_t step = (total_ms - elapsed) < slice ? (total_ms - elapsed) : slice;
            vTaskDelay(pdMS_TO_TICKS(step));
            elapsed += step;
        }
    };

    while (check_running()) {
        // Leg 1 — full left.
        sc.motion().moveWithSpeed(-500, 0, 50, "state_manager_security_leg1");
        sleep_ms(3000);
        if (!check_running()) break;
        sleep_ms(1000);  // dwell at extreme
        if (!check_running()) break;

        // Leg 2 — full right.
        sc.motion().moveWithSpeed(500, 0, 50, "state_manager_security_leg2");
        sleep_ms(3000);
        if (!check_running()) break;
        sleep_ms(1000);  // dwell at extreme
        if (!check_running()) break;

        // Leg 3 — back to centre, then a longer 4 s pause before repeating
        // gives the cadence its "heads up, scanning" rhythm rather than
        // continuous churn.
        sc.motion().moveWithSpeed(0, 0, 50, "state_manager_security_centre");
        sleep_ms(3000);
        if (!check_running()) break;
        sleep_ms(4000);
    }
}

void StateManager::applyIdleProfile()
{
    auto* idle = static_cast<IdleMotionModifier*>(
        ::GetStackChan().getModifierByName(IdleMotionModifier::kName));
    if (!idle) return;
    using P = IdleMotionModifier::IdleProfile;
    switch (_state) {
        case State::SLEEP:    idle->setIdleProfile(P::SLEEPY);       break;
        case State::SECURITY: idle->setIdleProfile(P::SURVEILLANCE); break;
        // TALK / STORY_TIME / DANCE / IDLE all use NORMAL — chat overlay
        // (face_tracking's tracking-mode) and dance choreography drive the
        // head while those states are active anyway.
        default:              idle->setIdleProfile(P::NORMAL);       break;
    }
}

void StateManager::emitStateChanged()
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{\"state\":\"%s\"}", stateName(_state));
    Application::GetInstance().SendEvent("state_changed", buf);
}

void StateManager::writePips(Modifiable& stackchan, uint32_t now)
{
    // ---- State arc on left ring global 0-5 ----
    // Dance owns the left ring directly via its animation timeline (rainbow
    // sweep) so we skip writing during DANCE — otherwise we'd clobber the
    // rainbow with state colour on every tick.
    if (_state != State::DANCE) {
        uint8_t r = 0, g = 0, b = 0;
        switch (_state) {
            case State::IDLE:       r = 0;   g = 0;   b = 0;  break;  // off
            case State::TALK:       r = 0;   g = 60;  b = 0;  break;  // dim green
            case State::STORY_TIME: r = 100; g = 40;  b = 0;  break;  // warm
            case State::SLEEP:      r = 0;   g = 0;   b = 16; break;  // very dim blue
            case State::DANCE:      break;  // unreachable — guarded above
            case State::SECURITY: {
                // 1 Hz flash. Phase computed from time-since-state-entry so the
                // flash starts at "on" the moment SECURITY is entered.
                uint32_t age = now - _state_change_ms;
                bool     on  = ((age / kSecurityFlashHalfMs) % 2) == 0;
                if (on) { r = 80; g = 80; b = 80; }
                break;
            }
        }
        for (uint8_t i = 0; i < 6; ++i) {
            stackchan.leftNeonLight().setColorAt(i, r, g, b);
        }
    }

    // ---- Right ring (global 6-11) — all six pixels owned and re-asserted here ----
    //
    // The right ring is the status-indicator strip. Every pixel is written
    // every tick so MCP writes (set_led_color/set_led_multi), dance keyframes,
    // or any other future writer can't permanently clobber the indicators —
    // the worst they can do is a 200 ms flicker before the next re-assert.

    // Pixel 6: face state — yellow=detected, green=identified.
    // Identified has a self-timeout; the bridge refreshes by calling the
    // self.robot.set_face_identified MCP tool again on each VLM match.
    if (_face_state == FaceState::Identified
        && (now - _face_state_set_ms) > kFaceIdentifiedTimeoutMs) {
        _face_state = _face_detected ? FaceState::Detected : FaceState::Off;
    }
    switch (_face_state) {
        case FaceState::Off:
            stackchan.rightNeonLight().setColorAt(kFacePipRightLocal, 0, 0, 0);
            break;
        case FaceState::Detected:
            // Tuned yellow — sits in the (0-168) band the existing pips use so
            // brightness reads consistent across the ring after RGB565 rounding.
            stackchan.rightNeonLight().setColorAt(kFacePipRightLocal, 168, 140, 0);
            break;
        case FaceState::Identified:
            // Tuned green — ditto.
            stackchan.rightNeonLight().setColorAt(kFacePipRightLocal, 0, 140, 30);
            break;
    }

    // Pixel 7: reserved, locked off (defense-in-depth — if anything else writes
    // here we over-write within 200 ms).
    stackchan.rightNeonLight().setColorAt(kReservedPipRightLocal_7, 0, 0, 0);

    // Pixel 8: kid_mode pip.
    if (_kid_mode) {
        // Salmon pink — G == B prevents the RGB565 cool cast that made the prior
        // (168,80,100) hue read as purple/magenta after PY32 quantization.
        stackchan.rightNeonLight().setColorAt(kKidModePipRightLocal, 220, 80, 80);
    } else {
        stackchan.rightNeonLight().setColorAt(kKidModePipRightLocal, 0, 0, 0);
    }

    // Pixel 9: smart_mode pip.
    if (_smart_mode) {
        stackchan.rightNeonLight().setColorAt(kSmartModePipRightLocal, 168, 80, 0);
    } else {
        stackchan.rightNeonLight().setColorAt(kSmartModePipRightLocal, 0, 0, 0);
    }

    // Pixel 10: reserved, locked off.
    stackchan.rightNeonLight().setColorAt(kReservedPipRightLocal_10, 0, 0, 0);

    // Pixel 11: listening pixel — red while xiaozhi is in LISTENING (mic open,
    // user's turn). Off otherwise. setListening() is called from
    // stackchan_display.cc SetStatus() at LISTENING / STANDBY / SPEAKING edges.
    if (_listening) {
        stackchan.rightNeonLight().setColorAt(kListeningPipRightLocal, 120, 0, 0);
    } else {
        stackchan.rightNeonLight().setColorAt(kListeningPipRightLocal, 0, 0, 0);
    }
}

void StateManager::_update(Modifiable& stackchan)
{
    uint32_t now = GetHAL().millis();

    // Privacy sleep — release servo torque so the servos aren't powered for
    // the duration of sleep (longevity + heat). Two paths to fire:
    //   1. Pose settled cleanly — preferred. isMoving() polls the SCS bus
    //      which can keep reporting busy if the servo holds position with
    //      micro-corrections, so this isn't always reachable.
    //   2. Timeout fallback at kSleepTorqueReleaseTimeoutMs after sleep
    //      entry. goHome at speed 80 should converge in ~1 s; 3 s is a
    //      generous deadline that bounds servo-on time even when the bus
    //      check never reports settled.
    // Either path is one-shot — _sleep_torque_release_pending clears on
    // first fire.
    if (_sleep_torque_release_pending && _state == State::SLEEP) {
        const bool settled = !stackchan.motion().isMoving();
        const bool timeout = (now - _state_change_ms) > kSleepTorqueReleaseTimeoutMs;
        if (settled || timeout) {
            stackchan.motion().setTorqueEnabled(false);
            _sleep_torque_release_pending = false;
            mclog::tagInfo(_tag, "sleep: torque released ({})",
                           settled ? "pose settled" : "timeout fallback");
        }
    }

    // 5 Hz tick. Primary purpose is driving the SECURITY 1 Hz flash —
    // the flash phase is recomputed on every pass, so a 200 ms tick
    // produces a clean 500 ms on / 500 ms off. The same tick also re-
    // asserts the state arc + toggle pips as defense-in-depth in case
    // any future writer clobbers them.
    if ((now - _last_assert_ms) < kReassertIntervalMs) return;
    writePips(stackchan, now);
    _last_assert_ms = now;
}

}  // namespace stackchan
