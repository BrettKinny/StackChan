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

    TaskHandle_t _task_handle = nullptr;
    std::atomic<bool> _enabled{false};
    std::atomic<bool> _running{false};
    SemaphoreHandle_t _stop_sem = nullptr;
    uint8_t* _rgb_buffer = nullptr;
};

}  // namespace stackchan
