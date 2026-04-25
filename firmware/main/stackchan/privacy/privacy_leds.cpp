/*
 * SPDX-FileCopyrightText: 2026 Brett Kinny / squarewavesystems
 *
 * SPDX-License-Identifier: MIT
 */
#include "privacy_leds.h"

#include <hal/hal.h>
#include <application.h>

namespace stackchan::privacy {

PrivacyLeds& PrivacyLeds::getInstance()
{
    static PrivacyLeds instance;
    return instance;
}

void PrivacyLeds::update()
{
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

    // Mic indicator at global index 6.
    switch (_mic_state.load(std::memory_order_acquire)) {
        case MicState::Off:
            hal.setRgbColor(kMicLedIndex, 0, 0, 0);
            break;
        case MicState::Local:
            hal.setRgbColor(kMicLedIndex, kMicLocalR, kMicLocalG, kMicLocalB);
            break;
        case MicState::Stream:
            hal.setRgbColor(kMicLedIndex, kMicStreamR, kMicStreamG, kMicStreamB);
            break;
    }

    // Camera indicator at global index 7.
    switch (_camera_state.load(std::memory_order_acquire)) {
        case CameraState::Off:
            hal.setRgbColor(kCameraLedIndex, 0, 0, 0);
            break;
        case CameraState::Active:
            hal.setRgbColor(kCameraLedIndex, kCameraR, kCameraG, kCameraB);
            break;
    }

    hal.refreshRgb();
}

}  // namespace stackchan::privacy
