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

SoundLocalizer& SoundLocalizer::Instance()
{
    static SoundLocalizer s;
    return s;
}

void SoundLocalizer::OnStereoFrame(const std::vector<int16_t>& interleaved_lr)
{
    // Per-frame energy computation runs UNCONDITIONALLY (no cooldown gate
    // here). The ring buffer must record every frame so wake-word handlers
    // calling GetRecentDirection during the 750 ms emit-cooldown still see
    // valid direction data. Cost: ~320 FPU ops per 10 ms frame on the S3 —
    // negligible.
    //
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
    const int64_t now = esp_timer_get_time();
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

    // Record into the ring before any emit gating. Reader uses release
    // semantics on the head index; slot writes themselves race-tolerant
    // (one bad slot among ~150 in the aggregation window is dominated).
    const size_t slot = _ring_head.load(std::memory_order_relaxed) % kRingSize;
    _ring[slot].ts_us        = now;
    _ring[slot].left_energy  = left_energy;
    _ring[slot].right_energy = right_energy;
    _ring_head.fetch_add(1, std::memory_order_release);

    const int64_t total = left_energy + right_energy;

    // Emit gates: cooldown, then energy, then direction-change. These
    // throttle the perception bus; the ring buffer above is unaffected.
    if (now - _last_emit_us < kCooldownUs) {
        return;
    }
    if (total < kEnergyThreshold) {
        return;
    }

    const double balance = double(left_energy - right_energy) / double(total);
    const Direction dir  = Localize(balance);

    // Direction-change gate removed 2026-04-27. It was filtering out
    // legitimate clap events from the same side as the most recent
    // emit (e.g. boot self-test fires `left`, then every clap from
    // the left is silently dropped). The 750 ms cooldown alone bounds
    // emit rate to ~1.3 events/sec, which handles the original spam
    // concern for sustained sounds. _last_dir is no longer consulted
    // for gating but kept around in case future logic needs it.

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

SoundLocalizer::DirSummary SoundLocalizer::GetRecentDirection(int64_t window_us) const
{
    // Acquire pairs with the release in OnStereoFrame's fetch_add. Anything
    // we read from the ring with index < head was published before this load.
    const size_t head = _ring_head.load(std::memory_order_acquire);
    const int64_t now = esp_timer_get_time();
    const int64_t cutoff = now - window_us;

    int64_t l_sum = 0;
    int64_t r_sum = 0;
    // Walk backwards from head until we exit the time window or wrap the ring.
    const size_t walk = head < kRingSize ? head : kRingSize;
    for (size_t i = 0; i < walk; ++i) {
        const size_t idx = (head - 1 - i) % kRingSize;
        const DirSample& s = _ring[idx];
        if (s.ts_us < cutoff) break;
        l_sum += s.left_energy;
        r_sum += s.right_energy;
    }

    DirSummary out{ "centre", 0.0, 0.0 };
    const int64_t total = l_sum + r_sum;
    if (total < kEnergyThreshold) {
        // Sub-threshold: not enough audio in the window to call a direction.
        return out;
    }
    const double balance = double(l_sum - r_sum) / double(total);
    if (balance >  kBalanceThreshold) out.direction = "left";
    else if (balance < -kBalanceThreshold) out.direction = "right";
    else out.direction = "centre";
    out.balance = balance;
    out.energy  = double(total);
    return out;
}

SoundLocalizer::Direction SoundLocalizer::Localize(double balance) const
{
    if (balance >  kBalanceThreshold) return Direction::Left;
    if (balance < -kBalanceThreshold) return Direction::Right;
    return Direction::Centre;
}

}  // namespace stackchan
