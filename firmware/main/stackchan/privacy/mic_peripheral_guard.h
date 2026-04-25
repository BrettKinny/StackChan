/*
 * SPDX-FileCopyrightText: 2026 Brett Kinny / squarewavesystems
 *
 * SPDX-License-Identifier: MIT
 *
 * RAII guard for the microphone peripheral.
 *
 *   Construct at the moment the codec input device is opened (mic ADC
 *   enabled) — the guard sets PrivacyLeds::MicState to whatever the
 *   caller declares (Local for wake-word/VAD-only, Stream for active
 *   uplink to xiaozhi).
 *
 *   Destruct when the codec input device is closed (mic ADC disabled);
 *   the guard restores MicState to Off.
 *
 * Insert at every code path that calls esp_codec_dev_open / close on the
 * input device. Currently this is CoreS3AudioCodec::EnableInput in
 * cores3_audio_codec.cc; tag with the appropriate mic mode.
 *
 * The guard is intentionally non-copyable / non-movable. It is OWNED by
 * the peripheral driver and lives for the duration of the open() call.
 */
#pragma once

#include "privacy_leds.h"

namespace stackchan::privacy {

class MicPeripheralGuard {
public:
    explicit MicPeripheralGuard(MicState s) : _state(s)
    {
        // Always escalate from Off to the requested state. We do NOT try to
        // be clever and skip the LED if mic was already on — the very point
        // of this guard is that the LED tracks the peripheral lifecycle.
        PrivacyLeds::getInstance().setMicState(s);
    }

    ~MicPeripheralGuard()
    {
        PrivacyLeds::getInstance().setMicState(MicState::Off);
    }

    // If we want to upgrade Local->Stream mid-flight without closing/
    // reopening the ADC (e.g. wake word fired and we now stream the same
    // already-open input device), we expose a controlled transition rather
    // than letting random code call setMicState directly.
    void upgrade(MicState s)
    {
        _state = s;
        PrivacyLeds::getInstance().setMicState(s);
    }

    MicState state() const { return _state; }

    MicPeripheralGuard(const MicPeripheralGuard&)            = delete;
    MicPeripheralGuard& operator=(const MicPeripheralGuard&) = delete;
    MicPeripheralGuard(MicPeripheralGuard&&)                 = delete;
    MicPeripheralGuard& operator=(MicPeripheralGuard&&)      = delete;

private:
    MicState _state;
};

}  // namespace stackchan::privacy
