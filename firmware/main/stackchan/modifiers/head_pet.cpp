/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "head_pet.h"
#include "../stackchan.h"             // Phase 5: ::GetStackChan() for state lookup
#include "../utils/random.h"
#include "../modes/state_manager.h"   // Phase 5: head-pet wakes from SLEEP
#include "application.h"  // perception events + WakeWordInvoke
#include <smooth_ui_toolkit.hpp>
#include <memory>

namespace stackchan {

// ---------------------------------------------------------------------------
// kHoldToListenMs : how long the head must be held before a non-visual wake
//                   fires. Gives users a way into voice mode in dark rooms
//                   where face detection can't lock in. 2 s feels deliberate
//                   without being tedious; below ~1.2 s starts catching
//                   incidental pets.
// ---------------------------------------------------------------------------
static constexpr uint32_t kHoldToListenMs = 2000;

HeadPetModifier::HeadPetModifier(uint32_t restoreDelayMs) : _restore_delay_ms(restoreDelayMs)
{
    _signal_connection = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture gesture) {
        if (gesture == HeadPetGesture::Press) {
            _event_press = true;
        } else if (gesture == HeadPetGesture::SwipeForward || gesture == HeadPetGesture::SwipeBackward) {
            _event_swipe = true;
        } else if (gesture == HeadPetGesture::Release) {
            _event_release = true;
        }
    });
}

HeadPetModifier::~HeadPetModifier()
{
    GetHAL().onHeadPetGesture.disconnect(_signal_connection);
}

void HeadPetModifier::_update(Modifiable& stackchan)
{
    uint32_t now = GetHAL().millis();

    // Touch start — emit perception event, arm hold timer.
    // Affect (HeartDecorator + Happy emotion) still runs off swipe events
    // below, unchanged.
    if (_event_press) {
        _event_press      = false;
        _is_touched       = true;
        _hold_wake_fired  = false;
        _touch_start_ms   = now;
        Application::GetInstance().SendEvent("head_pet_started", "{}");
        // Phase 5 — wake from SLEEP on capacitive touch. StateManager gates
        // on current state internally; outside SLEEP this is a no-op.
        if (auto* sm = static_cast<StateManager*>(
                ::GetStackChan().getModifierByName(StateManager::kName))) {
            sm->onHeadPet();
        }
    }

    // Affect: handle "being petted" (swipe gestures fire while held).
    if (_event_swipe) {
        _event_swipe = false;
        handle_swipe(stackchan);
        // While they're still petting, defer the restore.
        _is_waiting_restore = false;
    }

    // Hold-to-listen: 2 s continuous touch → fire wake once per session.
    // Coexists with HeartDecorator/Happy — wake just opens the mic window;
    // the avatar is free to keep emoting.
    if (_is_touched && !_hold_wake_fired && (now - _touch_start_ms) >= kHoldToListenMs) {
        _hold_wake_fired = true;
        flashWakeFeedback(stackchan);
        Application::GetInstance().WakeWordInvoke("head_pet_hold");
    }

    // Touch end — emit perception event, clear hold state, schedule restore.
    if (_event_release) {
        _event_release   = false;
        _is_touched      = false;
        _hold_wake_fired = false;
        Application::GetInstance().SendEvent("head_pet_ended", "{}");
        if (_in_happy_state) {
            _is_waiting_restore = true;
            _restore_tick       = now + _restore_delay_ms;
        }
    }

    // Affect restore.
    if (_is_waiting_restore && now >= _restore_tick) {
        _is_waiting_restore = false;
        restore_original_state(stackchan);
    }
}

void HeadPetModifier::handle_swipe(Modifiable& stackchan)
{
    auto& avatar = stackchan.avatar();

    // First entry into happy state — record original pose so we can restore.
    if (!_in_happy_state) {
        _in_happy_state = true;
        _prev_emotion   = avatar.getEmotion();
        auto angles     = stackchan.motion().getCurrentAngles();
        _prev_yaw       = angles.x;
        _prev_pitch     = angles.y;
    }

    // Visual feedback
    avatar.setEmotion(avatar::Emotion::Happy);

    // Heart + shy decorators
    int duration = Random::getInstance().getInt(1500, 2500);
    avatar.removeDecorator(_heart_decorator_id);
    avatar.removeDecorator(_shy_decorator_id);
    _heart_decorator_id =
        avatar.addDecorator(std::make_unique<avatar::HeartDecorator>(lv_screen_active(), duration, 500));
    _shy_decorator_id = avatar.addDecorator(std::make_unique<avatar::ShyDecorator>(lv_screen_active(), duration));

    // Motion feedback
    perform_pet_motion(stackchan);
}

void HeadPetModifier::restore_original_state(Modifiable& stackchan)
{
    if (!_in_happy_state) {
        return;
    }

    stackchan.avatar().setEmotion(_prev_emotion);
    stackchan.motion().moveWithSpeed(_prev_yaw, _prev_pitch, 200, "head_pet_restore");

    _in_happy_state = false;
}

void HeadPetModifier::perform_pet_motion(Modifiable& stackchan)
{
    auto& motion = stackchan.motion();
    if (motion.isModifyLocked() || motion.isMoving()) {
        return;
    }

    int action = Random::getInstance().getInt(0, 2);
    int speed  = Random::getInstance().getInt(300, 500);

    int32_t target_yaw   = _prev_yaw;
    int32_t target_pitch = _prev_pitch;

    switch (action) {
        case 0:  // tilt up
            target_pitch += Random::getInstance().getInt(150, 250);
            target_yaw += Random::getInstance().getInt(-50, 50);
            break;
        case 1:  // head-tilt
            target_pitch -= Random::getInstance().getInt(0, 50);
            target_yaw += (Random::getInstance().getInt(0, 1) == 0 ? 150 : -150);
            break;
        case 2:  // big happy
            target_pitch += Random::getInstance().getInt(250, 400);
            break;
    }

    target_pitch = uitk::clamp(target_pitch, 0, 540);
    target_yaw   = uitk::clamp(target_yaw, -512, 512);

    motion.moveWithSpeed(target_yaw, target_pitch, speed, "head_pet_perform");
}

void HeadPetModifier::flashWakeFeedback(Modifiable& stackchan)
{
    // The state arc on the left ring is owned by StateManager — head-pet
    // wake feedback is conveyed by the avatar (sleepy → neutral) and the
    // wake-tilt motion. No LED flash needed; an unannounced clobber would
    // fight StateManager's 5 Hz re-assert anyway.
    (void)stackchan;
}

}  // namespace stackchan
