/*
 * SPDX-FileCopyrightText: 2026 Dotty
 *
 * SPDX-License-Identifier: MIT
 */
#include "state_manager.h"
#include "../stackchan.h"
#include "../modifiers/idle_motion.h"
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
    if (next == _state) return;
    State prev        = _state;
    _state            = next;
    _state_change_ms  = GetHAL().millis();
    _last_assert_ms   = 0;  // forces a pip repaint on the next _update tick
    mclog::tagInfo(_tag, "state {} -> {}", stateName(prev), stateName(next));
    // Exit hook for the OUTGOING state runs FIRST, before we touch the new
    // state's profile or entry hook. Security needs to tear down its pan task
    // and release the motion lock before any successor state takes over.
    if (prev == State::SECURITY) {
        onExitSecurity();
    }
    applyIdleProfile();
    // Phase 5 — sleep entry/exit edge hooks. Run AFTER applyIdleProfile so the
    // SLEEPY profile is already in place when we lock idle_motion out, and
    // BEFORE emitStateChanged so the bridge sees the event after the firmware
    // is fully in its new pose.
    if (next == State::SLEEP) {
        onEnterSleep();
    } else if (prev == State::SLEEP) {
        onExitSleep();
    }
    // Security entry hook also runs after applyIdleProfile so the
    // SURVEILLANCE profile is in place (the underlying default) before we
    // take the lock; idle_motion stays locked out for the whole tenure.
    if (next == State::SECURITY) {
        onEnterSecurity();
    }
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
    // IDLE -> TALK on face_detected. Sticky states (STORY_TIME, SECURITY,
    // DANCE) own their own exits — a face appearing mid-story shouldn't
    // bump us out of story_time, and a face appearing during security mode is
    // exactly what security mode is watching for (the bridge handles that
    // transition explicitly via set_state MCP, not here).
    //
    // Phase 5 — SLEEP also wakes on face_detected; we go straight to TALK so
    // the conversation path engages without an extra IDLE hop. The exit
    // hook runs the wake-pose; face_tracking's lookAt overrides it within a
    // few ticks, which is the desired feel ("Dotty looks up, then at you").
    if (_state == State::IDLE || _state == State::SLEEP) {
        setState(State::TALK);
    }
}

void StateManager::onFaceLost()
{
    // Only TALK -> IDLE on face_lost. The grace-period check has already
    // happened in face_tracking before this call site, so by the time we land
    // here the face is genuinely gone.
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
    //    legacy hard-sleep path (face_detector off, modifiers removed) that
    //    we explicitly do NOT want here — Phase 5 sleep keeps face detection
    //    alive so face_detected wakes Dotty back up.
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
    sc.motion().goHome(80);
    // 4. Defer torque-release until the move settles — _update polls
    //    motion.isMoving() each tick and releases torque the moment it's
    //    safe. Torque-off mid-move would freeze the head wherever it
    //    happens to be, which is uglier than the brief drop after settling.
    _sleep_torque_release_pending = true;
    mclog::tagInfo(_tag, "sleep: pose initiated, lock taken, torque release deferred");
}

void StateManager::onExitSleep()
{
    auto& sc = ::GetStackChan();
    // 1. Re-enable torque BEFORE any motion command — servos can't move
    //    while torque is off. Cancels the deferred release if onEnterSleep's
    //    pose hadn't settled yet (wake came in mid-pose).
    sc.motion().setTorqueEnabled(true);
    _sleep_torque_release_pending = false;
    // 2. Wake-up tilt: gentle bow lift to ~70 pitch, yaw centred. If
    //    face_tracking is acquiring at the same time (SLEEP -> TALK via
    //    face_detected), its lookAt will override within a few ticks —
    //    that's the desired feel ("Dotty looks up, then at you").
    sc.motion().moveWithSpeed(0, 70, 80);
    // 3. Release the modify lock so idle_motion (now back on the NORMAL
    //    profile via applyIdleProfile) can resume.
    sc.motion().setModifyLock(false);
    // 4. Avatar back to neutral + clear the Zzz bubble.
    if (sc.hasAvatar()) {
        sc.avatar().setEmotion(avatar::Emotion::Neutral);
        sc.avatar().setSpeech("");
    }
    mclog::tagInfo(_tag, "sleep: woke; torque on, lock released, wake-pose initiated");
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
    sc.motion().goHome(80);
    // 4. Restore neutral expression (was latched Angry on entry).
    if (sc.hasAvatar()) {
        sc.avatar().setEmotion(avatar::Emotion::Neutral);
    }
    mclog::tagInfo(_tag, "security: pan task stopped, lock released, head to home");
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
        sc.motion().moveWithSpeed(-500, 0, 50);
        sleep_ms(3000);
        if (!check_running()) break;
        sleep_ms(1000);  // dwell at extreme
        if (!check_running()) break;

        // Leg 2 — full right.
        sc.motion().moveWithSpeed(500, 0, 50);
        sleep_ms(3000);
        if (!check_running()) break;
        sleep_ms(1000);  // dwell at extreme
        if (!check_running()) break;

        // Leg 3 — back to centre, then a longer 4 s pause before repeating
        // gives the cadence its "heads up, scanning" rhythm rather than
        // continuous churn.
        sc.motion().moveWithSpeed(0, 0, 50);
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
    // ---- State pip on left ring global 0 ----
    uint8_t r = 0, g = 0, b = 0;
    switch (_state) {
        case State::IDLE:       r = 0;   g = 0;   b = 0;  break;  // off
        case State::TALK:       r = 0;   g = 40;  b = 60; break;  // dim cyan
        case State::STORY_TIME: r = 100; g = 40;  b = 0;  break;  // warm
        case State::SLEEP:      r = 0;   g = 0;   b = 16; break;  // very dim blue
        case State::DANCE:      r = 0;   g = 0;   b = 0;  break;  // suppressed (rainbow takes over)
        case State::SECURITY: {
            // 1 Hz flash. Phase computed from time-since-state-entry so the
            // flash starts at "on" the moment SECURITY is entered.
            uint32_t age = now - _state_change_ms;
            bool     on  = ((age / kSecurityFlashHalfMs) % 2) == 0;
            if (on) { r = 80; g = 80; b = 80; }
            break;
        }
    }
    stackchan.leftNeonLight().setColorAt(kStatePipLeftIndex, r, g, b);

    // ---- Toggle pips on right ring (global 8 = local 2, global 9 = local 3) ----
    if (_kid_mode) {
        // Warm pink — RGB565 quantises hard, so this hue (slightly red-shifted)
        // is what reads as "soft pink" once the PY32 IO expander rounds it.
        stackchan.rightNeonLight().setColorAt(kKidModePipRightLocal, 168, 80, 100);
    } else {
        stackchan.rightNeonLight().setColorAt(kKidModePipRightLocal, 0, 0, 0);
    }
    if (_smart_mode) {
        stackchan.rightNeonLight().setColorAt(kSmartModePipRightLocal, 168, 80, 0);
    } else {
        stackchan.rightNeonLight().setColorAt(kSmartModePipRightLocal, 0, 0, 0);
    }
}

void StateManager::_update(Modifiable& stackchan)
{
    uint32_t now = GetHAL().millis();

    // Phase 5 — release servo torque once the sleep-entry pose settles.
    // Polling each tick is cheap (just a flag check + isMoving()); the work
    // only fires once per sleep entry.
    if (_sleep_torque_release_pending && _state == State::SLEEP &&
        !stackchan.motion().isMoving()) {
        stackchan.motion().setTorqueEnabled(false);
        _sleep_torque_release_pending = false;
        mclog::tagInfo(_tag, "sleep: torque released (pose settled)");
    }

    // 5 Hz unconditional pip re-assert. The chat-state writes in
    // stackchan_display.cc::set_left_leds() repaint pixel 0 along with the
    // rest of the left ring on every LISTENING/SPEAKING/STANDBY transition,
    // so we restore the pip within ~200 ms.
    //
    // SECURITY also rides this tick — the flash phase is recomputed on every
    // pass, so a 200 ms tick produces a clean 1 Hz flash (500 ms on / 500 ms off).
    if ((now - _last_assert_ms) < kReassertIntervalMs) return;
    writePips(stackchan, now);
    _last_assert_ms = now;
}

}  // namespace stackchan
