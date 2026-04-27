/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace stackchan {

// Ambient sound localizer for the M5Stack CoreS3 dual-mic configuration.
//
// The CoreS3 has two on-board MEMS mics through an ES7210 codec. The
// firmware's audio input task (xiaozhi-esp32 audio_service) reads
// interleaved L/R PCM frames at 16 kHz / 10 ms and downmixes to mono
// for ASR; before the downmix it now also fires a stereo callback,
// which this class consumes.
//
// The sub-cm baseline between the two mics rules out reliable
// time-of-arrival (ITD) localization for human voice frequencies.
// We use the **interaural intensity difference (IID)** instead:
// integrate per-channel energy across the frame and bucket the
// balance into Left / Centre / Right. Coarse but stable.
//
// Cooldown'd; emits {"type":"event","name":"sound_event","data":{
//   "direction":"left|centre|right","balance":<float>,
//   "energy":<int64>}} via Application::SendEvent on direction CHANGE.
class SoundLocalizer {
public:
    SoundLocalizer() = default;

    // Canonical singleton — same instance read by Application::HandleWakeWordDetectedEvent
    // (via GetRecentDirection) and written by the audio task callback registered in
    // stackchan_display.cc. Function-local static; safe to call before/after init.
    static SoundLocalizer& Instance();

    // Called from the audio input task (Core 0 / dedicated task — not
    // the main task). Frame is 320 int16_t at 16 kHz / 10 ms when
    // input_channels == 2. Cheap (zero-cost when in cooldown).
    void OnStereoFrame(const std::vector<int16_t>& interleaved_lr);

    // Snapshot of the dominant direction over the past `window_us` microseconds.
    // Reads the ring buffer written by OnStereoFrame; aggregates per-channel
    // energy across the window; returns L/C/R using the same kBalanceThreshold
    // rule as the live Localize(). Used by HandleWakeWordDetectedEvent to
    // attach a direction to the wake event. Safe to call from any task.
    struct DirSummary {
        const char* direction;  // "left" | "centre" | "right"
        double      balance;
        double      energy;
    };
    DirSummary GetRecentDirection(int64_t window_us = 1500000) const;

private:
    enum class Direction { Left, Centre, Right };

    Direction Localize(double balance) const;

    int64_t   _last_emit_us  = 0;
    Direction _last_dir      = Direction::Centre;

    // Per-channel 1st-order high-pass filter state (in float).
    // y[n] = α (y[n-1] + x[n] - x[n-1]). One mult + two adds per sample;
    // negligible on ESP32-S3 with FPU at 16 kHz.
    // α chosen for ~300 Hz cutoff at 16 kHz sample rate
    // (RC / (RC + dt) where RC = 1/(2π·300), dt = 1/16000).
    float _hp_l_prev_x = 0.0f;
    float _hp_l_prev_y = 0.0f;
    float _hp_r_prev_x = 0.0f;
    float _hp_r_prev_y = 0.0f;

    static constexpr int64_t kCooldownUs       = 750000;   // 750 ms
    // 1.2B chosen to suppress ambient conversation cluster (500M-900M
    // observed in live data 2026-04-27) while still catching genuine
    // claps (1B+). Halve cycle: rev'd from 500M after the activity feed
    // was being drowned by ambient noise. Earlier history: 1e9 → 5e8 with
    // the HPF added in 57e12cb (claps fell below the original threshold);
    // now 5e8 → 1.2e9 once the HPF settled and we had ambient-vs-clap
    // energy data to set a real boundary.
    static constexpr int64_t kEnergyThreshold  = 1200000000;
    static constexpr double  kBalanceThreshold = 0.15;     // L vs R fraction
    // 1st-order HPF coefficient. ~300 Hz cutoff, -3 dB at 300 Hz, -6 dB/oct
    // rolloff below. Knocks out HVAC / fan rumble (typically <200 Hz dominant)
    // while passing speech (fundamental ~100-300 Hz, formants 500-3000 Hz —
    // formants alone carry enough energy for direction localisation).
    static constexpr float   kHpAlpha          = 0.8946f;  // for fc=300Hz, fs=16kHz

    // Ring of per-frame energy samples. Written by OnStereoFrame on the
    // audio task, read by GetRecentDirection on whoever calls it. The head
    // index is atomic with release/acquire semantics; slot data is plain
    // (a torn read of one slot among ~150 in the aggregation window is
    // dominated by the rest — acceptable for direction estimation).
    struct DirSample {
        int64_t ts_us;
        int64_t left_energy;
        int64_t right_energy;
    };
    static constexpr size_t   kRingSize = 200;  // ~2 s at 10 ms frames
    DirSample                 _ring[kRingSize] = {};
    std::atomic<size_t>       _ring_head{0};
};

}  // namespace stackchan
