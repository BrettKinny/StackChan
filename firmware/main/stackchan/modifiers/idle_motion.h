/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../utils/random.h"
#include <smooth_ui_toolkit.hpp>
// #include <mooncake_log.h>
#include <hal/hal.h>
#include <cstdint>

namespace stackchan {

/**
 * @brief
 *
 */
class IdleMotionModifier : public Modifier {
public:
    static constexpr const char* kName = "idle_motion";

    // Phase 2 — overlay interval used while a face is locked. Longer than
    // the idle range (4–8 s) so the head only drifts every 12–24 s instead
    // of every few seconds; preserves the "Dotty is paying attention to
    // you" feel while still giving the gaze occasional life.
    static constexpr uint32_t kTrackingOverlayMinMs = 12000;
    static constexpr uint32_t kTrackingOverlayMaxMs = 24000;
    // Phase 2 — Return-to-center action only fires after the face has been
    // continuously locked for this long. Below the threshold we stick to
    // small-observation drifts only.
    static constexpr uint32_t kReturnToCenterSteadyMs = 5000;

    IdleMotionModifier(uint32_t interval_min = 4000, uint32_t interval_max = 8000)
        : _interval_min(interval_min), _interval_max(interval_max)
    {
        _next_tick = GetHAL().millis() + 1000;  // 启动 1 秒后开始第一次动作
    }

    const char* name() const override
    {
        return kName;
    }

    // Phase 2 — replace pause/resume with a tracking-mode switch.
    // tracking_mode=true: head is currently locked on a face; emit reduced,
    // longer-interval drift actions so Dotty stays "alive" without snapping
    // off the face. tracking_mode=false: full idle motion (the original
    // four-action mix). Idempotent — repeated identical calls are no-ops.
    void setTrackingMode(bool enabled)
    {
        if (_tracking_mode == enabled) return;
        _tracking_mode = enabled;
        uint32_t now   = GetHAL().millis();
        if (enabled) {
            _tracking_entered_ms = now;
            // Schedule first overlay drift one full overlay-interval out so
            // the head doesn't immediately drift away from the just-acquired
            // face — let the lock settle first.
            _next_tick = now + Random::getInstance().getInt(
                                   kTrackingOverlayMinMs, kTrackingOverlayMaxMs);
        } else {
            // Exiting tracking mode (face lost / grace expired) — kick the
            // next idle action soon so the head doesn't sit dead-eyed for
            // a full 4–8 s.
            _next_tick = now + 500;
        }
    }

    void _update(Modifiable& stackchan) override
    {
        if (!stackchan.hasAvatar()) return;

        uint32_t now = GetHAL().millis();

        // 如果时间没到，直接跳过
        if (now < _next_tick) {
            return;
        }

        // 如果上次动作还没做完，就把下一次尝试推迟 500ms，避免指令堆积
        if (stackchan.motion().isMoving()) {
            _next_tick = now + 500;
            return;
        }

        // 执行动作 — tracking mode picks the reduced overlay set, idle mode
        // picks the full four-action mix.
        if (_tracking_mode) {
            perform_tracking_overlay(stackchan, now);
        } else {
            perform_idle_motion(stackchan);
        }

        // 算下一次的时间间隔 — overlay uses a longer cadence than full idle.
        uint32_t delay = _tracking_mode
            ? Random::getInstance().getInt(kTrackingOverlayMinMs, kTrackingOverlayMaxMs)
            : Random::getInstance().getInt(_interval_min, _interval_max);
        _next_tick     = now + delay;
        // mclog::info("next idle motion in {} ms", delay);
    }

private:
    // Phase 2 — reduced action set for "face locked" mode. Drops the
    // Random look (would snap off the locked face) and Quick glance (too
    // jarring) branches entirely. Keeps Small observation with halved
    // ranges (face_tracking will pull the head back on the next detection
    // tick anyway) and Return-to-center, gated on >5 s of steady lock so
    // we don't yank away the moment a face is acquired.
    void perform_tracking_overlay(Modifiable& stackchan, uint32_t now)
    {
        auto& motion = stackchan.motion();
        if (motion.isModifyLocked()) {
            return;
        }

        uint32_t steady_ms = now - _tracking_entered_ms;
        int action         = Random::getInstance().getInt(0, 100);

        // < 80 OR not yet steady: small observation only.
        // ≥ 80 AND steady: return-to-center.
        if (action < 80 || steady_ms < kReturnToCenterSteadyMs) {
            // Halved offset ranges vs idle's Small observation (was ±150° / ±80°).
            auto current     = motion.getCurrentAngles();
            int diff_yaw     = Random::getInstance().getInt(-75, 75);
            int diff_pitch   = Random::getInstance().getInt(-40, 40);
            int target_yaw   = uitk::clamp(current.x + diff_yaw, -800, 800);
            int target_pitch = uitk::clamp(current.y + diff_pitch, 0, 600);
            int speed        = Random::getInstance().getInt(100, 250);
            motion.moveWithSpeed(target_yaw, target_pitch, speed);
        } else {
            // Return-to-center: yaw → 0, gentle pitch shift. Face_tracking
            // will pull back to the locked face on the next detection tick.
            int target_pitch = Random::getInstance().getInt(50, 400);
            int speed        = Random::getInstance().getInt(100, 300);
            motion.moveWithSpeed(0, target_pitch, speed);
        }
    }

    void perform_idle_motion(Modifiable& stackchan)
    {
        auto& motion = stackchan.motion();
        if (motion.isModifyLocked()) {
            return;
        }

        int action = Random::getInstance().getInt(0, 100);

        if (action < 50) {
            // 【动作 1：随意环视】使用归一化坐标 (-1.0 ~ 1.0)
            float target_x = Random::getInstance().getFloat(-0.4f, 0.4f);   // 左右看
            float target_y = Random::getInstance().getFloat(-0.95f, 0.2f);  // 上下看
            int speed      = Random::getInstance().getInt(150, 300);

            // mclog::info("action 1: look at normalized ({}, {}) in speed {}", target_x, target_y, speed);
            motion.lookAtNormalized(target_x, target_y, speed);
        } else if (action < 80) {
            // 【动作 2：微小的观察动作】基于当前位置的小偏移
            auto current = motion.getCurrentAngles();  // Vector2i(yaw, pitch)

            int diff_yaw   = Random::getInstance().getInt(-150, 150);
            int diff_pitch = Random::getInstance().getInt(-80, 80);

            int target_yaw   = uitk::clamp(current.x + diff_yaw, -800, 800);
            int target_pitch = uitk::clamp(current.y + diff_pitch, 0, 600);
            int speed        = Random::getInstance().getInt(100, 250);

            // mclog::info("action 2: small move to ({}, {}) in speed {}", target_yaw, target_pitch, speed);
            motion.moveWithSpeed(target_yaw, target_pitch, speed);
        } else if (action < 90) {
            // 【动作 3：快速撇一眼】速度快，跨度中等
            int target_yaw   = Random::getInstance().getInt(-500, 500);
            int target_pitch = Random::getInstance().getInt(100, 400);
            int speed        = Random::getInstance().getInt(250, 400);

            // mclog::info("action 3: quick glance to ({}, {}) in speed {}", target_yaw, target_pitch, speed);
            motion.moveWithSpeed(target_yaw, target_pitch, speed);
        } else {
            // 【动作 4：yaw 回正】
            int target_pitch = Random::getInstance().getInt(50, 400);
            int speed        = Random::getInstance().getInt(100, 300);

            // mclog::info("action 4: go home to (0, {}) in speed {}", target_pitch, speed);
            motion.moveWithSpeed(0, target_pitch, speed);
        }
    }

    uint32_t _interval_min;
    uint32_t _interval_max;
    uint32_t _next_tick = 0;
    // Phase 2 — tracking-overlay state. _tracking_mode replaces the old
    // _paused flag; when true, _update picks perform_tracking_overlay
    // instead of perform_idle_motion. _tracking_entered_ms is set on each
    // false→true transition so the overlay can gate Return-to-center on
    // the face being held steady.
    bool _tracking_mode          = false;
    uint32_t _tracking_entered_ms = 0;
};

}  // namespace stackchan
