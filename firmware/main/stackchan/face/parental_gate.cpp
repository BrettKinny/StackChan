/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "parental_gate.h"

#include <esp_log.h>
#include <hal/hal.h>

#define TAG "ParentalGate"

namespace stackchan {

std::atomic<uint32_t> ParentalGate::_unlocked_at_ms{0};
std::atomic<bool>     ParentalGate::_long_press_flag{false};

// Short-circuit constants. Defensive bounds so a malformed MCP arg can't
// cause us to compare against something silly. Real PIN is 4 digits today;
// keep the upper bound generous in case we move to passphrases.
static constexpr size_t kPinMinLen = 1;
static constexpr size_t kPinMaxLen = 32;

static uint32_t now_ms()
{
    return GetHAL().millis();
}

bool ParentalGate::tryUnlockByPIN(const std::string& pin)
{
    if (pin.size() < kPinMinLen || pin.size() > kPinMaxLen) {
        ESP_LOGW(TAG, "PIN unlock rejected: length out of range (%u)", (unsigned)pin.size());
        return false;
    }
    // NB: not a constant-time compare. Threat model is "kid in the room",
    // not network-side timing attacks; the PIN is also a dev placeholder
    // (see header). Move to mbedtls_constant_time_memcmp + hashed PIN
    // when wiring real credentials.
    if (pin != kEnrollPIN) {
        ESP_LOGW(TAG, "PIN unlock rejected");
        return false;
    }
    _unlocked_at_ms.store(now_ms(), std::memory_order_release);
    ESP_LOGI(TAG, "Parental gate unlocked via PIN (window %u ms)", (unsigned)kUnlockWindowMs);
    return true;
}

bool ParentalGate::tryUnlockByLongPress()
{
    bool expected = true;
    // Atomically consume the long-press flag. If it wasn't set, refuse.
    if (!_long_press_flag.compare_exchange_strong(expected, false,
                                                  std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "Long-press unlock rejected: flag not armed");
        return false;
    }
    _unlocked_at_ms.store(now_ms(), std::memory_order_release);
    ESP_LOGI(TAG, "Parental gate unlocked via long-press (window %u ms)",
             (unsigned)kUnlockWindowMs);
    return true;
}

void ParentalGate::setLongPressFlag(bool armed)
{
    _long_press_flag.store(armed, std::memory_order_release);
    ESP_LOGI(TAG, "Long-press flag %s", armed ? "armed" : "cleared");
}

bool ParentalGate::isUnlocked()
{
    uint32_t t = _unlocked_at_ms.load(std::memory_order_acquire);
    if (t == 0) return false;
    uint32_t now = now_ms();
    // Guard against monotonic-ms wrap (ESP-IDF GetHAL().millis() returns
    // uint32_t — wraps every ~49 days). If now < t, treat as expired so
    // a wrap can't accidentally extend a stale unlock indefinitely.
    if (now < t) {
        _unlocked_at_ms.store(0, std::memory_order_release);
        return false;
    }
    if ((now - t) > kUnlockWindowMs) {
        _unlocked_at_ms.store(0, std::memory_order_release);
        return false;
    }
    return true;
}

void ParentalGate::consume()
{
    _unlocked_at_ms.store(0, std::memory_order_release);
    ESP_LOGI(TAG, "Parental gate consumed");
}

}  // namespace stackchan
