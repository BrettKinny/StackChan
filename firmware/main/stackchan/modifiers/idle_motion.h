/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../utils/random.h"
#include "application.h"
#include <smooth_ui_toolkit.hpp>
// #include <mooncake_log.h>
#include <hal/hal.h>
#include <cstdint>
#include <cstdio>

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

    // Phase 3 — high-level idle profile. ModeManager (Phase 4) drives this
    // via setIdleProfile() on mode entry. NORMAL is the default for awake
    // idle; LOOKING_AROUND / SLEEPY / SURVEILLANCE shape cadence + amplitude
    // + pitch baseline to match the corresponding mode's feel.
    enum class IdleProfile {
        NORMAL,         // gentle pans + small offsets, 8–15 s cadence
        LOOKING_AROUND, // wider curious pans, 6–12 s cadence
        SLEEPY,         // rare slow micro-movements, head pitched down
        SURVEILLANCE,   // wide deliberate scans, 10–20 s cadence
    };

    // Empty-room backoff (servo lifespan): once no face has been locked for
    // kEmptyRoomThresholdMs, the next idle delay is drawn from
    // kEmptyRoomMinMs..kEmptyRoomMaxMs instead of _interval_min.._interval_max.
    // Halves idle servo cycles in unattended periods. Reverts to the normal
    // mix the moment a face is re-acquired (setTrackingMode(true) restamps
    // _last_tracking_active_ms via setTrackingMode(false) when the face later
    // leaves frame).
    static constexpr uint32_t kEmptyRoomThresholdMs = 120000;  // 2 min
    static constexpr uint32_t kEmptyRoomMinMs       = 15000;
    static constexpr uint32_t kEmptyRoomMaxMs       = 30000;

    // Defaults match IdleProfile::NORMAL (Phase 3) — slower, gentler than the
    // pre-Phase-3 4–8 s mix that produced the "jerky periodic moves" complaint.
    IdleMotionModifier(uint32_t interval_min = 8000, uint32_t interval_max = 15000)
        : _interval_min(interval_min), _interval_max(interval_max)
    {
        uint32_t now = GetHAL().millis();
        _next_tick = now + 1000;          // 启动 1 秒后开始第一次动作
        _last_tracking_active_ms = now;   // Empty-room timer starts at boot.
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
                                   _tracking_overlay_min_ms, _tracking_overlay_max_ms);
        } else {
            // Exiting tracking mode (face lost / grace expired). The pre-
            // Phase-3 hop was `_next_tick = now + 500` to wake an idle action
            // quickly. With Phase 3's slower cadence + profile-shaped ranges
            // that 500 ms hop produces a "big move away" the moment the user
            // walks out of frame after a photo — they haven't finished moving
            // and Dotty is already panning to a new gentle-look target. Use
            // the normal cadence so the head settles for the full window
            // before the first post-face-lost idle action fires.
            _last_tracking_active_ms = now;
            _next_tick = now + Random::getInstance().getInt(_interval_min, _interval_max);
        }
    }

    // Phase 3 — chat-state-driven tunables. face_tracking pushes these on
    // each chat-state transition (via FaceTrackingModifier::setChatProfile).
    // setIntervalRange affects the *tracking-overlay* cadence only — the
    // full-idle 4–8 s mix stays at constructor defaults so non-locked idle
    // behaviour is unchanged. setAmplitudeScale multiplies the small-
    // observation deltas in perform_tracking_overlay (1.0 = Phase 2 default
    // ±75°/±40°, 0.5 = quarter range).
    void setIntervalRange(uint32_t min_ms, uint32_t max_ms)
    {
        _tracking_overlay_min_ms = min_ms;
        _tracking_overlay_max_ms = max_ms;
    }
    void setAmplitudeScale(float scale)
    {
        _amplitude_scale = scale;
    }

    // Phase 3 — switch the high-level idle profile. Updates cadence range
    // (used by _update's next-tick computation) and shapes the action ranges
    // applied in perform_idle_motion. Idempotent. Does NOT affect the
    // tracking-overlay path (face_tracking owns that via setIntervalRange /
    // setAmplitudeScale).
    void setIdleProfile(IdleProfile profile)
    {
        if (_profile == profile) return;
        _profile        = profile;
        const auto& p   = paramsFor(_profile);
        _interval_min   = p.cadence_min_ms;
        _interval_max   = p.cadence_max_ms;
    }

    IdleProfile idleProfile() const { return _profile; }

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
        // Overlay range is settable via setIntervalRange (Phase 3); idle range
        // stays at constructor defaults. When idle and the empty-room
        // threshold has elapsed, draw from the slow range instead.
        uint32_t delay;
        if (_tracking_mode) {
            delay = Random::getInstance().getInt(_tracking_overlay_min_ms, _tracking_overlay_max_ms);
        } else if ((now - _last_tracking_active_ms) > kEmptyRoomThresholdMs) {
            delay = Random::getInstance().getInt(kEmptyRoomMinMs, kEmptyRoomMaxMs);
        } else {
            delay = Random::getInstance().getInt(_interval_min, _interval_max);
        }
        _next_tick     = now + delay;
        // mclog::info("next idle motion in {} ms", delay);
    }

private:
    // Phase 3 — per-profile parameter table. Each profile shapes:
    //   cadence_*_ms      next-tick window for the idle path
    //   yaw/pitch_delta_range  ± deltas for action A (small offset)
    //   gentle_yaw_range  ± yaw target for action B (gentle look)
    //   pitch_baseline_*  pitch target window for actions B + C
    //   speed_min/max     servo speed range (100=slow gentle, 300=quick)
    //
    // Servo units are raw ticks: 0.3125° per tick. ±100 = ±31°. Yaw range is
    // -800 to +800; pitch is 0 to 600.
    struct ProfileParams {
        uint32_t cadence_min_ms;
        uint32_t cadence_max_ms;
        int yaw_delta_range;
        int pitch_delta_range;
        int gentle_yaw_range;
        int pitch_baseline_min;
        int pitch_baseline_max;
        int speed_min;
        int speed_max;
    };

    static constexpr ProfileParams kProfileNormal = {
        // 8–15 s cadence, modest deltas, level pitch baseline. Default awake idle.
        8000, 15000, 100, 50, 200, 100, 350, 80, 180,
    };
    static constexpr ProfileParams kProfileLookAround = {
        // 6–12 s cadence, wider yaw, slightly raised pitch baseline (alert).
        6000, 12000, 200, 80, 350, 150, 400, 100, 220,
    };
    static constexpr ProfileParams kProfileSleepy = {
        // 30–60 s cadence, tiny deltas, head pitched DOWN (300–450 baseline).
        // Slow speeds for languid feel. Pair with droopy eyelid in Phase 3+.
        30000, 60000, 50, 30, 80, 300, 450, 50, 100,
    };
    static constexpr ProfileParams kProfileSurveillance = {
        // 10–20 s cadence, wide deliberate yaw scans, level pitch baseline.
        // Slightly slower speeds than NORMAL for the "watching" feel.
        10000, 20000, 250, 60, 500, 100, 350, 90, 180,
    };

    static constexpr const ProfileParams& paramsFor(IdleProfile p)
    {
        switch (p) {
            case IdleProfile::LOOKING_AROUND: return kProfileLookAround;
            case IdleProfile::SLEEPY:         return kProfileSleepy;
            case IdleProfile::SURVEILLANCE:   return kProfileSurveillance;
            default:                          return kProfileNormal;
        }
    }

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
            // Halved offset ranges vs idle's Small observation (was ±150° / ±80°),
            // further scaled by Phase 3's amplitude_scale (1.0 = Phase 2 default).
            int yaw_range    = static_cast<int>(75 * _amplitude_scale);
            int pitch_range  = static_cast<int>(40 * _amplitude_scale);
            // Guard against zero-range getInt(0,0) — Random returns the bound.
            if (yaw_range   < 1) yaw_range   = 1;
            if (pitch_range < 1) pitch_range = 1;
            auto current     = motion.getCurrentAngles();
            int diff_yaw     = Random::getInstance().getInt(-yaw_range, yaw_range);
            int diff_pitch   = Random::getInstance().getInt(-pitch_range, pitch_range);
            int target_yaw   = uitk::clamp(current.x + diff_yaw, -800, 800);
            int target_pitch = uitk::clamp(current.y + diff_pitch, 0, 600);
            int speed        = Random::getInstance().getInt(100, 250);
            motion.moveWithSpeed(target_yaw, target_pitch, speed, "idle_motion_overlay");
        } else {
            // Return-to-center: yaw → 0, gentle pitch shift. Face_tracking
            // will pull back to the locked face on the next detection tick.
            int target_pitch = Random::getInstance().getInt(50, 400);
            int speed        = Random::getInstance().getInt(100, 300);
            motion.moveWithSpeed(0, target_pitch, speed, "idle_motion_overlay");
        }
    }

    // Phase 3 — profile-driven action set. Three actions only (was four),
    // tuned per IdleProfile. The dropped pre-Phase-3 "quick glance" branch
    // (±500° yaw at speed 250–400) was the dominant cause of the violent
    // post-photo head-snap the user complained about.
    //
    // All three actions go through Servo::moveWithSpeed, which uses
    // critically-damped spring physics (servo.cpp:84) — the trajectory to
    // the target is already smooth. Smoothness work in Phase 3 is therefore
    // about target SELECTION (delta size, cadence, pitch baseline) not about
    // adding a new easing engine.
    static const char* profileName(IdleProfile p)
    {
        switch (p) {
            case IdleProfile::NORMAL:         return "normal";
            case IdleProfile::LOOKING_AROUND: return "looking_around";
            case IdleProfile::SLEEPY:         return "sleepy";
            case IdleProfile::SURVEILLANCE:   return "surveillance";
        }
        return "unknown";
    }

    void perform_idle_motion(Modifiable& stackchan)
    {
        auto& motion = stackchan.motion();
        if (motion.isModifyLocked()) {
            return;
        }

        const auto& p = paramsFor(_profile);
        int action    = Random::getInstance().getInt(0, 100);
        const char* action_name =
            (action < 60) ? "small_offset" :
            (action < 85) ? "gentle_look"  :
                            "return_to_center";
        char ev_buf[96];
        std::snprintf(ev_buf, sizeof(ev_buf),
                      "{\"action\":\"%s\",\"profile\":\"%s\"}",
                      action_name, profileName(_profile));
        Application::GetInstance().SendEvent("idle_motion", ev_buf);

        if (action < 60) {
            // Action A: small offset from current — the "fidget" / "subtle
            // life" tick. Most common because it never leaves the user's
            // peripheral; reads as breathing rather than reacting.
            auto current     = motion.getCurrentAngles();
            int diff_yaw     = Random::getInstance().getInt(-p.yaw_delta_range, p.yaw_delta_range);
            int diff_pitch   = Random::getInstance().getInt(-p.pitch_delta_range, p.pitch_delta_range);
            int target_yaw   = uitk::clamp(current.x + diff_yaw, -800, 800);
            int target_pitch = uitk::clamp(current.y + diff_pitch, 0, 600);
            int speed        = Random::getInstance().getInt(p.speed_min, p.speed_max);
            motion.moveWithSpeed(target_yaw, target_pitch, speed, "idle_motion_small_offset");
        } else if (action < 85) {
            // Action B: gentle look — moderate yaw target with profile-bounded
            // pitch baseline. The pitch baseline is what gives SLEEPY its
            // droopy feel (high pitch = head down) and SURVEILLANCE its
            // attentive level head.
            int target_yaw   = Random::getInstance().getInt(-p.gentle_yaw_range, p.gentle_yaw_range);
            int target_pitch = Random::getInstance().getInt(p.pitch_baseline_min, p.pitch_baseline_max);
            int speed        = Random::getInstance().getInt(p.speed_min, p.speed_max);
            motion.moveWithSpeed(target_yaw, target_pitch, speed, "idle_motion_gentle_look");
        } else {
            // Action C: return to centre. Yaw to 0, pitch to baseline. Resets
            // accumulated drift so Dotty doesn't end up locked in one quadrant
            // after enough small-offset ticks.
            int target_pitch = Random::getInstance().getInt(p.pitch_baseline_min, p.pitch_baseline_max);
            int speed        = Random::getInstance().getInt(p.speed_min, p.speed_max);
            motion.moveWithSpeed(0, target_pitch, speed, "idle_motion_return_center");
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
    // Phase 3 — chat-state-driven tunables for the tracking-overlay path.
    // Defaults match Phase 2's constexpr values; setIntervalRange and
    // setAmplitudeScale (called by face_tracking on chat-state transitions)
    // override them per profile.
    uint32_t _tracking_overlay_min_ms = kTrackingOverlayMinMs;
    uint32_t _tracking_overlay_max_ms = kTrackingOverlayMaxMs;
    float    _amplitude_scale         = 1.0f;
    // Empty-room backoff state: stamps the time of the last face-locked
    // → unlocked transition (or boot, whichever is later). Constructor
    // initialises to GetHAL().millis() so the timer starts counting from
    // power-on; setTrackingMode(false) re-stamps it on each face_lost.
    uint32_t _last_tracking_active_ms = 0;
    // Phase 3 — current high-level idle profile. Drives perform_idle_motion's
    // action ranges + cadence. ModeManager (Phase 4) sets via setIdleProfile.
    IdleProfile _profile = IdleProfile::NORMAL;
};

}  // namespace stackchan
