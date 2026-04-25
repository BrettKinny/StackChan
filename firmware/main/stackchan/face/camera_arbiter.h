/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>

namespace stackchan {

class CameraArbiter {
public:
    static CameraArbiter& getInstance();

    bool tryAcquireForDetection()
    {
        // Take the mutex first, then re-check _capture_pending while holding
        // it. Checking the flag before the take is a TOCTOU race: Capture()
        // can set the flag between our check and the take, after which we'd
        // hold the mutex and force Capture() to wait up to its 2 s timeout.
        // With the check inside the mutex region, if Capture() has already
        // signaled intent we yield immediately and let it proceed.
        if (xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
        if (_capture_pending.load(std::memory_order_acquire)) {
            xSemaphoreGive(_mutex);
            return false;
        }
        return true;
    }

    void releaseForDetection()
    {
        xSemaphoreGive(_mutex);
    }

    bool acquireForCapture(uint32_t timeout_ms = 2000)
    {
        _capture_pending.store(true, std::memory_order_release);
        bool got = xSemaphoreTake(_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
        if (!got) _capture_pending.store(false, std::memory_order_release);
        return got;
    }

    void releaseForCapture()
    {
        _capture_pending.store(false, std::memory_order_release);
        xSemaphoreGive(_mutex);
    }

    bool isCapturePending() const
    {
        return _capture_pending.load(std::memory_order_acquire);
    }

private:
    CameraArbiter();
    SemaphoreHandle_t _mutex;
    std::atomic<bool> _capture_pending{false};
};

}  // namespace stackchan
