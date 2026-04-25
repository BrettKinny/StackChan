/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_detector.h"
#include "face_detection_result.h"
#include "camera_arbiter.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <hal/board/stackchan_camera.h>

#include "human_face_detect.hpp"

#define TAG "FaceDetector"

static constexpr int FRAME_W = 320;
static constexpr int FRAME_H = 240;
static constexpr size_t RGB_BUF_SIZE = FRAME_W * FRAME_H * 3;

namespace stackchan {

FaceDetector& FaceDetector::getInstance()
{
    static FaceDetector instance;
    return instance;
}

void FaceDetector::start()
{
    if (_task_handle != nullptr) return;
    _running.store(true, std::memory_order_release);
    _stop_sem = xSemaphoreCreateBinary();

    _rgb_buffer = (uint8_t*)heap_caps_malloc(RGB_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for RGB buffer", (int)RGB_BUF_SIZE);
        _running.store(false, std::memory_order_release);
        if (_stop_sem) {
            vSemaphoreDelete(_stop_sem);
            _stop_sem = nullptr;
        }
        return;
    }

    xTaskCreatePinnedToCore(taskEntry, "face_det", 16384, this, 1, &_task_handle, 0);
    ESP_LOGI(TAG, "Face detector task started on Core 0");
}

void FaceDetector::stop()
{
    _running.store(false, std::memory_order_release);
    _enabled.store(false, std::memory_order_release);
    if (_task_handle) {
        xSemaphoreTake(_stop_sem, pdMS_TO_TICKS(2000));
        _task_handle = nullptr;
    }
    if (_stop_sem) {
        vSemaphoreDelete(_stop_sem);
        _stop_sem = nullptr;
    }
    if (_rgb_buffer) {
        heap_caps_free(_rgb_buffer);
        _rgb_buffer = nullptr;
    }
    ESP_LOGI(TAG, "Face detector stopped");
}

void FaceDetector::setEnabled(bool enabled)
{
    _enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        auto& result = GetFaceDetectionResult();
        result.write(false, 0, 0, 0, GetHAL().millis());
    }
    ESP_LOGI(TAG, "Face detector %s", enabled ? "enabled" : "disabled");
}

void FaceDetector::taskEntry(void* arg)
{
    auto* self = static_cast<FaceDetector*>(arg);

    while (self->_running.load(std::memory_order_acquire)) {
        if (!self->_enabled.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        self->processFrame();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    xSemaphoreGive(self->_stop_sem);
    vTaskDelete(nullptr);
}

void FaceDetector::processFrame()
{
    auto& arbiter = CameraArbiter::getInstance();
    if (!arbiter.tryAcquireForDetection()) return;

    auto* camera = hal_bridge::board_get_camera();
    if (!camera) {
        ESP_LOGW(TAG, "Camera unavailable");
        arbiter.releaseForDetection();
        return;
    }

    if (!camera->StreamCaptures()) {
        ESP_LOGW(TAG, "StreamCaptures failed");
        arbiter.releaseForDetection();
        return;
    }

    const uint8_t* frame_data = camera->GetFrameData();
    int frame_w               = camera->GetFrameWidth();
    int frame_h               = camera->GetFrameHeight();
    int frame_fmt              = camera->GetFrameFormat();

    if (!frame_data || frame_w <= 0 || frame_h <= 0) {
        ESP_LOGW(TAG, "No frame data");
        arbiter.releaseForDetection();
        return;
    }

    // Bail before the heavy YUV→RGB + inference if disable was requested
    // while we were waiting for the camera frame.
    if (!_enabled.load(std::memory_order_acquire)) {
        arbiter.releaseForDetection();
        return;
    }

    // Map our V4L2 format to ESP-DL pix_type. The model's preprocessor
    // handles conversion to whatever the model needs.
    dl::image::pix_type_t pix_type;
    if (frame_fmt == V4L2_PIX_FMT_YUYV || frame_fmt == V4L2_PIX_FMT_YUV422P) {
        pix_type = dl::image::DL_IMAGE_PIX_TYPE_YUYV;
    } else if (frame_fmt == V4L2_PIX_FMT_RGB565) {
        pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
    } else if (frame_fmt == V4L2_PIX_FMT_RGB24) {
        pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
    } else {
        ESP_LOGW(TAG, "unsupported frame format 0x%08x", (unsigned)frame_fmt);
        arbiter.releaseForDetection();
        return;
    }

    // Defaults (0.5/0.5) are too strict for real-world conditions
    // (kid faces, poor lighting). Lower once on first use.
    static HumanFaceDetect detector;
    static bool detector_configured = false;
    if (!detector_configured) {
        detector.set_score_thr(0.25f, 0);  // MSR (stage 1)
        detector.set_score_thr(0.30f, 1);  // MNP (stage 2)
        detector_configured = true;
        ESP_LOGI(TAG, "Detector configured: MSR thr=0.25, MNP thr=0.30");
    }

    dl::image::img_t img = {
        .data     = (void*)frame_data,
        .width    = (uint16_t)frame_w,
        .height   = (uint16_t)frame_h,
        .pix_type = pix_type,
    };
    auto& results = detector.run(img);
    arbiter.releaseForDetection();

    auto& result = GetFaceDetectionResult();
    uint32_t now = GetHAL().millis();

    if (!results.empty()) {
        auto& best = results.front();
        // best.box = [x1, y1, x2, y2]
        float bbox_cx = (best.box[0] + best.box[2]) / 2.0f;
        float bbox_cy = (best.box[1] + best.box[3]) / 2.0f;
        float bbox_w  = best.box[2] - best.box[0];
        float bbox_h  = best.box[3] - best.box[1];

        // Map to normalized coords; non-mirrored camera so X is direct,
        // Y is inverted (image y-down vs servo y-up).
        float norm_x = (bbox_cx / (frame_w - 1.0f)) * 2.0f - 1.0f;
        float norm_y = -((bbox_cy / (frame_h - 1.0f)) * 2.0f - 1.0f);
        float face_size = (bbox_w * bbox_h) / (float)(frame_w * frame_h);

        result.write(true, norm_x, norm_y, face_size, now);

        ESP_LOGD(TAG, "face bbox=[%d,%d,%d,%d] score=%.2f norm=(%.2f,%.2f)",
                 best.box[0], best.box[1], best.box[2], best.box[3], best.score,
                 norm_x, norm_y);
    } else {
        result.write(false, 0, 0, 0, now);
    }
}

}  // namespace stackchan
