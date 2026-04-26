/*
 * SPDX-FileCopyrightText: 2026 Brett Kinny / squarewavesystems
 *
 * SPDX-License-Identifier: MIT
 */
#include "privacy_leds.h"

#include <hal/hal.h>
#include <application.h>
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

    // Mic indicator at global index 6. Uses the friend-only writer so the
    // public Hal::setRgbColor guard (which rejects 6/7) doesn't fire.
    switch (_mic_state.load(std::memory_order_acquire)) {
        case MicState::Off:
            hal.setRgbColor_privacy_only(kMicLedIndex, 0, 0, 0);
            break;
        case MicState::Local:
            hal.setRgbColor_privacy_only(kMicLedIndex, kMicLocalR, kMicLocalG, kMicLocalB);
            break;
        case MicState::Stream:
            hal.setRgbColor_privacy_only(kMicLedIndex, kMicStreamR, kMicStreamG, kMicStreamB);
            break;
    }

    // Camera indicator at global index 7.
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
    mclog::tagInfo(_tag, "boot self-test: amber -> cyan-blue -> red -> off");
    _inhibit_update.store(true, std::memory_order_release);

    auto& hal = GetHAL();
    struct Stage { const char* name; uint8_t r, g, b; };
    constexpr Stage stages[] = {
        {"amber",     kMicLocalR,  kMicLocalG,  kMicLocalB},
        {"cyan-blue", kMicStreamR, kMicStreamG, kMicStreamB},
        {"red",       kCameraR,    kCameraG,    kCameraB},
        {"off",       0,           0,           0},
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
