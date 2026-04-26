/*
 * SPDX-FileCopyrightText: 2026 Brett Kinny / squarewavesystems
 *
 * SPDX-License-Identifier: MIT
 */
#include "privacy_leds.h"

#include <hal/hal.h>
#include <esp_log.h>          // esp_log_timestamp() for pulse phase
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mooncake_log.h>

namespace stackchan::privacy {

static constexpr const char* _tag = "PrivacyLeds";

PrivacyLeds& PrivacyLeds::getInstance()
{
    static PrivacyLeds instance;
    return instance;
}

void PrivacyLeds::update()
{
    // Skip while runBootSelfTest() owns the two privacy pixels. Otherwise
    // the 10 ms stackchan tick would overwrite each test stage almost
    // immediately, making the self-test invisible. (In practice the update
    // task isn't running yet during init — boot self-test happens before
    // startXiaozhi() — but this is belt-and-braces against re-ordering.)
    if (_inhibit_update.load(std::memory_order_acquire)) {
        return;
    }
    auto& hal = GetHAL();

    // Pulse derivation: 1 Hz period, 50% duty cycle, derived from
    // millis-since-boot so all pulsing pixels stay phase-locked (looks
    // intentional rather than glitchy).
    const uint32_t now_ms   = esp_log_timestamp();
    const bool     pulse_on = (now_ms % kPulsePeriodMs) < (kPulsePeriodMs / 2);

    // Reconcile WAN-bound flags against the failsafe timeout, and against
    // the underlying peripheral state.
    //
    // The bridge tells us when an upload is in flight. We never trust the
    // flag indefinitely: if no upload_end arrives within kWanBoundTimeoutMs,
    // we drop back. We also never let WanBound / Uploading outlive the
    // peripheral being on — a flag set while the mic is Off is a no-op,
    // and as soon as the corresponding base state is Off the flag clears.
    //
    // Old design (audio-service-derived Local <-> Stream reconcile from
    // IsAudioProcessorRunning) was removed: it conflated "audio service is
    // ticking" with "audio leaving the LAN", and the LED is supposed to
    // mean the latter. The bridge has the only reliable view.
    {
        if (_mic_wan_bound.load(std::memory_order_acquire)) {
            uint32_t ts = _mic_wan_bound_ts_ms.load(std::memory_order_acquire);
            if ((now_ms - ts) > kWanBoundTimeoutMs) {
                _mic_wan_bound.store(false, std::memory_order_release);
                mclog::tagWarn(_tag,
                    "mic WAN-bound flag timed out ({} ms with no upload_end)",
                    kWanBoundTimeoutMs);
            }
        }
        if (_camera_uploading.load(std::memory_order_acquire)) {
            uint32_t ts = _camera_uploading_ts_ms.load(std::memory_order_acquire);
            if ((now_ms - ts) > kWanBoundTimeoutMs) {
                _camera_uploading.store(false, std::memory_order_release);
                mclog::tagWarn(_tag,
                    "camera upload flag timed out ({} ms with no upload_end)",
                    kWanBoundTimeoutMs);
            }
        }
    }

    // Derive the displayed mic state from the base peripheral state plus
    // the WAN-bound flag. WanBound only ever applies on top of Local;
    // never elevate Off → WanBound (peripheral must actually be open).
    MicState mic_base = _mic_state.load(std::memory_order_acquire);
    MicState mic_disp = mic_base;
    if (mic_base == MicState::Local && _mic_wan_bound.load(std::memory_order_acquire)) {
        mic_disp = MicState::WanBound;
    }

    // Mic indicator at global index 6. Green when on; PULSING green when
    // audio is crossing the LAN (WanBound). Uses the friend-only writer
    // so the public Hal::setRgbColor guard (which rejects 6/11) doesn't
    // fire.
    switch (mic_disp) {
        case MicState::Off:
            hal.setRgbColor_privacy_only(kMicLedIndex, 0, 0, 0);
            break;
        case MicState::Local:
            hal.setRgbColor_privacy_only(kMicLedIndex, kMicR, kMicG, kMicB);
            break;
        case MicState::WanBound:
            // Same green hue, blink at 1 Hz so "data leaving" is a
            // distinct alarm without needing a second colour.
            hal.setRgbColor_privacy_only(kMicLedIndex,
                pulse_on ? kMicR : 0,
                pulse_on ? kMicG : 0,
                pulse_on ? kMicB : 0);
            break;
    }

    // Same derivation for the camera: Uploading only on top of Active.
    CameraState cam_base = _camera_state.load(std::memory_order_acquire);
    CameraState cam_disp = cam_base;
    if (cam_base == CameraState::Active && _camera_uploading.load(std::memory_order_acquire)) {
        cam_disp = CameraState::Uploading;
    }

    // Camera indicator at global index 11. Steady red when any consumer
    // is reading frames; PULSING red when frames are crossing the LAN.
    switch (cam_disp) {
        case CameraState::Off:
            hal.setRgbColor_privacy_only(kCameraLedIndex, 0, 0, 0);
            break;
        case CameraState::Active:
            hal.setRgbColor_privacy_only(kCameraLedIndex, kCameraR, kCameraG, kCameraB);
            break;
        case CameraState::Uploading:
            hal.setRgbColor_privacy_only(kCameraLedIndex,
                pulse_on ? kCameraR : 0,
                pulse_on ? kCameraG : 0,
                pulse_on ? kCameraB : 0);
            break;
    }

    hal.refreshRgb();
}

void PrivacyLeds::setMicWanBound(bool active)
{
    if (active) {
        _mic_wan_bound_ts_ms.store(esp_log_timestamp(), std::memory_order_release);
        _mic_wan_bound.store(true, std::memory_order_release);
    } else {
        _mic_wan_bound.store(false, std::memory_order_release);
    }
}

void PrivacyLeds::setCameraUploading(bool active)
{
    if (active) {
        _camera_uploading_ts_ms.store(esp_log_timestamp(), std::memory_order_release);
        _camera_uploading.store(true, std::memory_order_release);
    } else {
        _camera_uploading.store(false, std::memory_order_release);
    }
}

void PrivacyLeds::runBootSelfTest()
{
    mclog::tagInfo(_tag, "boot self-test: green (mic) -> red (camera) -> off");
    _inhibit_update.store(true, std::memory_order_release);

    auto& hal = GetHAL();
    struct Stage { const char* name; uint8_t r, g, b; };
    constexpr Stage stages[] = {
        {"green (mic)",    kMicR,    kMicG,    kMicB},
        {"red (camera)",   kCameraR, kCameraG, kCameraB},
        {"off",            0,        0,        0},
    };
    for (const auto& s : stages) {
        mclog::tagInfo(_tag, "  stage: {}", s.name);
        hal.setRgbColor_privacy_only(kMicLedIndex,    s.r, s.g, s.b);
        hal.setRgbColor_privacy_only(kCameraLedIndex, s.r, s.g, s.b);
        hal.refreshRgb();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    _inhibit_update.store(false, std::memory_order_release);
    mclog::tagInfo(_tag, "boot self-test done");
}

}  // namespace stackchan::privacy
