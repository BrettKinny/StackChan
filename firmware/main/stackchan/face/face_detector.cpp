/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_detector.h"
#include "face_detection_result.h"
#include "face_recognizer.h"
#include "camera_arbiter.h"
#include <stackchan/privacy/camera_peripheral_guard.h>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <hal/board/stackchan_camera.h>

#include <cstdio>
#include <string>

#include "human_face_detect.hpp"
#include "application.h"  // Layer 4: face_recognized perception event

#define TAG "FaceDetector"

static constexpr int FRAME_W = 320;
static constexpr int FRAME_H = 240;
static constexpr size_t RGB_BUF_SIZE = FRAME_W * FRAME_H * 3;

// ---------------------------------------------------------------------------
// Detector tuning knobs — keep all magic numbers here so reverts are one-line.
//
// kMsrScoreThr / kMnpScoreThr
//   Score thresholds for the two-stage MSR + MNP pipeline.
//   Defaults shipped by ESP-DL are 0.5 / 0.5 (too strict for kid faces in
//   poor lighting). We previously ran 0.25 / 0.30. We've now raised the MSR
//   threshold from 0.25 → 0.40 as a faster-inference tweak: MSR is the cheap
//   stage 1 proposer (~5 ms), MNP is the expensive stage 2 refiner (~33 ms
//   per candidate). Raising MSR culls weak proposals before they hit MNP,
//   so per-frame inference drops when nothing is in frame and the stage-2
//   pass is largely skipped. MNP threshold stays loose at 0.30 so the
//   refined detections aren't too picky for our use case.
//
//   *** Blocker note (MNP-only single-stage):
//       The original task asked for MNP_S8_V1 single-stage (~5ms vs 38ms).
//       The HumanFaceDetect wrapper API in
//       managed_components/espressif__human_face_detect/human_face_detect.hpp
//       only exposes MSRMNP_S8_V1 and ESPDET_PICO_{224,416} as model_type_t
//       enums. The MNP class is NOT a dl::detect::Detect subclass and its
//       run() requires an MSR-produced candidate list (it's a refiner, not
//       a standalone detector). ESPDET_PICO_224_224_FACE is the obvious
//       single-stage swap-in but needs its own .espdl flashed via menuconfig
//       (CONFIG_FLASH_ESPDET_PICO_224_224_FACE) — a separate build/flash
//       change Brett can take next session. For now we stick with MSRMNP
//       and tighten the MSR threshold instead.
//
// kEspdlPixelTypeFromYuyv etc — picked by V4L2 fmt at runtime; not tunable.
// ---------------------------------------------------------------------------
static constexpr float kMsrScoreThr = 0.40f;  // was 0.25f — raise to skip MNP more often
static constexpr float kMnpScoreThr = 0.30f;  // unchanged

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

    // Layer 4: bring the recognizer's NVS-backed enrollment cache up
    // before the detector task starts firing recognize() calls. init()
    // is idempotent and best-effort — a failure leaves the recognizer in
    // a degraded "always returns unknown" state without breaking
    // detection.
    if (!FaceRecognizer::getInstance().init()) {
        ESP_LOGW(TAG, "FaceRecognizer init returned false (degraded mode)");
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

        // Privacy LED: hold camera-active for the ENTIRE _enabled period,
        // not per-frame. Without this, the 50 ms vTaskDelay between
        // processFrame cycles makes the red privacy LED visibly flicker
        // at the inference cadence (~3 Hz), and the optical cross-talk
        // makes the adjacent green mic LED look like it's also flickering.
        // Inner loop runs while _enabled (and _running) stay true; guard
        // dtor fires when either flips false → setCameraState(Off) once.
        // Step 4-5 (refcounted CameraPeripheralGuard tied to STREAMON)
        // replaces this with the proper privacy invariant.
        {
            stackchan::privacy::CameraPeripheralGuard detector_active_guard;
            while (self->_enabled.load(std::memory_order_acquire) &&
                   self->_running.load(std::memory_order_acquire)) {
                self->processFrame();
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
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

    // Privacy LED guard is now held ONE LEVEL UP in taskEntry, scoped to
    // the entire _enabled period. See the comment there for why.

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
    // (kid faces, poor lighting). See kMsrScoreThr / kMnpScoreThr at the
    // top of this file for tuning rationale.
    static HumanFaceDetect detector;
    static bool detector_configured = false;
    if (!detector_configured) {
        detector.set_score_thr(kMsrScoreThr, 0);  // MSR (stage 1)
        detector.set_score_thr(kMnpScoreThr, 1);  // MNP (stage 2)
        detector_configured = true;
        ESP_LOGI(TAG, "Detector configured: MSR thr=%.2f, MNP thr=%.2f",
                 kMsrScoreThr, kMnpScoreThr);
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

        // Layer 4: on-device face recognition. We run this AFTER we've
        // already published the bbox to FaceDetectionResult so the
        // tracking modifier sees the position with zero recognizer
        // latency on its critical path.
        //
        // Throttled to once per kRecognizeThrottleMs because:
        //   - the embedding model (when wired) costs ~30 ms+ per crop;
        //   - the bridge only needs identity for greeting / personalization,
        //     not at frame rate;
        //   - emitting an event every frame would flood the WS protocol.
        //
        // SCAFFOLD: FaceRecognizer::recognize() always returns "unknown"
        // until ESP-DL face_recognition.so is wired in face_recognizer.cpp.
        if (now - _last_recognize_ms >= kRecognizeThrottleMs) {
            _last_recognize_ms = now;

            FaceImage face_img{};
            face_img.data    = frame_data;
            face_img.width   = frame_w;
            face_img.height  = frame_h;
            face_img.pix_fmt = (int)frame_fmt;
            // Pass bbox in source-image coords; recognizer will crop.
            // Clamp negative values defensively even though best.box is
            // generally well-formed from the model.
            face_img.bbox_x1 = best.box[0] < 0 ? 0 : best.box[0];
            face_img.bbox_y1 = best.box[1] < 0 ? 0 : best.box[1];
            face_img.bbox_x2 = best.box[2] < 0 ? 0 : best.box[2];
            face_img.bbox_y2 = best.box[3] < 0 ? 0 : best.box[3];

            std::string identity;
            try {
                identity = FaceRecognizer::getInstance().recognize(face_img);
            } catch (...) {
                // recognize() never throws today, but keep the guard so a
                // future ESP-DL hookup that does throw can't take down the
                // detector task.
                identity = FaceRecognizer::kUnknown;
                ESP_LOGW(TAG, "recognize threw — falling back to unknown");
            }
            if (identity.empty()) identity = FaceRecognizer::kUnknown;

            // Emit only on identity transition OR if enough time has
            // passed since the last emit, to keep the bus quiet when
            // the same person is in frame for a long stretch. The
            // throttle above already gates re-running recognize(); this
            // additional edge gate keeps the WS chatter to a minimum.
            bool changed = (identity != _last_emitted_identity);
            bool stale = (now - _last_emitted_ms) >= kEmitStaleMs;
            if (changed || stale) {
                _last_emitted_identity = identity;
                _last_emitted_ms       = now;

                // JSON-escape the identity so a future name with a quote
                // can't break the payload. Names are length-bounded and
                // control-char-rejected at enrollment time, so quote and
                // backslash are the only realistic problems here.
                std::string esc;
                esc.reserve(identity.size() + 2);
                for (char c : identity) {
                    if (c == '"' || c == '\\') esc.push_back('\\');
                    esc.push_back(c);
                }
                char payload[96];
                std::snprintf(payload, sizeof(payload),
                              R"({"identity":"%s"})", esc.c_str());
                Application::GetInstance().SendEvent("face_recognized", payload);
                ESP_LOGI(TAG, "face_recognized identity=%s", identity.c_str());
            }
        }
    } else {
        result.write(false, 0, 0, 0, now);
        // No face in frame: clear the emitted-identity tracker so the
        // next acquisition emits a fresh face_recognized even if it's
        // the same person who just stepped out and back in.
        _last_emitted_identity.clear();
    }
}

}  // namespace stackchan
