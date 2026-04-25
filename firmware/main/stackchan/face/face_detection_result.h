/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <atomic>

namespace stackchan {

struct FaceDetectionResult {
    bool face_detected = false;
    float norm_x       = 0.0f;  // [-1, 1] horizontal (mirrored for camera)
    float norm_y       = 0.0f;  // [-1, 1] vertical (inverted Y)
    float face_size    = 0.0f;  // relative size [0, 1]
    uint32_t timestamp_ms = 0;

    // Seqlock for single-writer (Core 0) / single-reader (Core 1)
    std::atomic<uint32_t> seq{0};

    void write(bool detected, float x, float y, float size, uint32_t ts)
    {
        seq.store(seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        face_detected = detected;
        norm_x        = x;
        norm_y        = y;
        face_size     = size;
        timestamp_ms  = ts;
        seq.store(seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    bool read(bool& detected, float& x, float& y, float& size, uint32_t& ts) const
    {
        for (int attempt = 0; attempt < 4; attempt++) {
            uint32_t s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1) continue;  // write in progress
            detected = face_detected;
            x        = norm_x;
            y        = norm_y;
            size     = face_size;
            ts       = timestamp_ms;
            uint32_t s2 = seq.load(std::memory_order_acquire);
            if (s1 == s2) return true;
        }
        return false;  // torn read after retries
    }
};

FaceDetectionResult& GetFaceDetectionResult();

}  // namespace stackchan
