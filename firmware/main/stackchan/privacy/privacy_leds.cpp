/*
 * SPDX-FileCopyrightText: 2026 Brett Kinny / squarewavesystems
 *
 * SPDX-License-Identifier: MIT
 */
#include "privacy_leds.h"

#include <hal/hal.h>
#include <application.h>
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

    // Reconcile Local <-> Stream while the mic is on.
    //
    // The MicPeripheralGuard ctor sets the state to Local when the codec
    // input device opens. But the wake-word -> voice-processing transition
    // does NOT reopen the codec — it just flips the AS_EVENT_AUDIO_PROCESSOR
    // bit while the existing codec session continues. So while the guard is
    // alive (= mic ADC actually on), we re-derive Local vs Stream from the
    // audio service event group on every tick. This keeps the LED in sync
    // without needing a hook in the upstream xiaozhi audio_service.cc.
    //
    // CRITICAL: this only ever upgrades / downgrades between Local and
    // Stream. We never flip Off -> on here; that path is the guard ctor.
    {
        MicState s = _mic_state.load(std::memory_order_acquire);
        if (s != MicState::Off) {
            // The Application singleton is alive by the time anything calls
            // EnableInput (which is what creates the guard), so this is safe.
            bool streaming = Application::GetInstance().GetAudioService().IsAudioProcessorRunning();
            MicState desired = streaming ? MicState::Stream : MicState::Local;
            if (desired != s) {
                _mic_state.store(desired, std::memory_order_release);
            }
        }
    }

    // Mic indicator at global index 6. Green when on; PULSING green when
    // streaming opus frames to the server (your voice is leaving the
    // device). Uses the friend-only writer so the public Hal::setRgbColor
    // guard (which rejects 6/7) doesn't fire.
    //
    // Pulse derivation: 1 Hz period, 50% duty cycle, derived from
    // millis-since-boot so all pulsing pixels stay phase-locked (looks
    // intentional rather than glitchy).
    const uint32_t now_ms     = esp_log_timestamp();
    const bool     pulse_on   = (now_ms % kPulsePeriodMs) < (kPulsePeriodMs / 2);
    switch (_mic_state.load(std::memory_order_acquire)) {
        case MicState::Off:
            hal.setRgbColor_privacy_only(kMicLedIndex, 0, 0, 0);
            break;
        case MicState::Local:
            hal.setRgbColor_privacy_only(kMicLedIndex, kMicR, kMicG, kMicB);
            break;
        case MicState::Stream:
            // Same green hue, blink at 1 Hz so "data leaving" is a
            // distinct alarm without needing a second color.
            hal.setRgbColor_privacy_only(kMicLedIndex,
                pulse_on ? kMicR : 0,
                pulse_on ? kMicG : 0,
                pulse_on ? kMicB : 0);
            break;
    }

    // Camera indicator at global index 7. Steady red when any consumer
    // is reading frames. Pulsing-when-uploading is planned but requires
    // a firmware-side local-vs-upload distinction (deferred to step 4-5).
    switch (_camera_state.load(std::memory_order_acquire)) {
        case CameraState::Off:
            hal.setRgbColor_privacy_only(kCameraLedIndex, 0, 0, 0);
            break;
        case CameraState::Active:
            hal.setRgbColor_privacy_only(kCameraLedIndex, kCameraR, kCameraG, kCameraB);
            break;
    }

    hal.refreshRgb();
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
