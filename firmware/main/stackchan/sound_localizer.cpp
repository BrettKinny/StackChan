/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "sound_localizer.h"

#include "application.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstdio>

#define TAG "SoundLocalizer"

namespace stackchan {

void SoundLocalizer::OnStereoFrame(const std::vector<int16_t>& interleaved_lr)
{
    // Cooldown gate first — avoids the per-sample work most of the time.
    int64_t now = esp_timer_get_time();
    if (now - _last_emit_us < kCooldownUs) {
        return;
    }

    // Interleaved [L0, R0, L1, R1, ...]. ES7210 slot 0 = MIC1 (treated
    // as 'left'), slot 1 = MIC2 ('right'). If real-world testing shows
    // the polarity reversed for the CoreS3's physical mic layout, swap
    // the indices below.
    //
    // Each sample is run through a per-channel 1st-order high-pass at
    // ~300 Hz before energy accumulation, so HVAC / fan / aircon rumble
    // (which lives mostly <200 Hz) doesn't trip the energy gate. Speech
    // formants (500-3000 Hz) pass through cleanly; that's plenty of
    // signal for direction localisation, which only needs the L/R
    // intensity ratio anyway.
    int64_t left_energy  = 0;
    int64_t right_energy = 0;
    const size_t n = interleaved_lr.size() & ~size_t(1);  // even count
    for (size_t i = 0; i + 1 < n; i += 2) {
        const float l_in = float(interleaved_lr[i]);
        const float r_in = float(interleaved_lr[i + 1]);

        // y[n] = α (y[n-1] + x[n] - x[n-1])  — 1st-order HPF
        const float l_hp = kHpAlpha * (_hp_l_prev_y + l_in - _hp_l_prev_x);
        const float r_hp = kHpAlpha * (_hp_r_prev_y + r_in - _hp_r_prev_x);
        _hp_l_prev_x = l_in;  _hp_l_prev_y = l_hp;
        _hp_r_prev_x = r_in;  _hp_r_prev_y = r_hp;

        left_energy  += int64_t(l_hp) * int64_t(l_hp);
        right_energy += int64_t(r_hp) * int64_t(r_hp);
    }
    const int64_t total = left_energy + right_energy;

    // Energy gate — silence threshold.
    if (total < kEnergyThreshold) {
        return;
    }

    const double balance = double(left_energy - right_energy) / double(total);
    const Direction dir  = Localize(balance);

    // Only emit on direction CHANGE, so a sustained sound from one
    // direction doesn't spam the bus.
    if (dir == _last_dir) {
        return;
    }

    const char* dir_str = (dir == Direction::Left)  ? "left"
                        : (dir == Direction::Right) ? "right"
                        :                             "centre";

    // ESP-IDF newlib printf doesn't reliably support %lld here, so
    // emit energy as a float ("normalised so JSON stays well-formed").
    const double energy_d = static_cast<double>(total);
    char data_json[96];
    int len = snprintf(data_json, sizeof(data_json),
        "{\"direction\":\"%s\",\"balance\":%.3f,\"energy\":%.0f}",
        dir_str, balance, energy_d);
    if (len < 0 || len >= static_cast<int>(sizeof(data_json))) {
        return;
    }

    ESP_LOGI(TAG, "sound_event: %s balance=%.3f energy=%.0f",
             dir_str, balance, energy_d);
    Application::GetInstance().SendEvent("sound_event", data_json);

    _last_emit_us = now;
    _last_dir     = dir;
}

SoundLocalizer::Direction SoundLocalizer::Localize(double balance) const
{
    if (balance >  kBalanceThreshold) return Direction::Left;
    if (balance < -kBalanceThreshold) return Direction::Right;
    return Direction::Centre;
}

}  // namespace stackchan
