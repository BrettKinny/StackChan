/*
 * SPDX-FileCopyrightText: 2026 Brett Kinny / squarewavesystems
 *
 * SPDX-License-Identifier: MIT
 *
 * Layer 1 privacy indicator LEDs.
 *
 * Hardware-guaranteed mic / camera "is Dotty capturing right now?" indicator.
 *
 *   Right ring (global LED indices 6..11) is divided as:
 *     index 6  -> MIC privacy indicator
 *                  - off              -> MIC_OFF      (mic ADC closed)
 *                  - steady green     -> MIC_LOCAL    (mic ADC open, only fed
 *                                                      to local wake-word /
 *                                                      VAD; no WAN uplink)
 *                  - pulsing green    -> MIC_WAN_BOUND (mic ADC open AND audio
 *                                                       is crossing the LAN
 *                                                       boundary — i.e. opus
 *                                                       being uploaded to a
 *                                                       cloud ASR / LLM)
 *     index 11 -> CAMERA privacy indicator
 *                  - off              -> CAMERA_OFF      (no consumer of
 *                                                         camera frames)
 *                  - steady red       -> CAMERA_ACTIVE   (face-detect or
 *                                                         Capture() actively
 *                                                         reading frames
 *                                                         locally — no upload)
 *                  - pulsing red      -> CAMERA_UPLOADING (frames are being
 *                                                          shipped to a cloud
 *                                                          vision API)
 *
 *   Indices 7..10 are NOT touched by this module — they remain owned by the
 *   chat-state / face-detect ring animations.
 *
 * Why this design:
 *
 *   The MCP server can NOT call setMicState or setCameraState from
 *   user-space — the only hooks that flip these states live in the RAII
 *   guards (mic_peripheral_guard.h, camera_peripheral_guard.h) which are
 *   inserted on the same code path that physically opens/closes the
 *   peripheral. So a compromised server or a misbehaving LLM cannot turn
 *   capture on while the indicator is off. The indicator follows the
 *   peripheral, not the server.
 *
 *   The right-ring chat-state animations (e.g. face-detect cyan via
 *   rightNeonLight().setColor(...)) overwrite all 6 right-ring LEDs each
 *   frame the animation is running. To beat that, PrivacyLeds::update()
 *   re-asserts the privacy pixels every tick, AFTER the ring animations
 *   have run for that tick. The hybrid pattern follows what
 *   set_led_multi already documents.
 *
 * Failure modes:
 *
 *   - If the PY32 IO expander fails to init, setRgbColor is a no-op (see
 *     hal_io_expander.cpp). In that case the LED is dark even though the
 *     peripheral is alive. We log loudly on construction; there is no
 *     hardware fallback.
 *   - If a peripheral is enabled WITHOUT going through the guard (e.g.
 *     someone bypasses our wrappers in a future patch), the LED will
 *     stay off. This is a known-unsafe state and only a code-review
 *     defence; it can be tightened by making EnableInput private and
 *     forcing all callers through the guard.
 *
 * Public API is read-only intent + read-only diagnostics. Mutators are
 * declared `friend` of the guard classes, NOT exposed to MCP.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace stackchan::privacy {

// LED ring indices reserved for Layer 1 privacy indicators.
// Right ring spans global 6..11; we take 6 (top) and 11 (bottom).
// Indices 7..10 stay available to the existing chat-state ring
// animations.
// Privacy pixels at OPPOSITE ENDS of the right ring (was 6 + 7, both at
// the top — visually crowded). Now top + bottom, leaving indices 7-10 in
// the middle free for future indicators (cloud-connection, smart-mode
// pip, etc.).
constexpr uint8_t kMicLedIndex    = 6;   // global; top of right ring
constexpr uint8_t kCameraLedIndex = 11;  // global; bottom of right ring

// Universal recording-light convention. Mic = GREEN ("Dotty is listening"),
// Camera = RED ("Dotty is recording"). When the corresponding peripheral
// data is crossing the LAN boundary (mic audio uploading to cloud ASR;
// camera frames uploading to a vision API), the LED pulses at ~1 Hz
// instead of staying steady — same hue, different pattern, so "data is
// leaving the device" reads as a distinct alarm without needing a second
// colour. Pre-quantized to RGB565 (5/6/5).
//
// The two pulsing states (mic WanBound, camera Uploading) are driven by
// the bridge over the avatar WS protocol: it knows when an upload is in
// flight and we don't. The bridge sends `privacy.upload_start` /
// `upload_end` events and PrivacyLeds clears the flag automatically
// after kWanBoundTimeoutMs if no end arrives (defensive against bridge
// crash mid-upload — never want the LED stuck pulsing while no data is
// actually leaving).
//
// Distinct from existing UI palette: left-ring chat states use the same
// green during LISTENING — that's intentional reinforcement (both say
// "Dotty is listening"). Spatial separation (left vs right ring) keeps
// them readable.
constexpr uint8_t kMicR         = 0;    // green: mic on
constexpr uint8_t kMicG         = 200;
constexpr uint8_t kMicB         = 0;

constexpr uint8_t kCameraR      = 200;  // red: camera on
constexpr uint8_t kCameraG      = 0;
constexpr uint8_t kCameraB      = 0;

// 1 Hz pulse period. update() runs every ~10 ms; a 1000 ms period with
// 50% duty cycle means the LED is lit for ~500 ms, dark for ~500 ms.
constexpr uint32_t kPulsePeriodMs = 1000;

// Failsafe drop-back interval for the WAN-bound flag. If the bridge
// sends `upload_start` and never sends `upload_end` (crash, network
// drop, etc.), the LED will revert from pulsing to steady after this
// many ms. Two seconds is comfortably longer than any expected single-
// shot vision API call (median ~700 ms, p99 ~1.5 s) but short enough
// that a stuck-pulsing LED is never visibly wrong for long.
constexpr uint32_t kWanBoundTimeoutMs = 2000;

enum class MicState : uint8_t {
    Off      = 0,  // codec input device closed
    Local    = 1,  // ADC on, audio is staying on-device (wake-word, VAD,
                   // local-only ASR if/when we have one)
    WanBound = 2,  // ADC on AND opus frames are crossing the LAN boundary
                   // to a cloud ASR / LLM — pulses to alert the operator
};

enum class CameraState : uint8_t {
    Off       = 0,  // VIDIOC_STREAMON in driver but no consumer reading
                    // frames (see PRIVACY_LEDS.md for the L362 STREAMOFF
                    // discussion)
    Active    = 1,  // a consumer (face_detector StreamCaptures or Capture())
                    // is actively dequeuing frames locally
    Uploading = 2,  // frames (or a derivative — JPEG, embedding, etc.) are
                   // crossing the LAN boundary to a cloud vision API —
                   // pulses to alert the operator
};

class PrivacyLeds {
public:
    static PrivacyLeds& getInstance();

    PrivacyLeds(const PrivacyLeds&) = delete;
    PrivacyLeds& operator=(const PrivacyLeds&) = delete;

    // Read-only diagnostic accessors.
    MicState micState() const { return _mic_state.load(std::memory_order_acquire); }
    CameraState cameraState() const { return _camera_state.load(std::memory_order_acquire); }

    // Re-assert the two privacy pixels onto the right ring.
    // Called once per stackchan update tick AFTER the ring animations have
    // run, so we visibly override their colour at the privacy indices.
    // Cheap (two setRgbColor + one refreshRgb).
    void update();

    // Bridge-driven WAN-bound flag controls. The bridge tells us when an
    // upload is in flight (it knows; we don't have a per-byte hook into
    // either the audio service's WS payloads or the cloud vision HTTP
    // calls). These methods are SAFE to call from any context — they
    // never enable a privacy LED from Off; they only upgrade an
    // already-Local mic to WanBound (or already-Active camera to
    // Uploading). If the corresponding peripheral is currently Off, the
    // flag is recorded but has no visible effect, and is automatically
    // cleared the next time update() reconciles state.
    //
    // The flag is also auto-cleared kWanBoundTimeoutMs after the most
    // recent setMicWanBound(true) / setCameraUploading(true) call, even
    // if no setX(false) ever arrives — defensive against a bridge crash
    // mid-upload that would otherwise leave the LED pulsing forever.
    void setMicWanBound(bool active);
    void setCameraUploading(bool active);

    // Boot-time self-test: cycles BOTH privacy pixels through the full
    // palette (amber -> cyan-blue -> red -> off, ~500 ms each, ~2 s total)
    // so the operator can confirm at power-on that both LEDs and the I2C
    // bus to the PY32 are alive. Inhibits update() reconciliation for the
    // duration so chat-state animations cannot overpaint the test pattern.
    void runBootSelfTest();

private:
    PrivacyLeds() = default;

    // The state mutators are private. They are reachable only through the
    // friend guard classes, which are themselves wired into the peripheral-
    // enable codepaths. This is the load-bearing invariant: a compromised
    // server cannot call setMicState / setCameraState.
    void setMicState(MicState s) { _mic_state.store(s, std::memory_order_release); }
    void setCameraState(CameraState s) { _camera_state.store(s, std::memory_order_release); }

    friend class MicPeripheralGuard;
    friend class CameraPeripheralGuard;

    std::atomic<MicState>    _mic_state    {MicState::Off};
    std::atomic<CameraState> _camera_state {CameraState::Off};
    std::atomic<bool>        _inhibit_update {false};

    // WAN-bound flags + their failsafe timestamps. Set by the bridge over
    // the avatar WS protocol; consumed by update() to decide whether to
    // pulse the corresponding privacy LED.
    std::atomic<bool>     _mic_wan_bound       {false};
    std::atomic<uint32_t> _mic_wan_bound_ts_ms {0};
    std::atomic<bool>     _camera_uploading       {false};
    std::atomic<uint32_t> _camera_uploading_ts_ms {0};
};

}  // namespace stackchan::privacy
