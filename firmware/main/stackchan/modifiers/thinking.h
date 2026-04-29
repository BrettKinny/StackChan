/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../utils/random.h"
#include <smooth_ui_toolkit.hpp>
#include <hal/hal.h>
#include <cstdint>

namespace stackchan {

class ThinkingModifier : public Modifier {
public:
    ThinkingModifier()
    {
        uint32_t now       = GetHAL().millis();
        _next_mouth_tick   = now + 100;
        _next_motion_tick  = now + Random::getInstance().getInt(1500, 3000);
        _need_get_prev_angles = true;
    }

    void _update(Modifiable& stackchan) override
    {
        if (!stackchan.hasAvatar()) {
            return;
        }

        uint32_t now = GetHAL().millis();

        if (now >= _next_mouth_tick) {
            _next_mouth_tick = now + 200;
            stackchan.avatar().mouth().setWeight(_pursed_weight);
        }

        if (now >= _next_motion_tick) {
            _next_motion_tick = now + Random::getInstance().getInt(3000, 5000);
            perform_thinking_motion(stackchan);
        }
    }

private:
    void perform_thinking_motion(Modifiable& stackchan)
    {
        auto& motion = stackchan.motion();
        if (motion.isMoving()) {
            return;
        }

        uitk::Vector2i current_actual_angles = motion.getCurrentAngles();

        if (_need_get_prev_angles) {
            _prev_angles          = current_actual_angles;
            _need_get_prev_angles = false;
        } else {
            const int32_t threshold = 300;
            int32_t diff_x = std::abs(current_actual_angles.x - _prev_angles.x);
            int32_t diff_y = std::abs(current_actual_angles.y - _prev_angles.y);
            if (diff_x > threshold || diff_y > threshold) {
                _prev_angles = current_actual_angles;
            }
        }

        int32_t target_yaw   = _prev_angles.x;
        int32_t target_pitch = _prev_angles.y;
        int speed            = Random::getInstance().getInt(60, 120);
        int action           = Random::getInstance().getInt(0, 10);

        if (action < 5) {
            // Look upward while pondering
            target_pitch += Random::getInstance().getInt(20, 60);
            target_yaw   += Random::getInstance().getInt(-15, 15);
        } else if (action < 8) {
            // Slight side-to-side drift
            target_yaw   += Random::getInstance().getInt(-30, 30);
            target_pitch += Random::getInstance().getInt(-10, 20);
        } else {
            // Return toward baseline
            target_yaw   = _prev_angles.x + Random::getInstance().getInt(-10, 10);
            target_pitch = _prev_angles.y + Random::getInstance().getInt(-5, 15);
        }

        motion.moveWithSpeed(target_yaw, target_pitch, speed, "thinking");
    }

    const int _pursed_weight = 8;

    uint32_t _next_mouth_tick  = 0;
    uint32_t _next_motion_tick = 0;
    bool _need_get_prev_angles = true;
    uitk::Vector2i _prev_angles;
};

}  // namespace stackchan
