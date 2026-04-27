/*
 * SPDX-FileCopyrightText: 2026 Brett Kinny / squarewavesystems
 *
 * SPDX-License-Identifier: MIT
 */
#include "camera_stream_guard.h"

#include <atomic>
#include <cstdint>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <hal/board/hal_bridge.h>
#include <hal/board/stackchan_camera.h>

namespace stackchan::camera {

namespace {

constexpr const char* kTag = "CameraStreamGuard";

// Recursive mutex serialises refcount transitions and the corresponding
// startStreaming()/stopStreaming() calls. Recursive so a task that
// somehow nests a guard (e.g. Capture() invoked from a callback that
// already holds one) cannot deadlock — non-recursive deadlock here
// would silently freeze the camera. Magic-static init gives us
// thread-safe one-time creation in C++11.
SemaphoreHandle_t lifecycleMutex()
{
    static SemaphoreHandle_t handle = xSemaphoreCreateRecursiveMutex();
    return handle;
}

std::atomic<uint32_t> g_refcount{0};

}  // namespace

CameraStreamGuard::CameraStreamGuard()
{
    SemaphoreHandle_t m = lifecycleMutex();
    if (m == nullptr) {
        ESP_LOGE(kTag, "lifecycle mutex unavailable; stream lifecycle skipped");
        return;
    }

    xSemaphoreTakeRecursive(m, portMAX_DELAY);
    const uint32_t old_rc = g_refcount.fetch_add(1, std::memory_order_acq_rel);
    if (old_rc == 0) {
        // 0 → 1: first consumer in; bring the V4L2 stream up.
        if (auto* cam = hal_bridge::board_get_camera()) {
            if (!cam->startStreaming()) {
                ESP_LOGE(kTag, "startStreaming failed");
            }
        } else {
            ESP_LOGW(kTag, "board_get_camera returned null at 0→1 acquire");
        }
    }
    xSemaphoreGiveRecursive(m);
}

CameraStreamGuard::~CameraStreamGuard()
{
    SemaphoreHandle_t m = lifecycleMutex();
    if (m == nullptr) {
        return;
    }

    xSemaphoreTakeRecursive(m, portMAX_DELAY);
    const uint32_t new_rc = g_refcount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (new_rc == 0) {
        // 1 → 0: last consumer out; tear the stream down.
        if (auto* cam = hal_bridge::board_get_camera()) {
            cam->stopStreaming();
        }
    }
    xSemaphoreGiveRecursive(m);
}

}  // namespace stackchan::camera
