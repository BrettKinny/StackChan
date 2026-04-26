/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "drivers/PY32IOExpander_Class/PY32IOExpander_Class.hpp"
#include <mooncake_log.h>
#include <esp_timer.h>
#include <memory>
#include <stackchan/privacy/privacy_leds.h>  // kMicLedIndex / kCameraLedIndex constants

static const std::string_view _tag = "HAL-IOE";

static std::unique_ptr<m5::PY32IOExpander_Class> _io_expander;

void Hal::io_expander_init()
{
    mclog::tagInfo(_tag, "init");

    auto i2c_bus        = hal_bridge::board_get_i2c_bus();
    _io_expander        = std::make_unique<m5::PY32IOExpander_Class>(i2c_bus);
    uint32_t start_tick = GetHAL().millis();

    // PY32 IO Expander may boot slowly, wait for it
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (GetHAL().millis() - start_tick > 1200) {
            mclog::tagError(_tag, "init timeout");
            _io_expander.reset();
            break;
        }

        if (_io_expander->begin()) {
            break;
        }
        mclog::tagInfo(_tag, "init failed, retrying...");
    }

    if (_io_expander) {
        // VM EN
        _io_expander->setDirection(0, true);  // Output
        _io_expander->setPullMode(0, true);   // Pull-up
        GetHAL().setServoPowerEnabled(true);
        vTaskDelay(pdMS_TO_TICKS(200));

        // RGB
        _io_expander->setDirection(13, true);   // Output
        _io_expander->setPullMode(13, true);    // Pull-up
        _io_expander->setDriveMode(13, false);  // Push-pull
        _io_expander->setLedCount(12);
        vTaskDelay(pdMS_TO_TICKS(200));
        GetHAL().showRgbColor(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        GetHAL().showRgbColor(0, 0, 0);

        mclog::tagInfo(_tag, "init done");
    }
}

void Hal::setServoPowerEnabled(bool enabled)
{
    if (!_io_expander) {
        return;
    }
    _io_expander->digitalWrite(0, enabled ? true : false);
}

void Hal::setRgbColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    // Indices 6 and 7 are reserved for the Layer-1 privacy indicators
    // (mic / camera). Only PrivacyLeds may write them, via the
    // setRgbColor_privacy_only() friend method below. Any other caller
    // (MCP tool, neon ring, chat-state animation) is rejected here so
    // the guarantee does not depend on every individual call site
    // remembering to skip 6/7.
    if (index == stackchan::privacy::kMicLedIndex ||
        index == stackchan::privacy::kCameraLedIndex) {
        mclog::tagWarn(_tag, "setRgbColor: index {} is privacy-reserved; use PrivacyLeds API", index);
        return;
    }
    if (!_io_expander) {
        return;
    }
    _io_expander->setLedColor(index, r, g, b);
}

void Hal::setRgbColor_privacy_only(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_io_expander) {
        return;
    }
    _io_expander->setLedColor(index, r, g, b);
}

void Hal::refreshRgb()
{
    if (!_io_expander) {
        return;
    }
    _io_expander->refreshLeds();
}

void Hal::showRgbColor(uint8_t r, uint8_t g, uint8_t b)
{
    // Skip the privacy-reserved indices. PrivacyLeds owns those.
    for (int i = 0; i < 12; i++) {
        if (i == stackchan::privacy::kMicLedIndex) continue;
        if (i == stackchan::privacy::kCameraLedIndex) continue;
        setRgbColor(i, r, g, b);
    }
    refreshRgb();
}

static esp_timer_handle_t _camera_led_timer = nullptr;

static void camera_led_timer_cb(void* arg)
{
    Hal* hal = static_cast<Hal*>(arg);
    hal->setCameraLedActive(false);
}

void Hal::setCameraLedActive(bool active, uint32_t duration_ms)
{
    // Privacy LED step 2 + position spread: animate the right-ring pixels
    // that ARE NOT privacy-reserved (currently indices 7..10, with mic at
    // 6 and camera at 11 owned by PrivacyLeds). Pre-fix this looped 6..11
    // and stomped both privacy pixels — fixed in step 2. Privacy pixels
    // are skipped via the public setRgbColor guard, so the loop body's
    // writes to 6 and 11 are no-ops with a warning log; the explicit
    // bounds here just avoid the warning noise.
    for (int i = stackchan::privacy::kMicLedIndex + 1;
         i < stackchan::privacy::kCameraLedIndex; i++) {
        setRgbColor(i, active ? 50 : 0, 0, 0);
    }
    refreshRgb();

    if (_camera_led_timer == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback              = camera_led_timer_cb,
            .arg                   = this,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "camera_led_timer",
            .skip_unhandled_events = false,
        };
        esp_timer_create(&timer_args, &_camera_led_timer);
    }

    esp_timer_stop(_camera_led_timer);

    if (active && duration_ms > 0) {
        esp_timer_start_once(_camera_led_timer, (uint64_t)duration_ms * 1000);
    }
}
