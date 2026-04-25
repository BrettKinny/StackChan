/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdint>
#include <string>

namespace stackchan {

class FaceDetector {
public:
    static FaceDetector& getInstance();

    void start();
    void stop();
    void setEnabled(bool enabled);
    bool isEnabled() const { return _enabled.load(std::memory_order_acquire); }

private:
    FaceDetector() = default;
    static void taskEntry(void* arg);
    void processFrame();

    // Layer 4 (face recognition) tuning — kept here so reverts are local.
    // kRecognizeThrottleMs : minimum gap between recognize() invocations
    //                       while a face is in frame. Embedding inference
    //                       is expensive; recognition is for greeting /
    //                       personalization, not tracking, so once every
    //                       2 s is plenty.
    // kEmitStaleMs         : minimum gap between repeat face_recognized
    //                       emissions of the SAME identity. Identity-
    //                       change always emits immediately; this only
    //                       refreshes a stale value so the bridge can
    //                       distinguish "still here" from "left and not
    //                       come back".
    static constexpr uint32_t kRecognizeThrottleMs = 2000;
    static constexpr uint32_t kEmitStaleMs         = 30 * 1000;

    TaskHandle_t _task_handle = nullptr;
    std::atomic<bool> _enabled{false};
    std::atomic<bool> _running{false};
    SemaphoreHandle_t _stop_sem = nullptr;
    uint8_t* _rgb_buffer = nullptr;

    // Face-recognition gating state. Single-task access (face_det only),
    // so plain members are fine.
    uint32_t    _last_recognize_ms     = 0;
    uint32_t    _last_emitted_ms       = 0;
    std::string _last_emitted_identity;
};

}  // namespace stackchan
