/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
//
// HeadPetModifier — touch-driven affect + non-visual wake fallback.
//
// Touch start: fires head_pet_started perception event + HeartDecorator + Happy
// emotion (existing behavior).
// Held for >= kHoldToListenMs (2 s default): fires WakeWordInvoke("head_pet_hold")
// once per session.
//   - Opens the listen window (mic uplink to server) just like face-detected.
//   - Works in the dark, where face detection can't fire.
// Touch end: fires head_pet_ended perception event.
//
// kHoldToListenMs lives at the top of head_pet.cpp for one-line tuning.
//
#include "../modifiable.h"
#include "../avatar/decorators/decorators.h"
#include <hal/hal.h>
#include <cstdint>

namespace stackchan {

/**
 * @brief
 *
 */
class HeadPetModifier : public Modifier {
public:
    HeadPetModifier(uint32_t restoreDelayMs = 3000);
    ~HeadPetModifier();

    void _update(Modifiable& stackchan) override;

private:
    void handle_swipe(Modifiable& stackchan);
    void restore_original_state(Modifiable& stackchan);
    void perform_pet_motion(Modifiable& stackchan);
    // Quick green pulse on the listen-mode color so the user gets
    // confirmation the 2 s hold registered. The actual listen-state
    // colors (driven by stackchan_display when the WS responds) take
    // over immediately after.
    void flashWakeFeedback(Modifiable& stackchan);

    // Signals
    int _signal_connection;
    volatile bool _event_press   = false;
    volatile bool _event_swipe   = false;
    volatile bool _event_release = false;

    // Affect state machine
    bool _in_happy_state     = false;
    bool _is_waiting_restore = false;
    uint32_t _restore_tick   = 0;
    uint32_t _restore_delay_ms;
    int _heart_decorator_id = -1;
    int _shy_decorator_id   = -1;

    // Hold-to-listen state
    bool _is_touched         = false;  // Press received, Release not yet
    bool _hold_wake_fired    = false;  // wake invoked this touch session
    uint32_t _touch_start_ms = 0;

    // Memory of pre-pet pose
    avatar::Emotion _prev_emotion = avatar::Emotion::Neutral;
    int32_t _prev_yaw             = 0;
    int32_t _prev_pitch           = 0;
};

}  // namespace stackchan
