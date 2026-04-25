/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include "core/color/color.hpp"
#include <string_view>

namespace stackchan::addon {

/**
 * @brief
 *
 */
class NeonLight {
public:
    NeonLight(int ledCount) : _led_count(ledCount)
    {
    }

    void init();
    void update();

    void setColor(uint8_t r, uint8_t g, uint8_t b);
    void setColor(const uitk::color::Rgb_t& rgb);
    void setColor(uint32_t hex);
    void setColor(std::string_view hex);
    void setDuration(float durationSec);

    /**
     * @brief Set a single LED in this ring directly, bypassing the color animation.
     *
     * Used for hybrid effects where one LED needs to hold a status colour while the
     * rest of the ring continues to animate (e.g. smart-mode indicator). Caller is
     * responsible for re-asserting the LED on each turn boundary if the next
     * animated setColor() would overwrite it.
     *
     * @param index 0..(led_count-1) within this ring (Left=0..5, Right=0..5).
     * @param r,g,b 0..255
     */
    void setColorAt(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
    int getLedCount() const
    {
        return _led_count;
    }

protected:
    virtual void set_rgb_color_impl(uint8_t index, uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void refresh_rgb_impl()                                                 = 0;

private:
    int _led_count;
    bool _is_inited              = false;
    bool _snap_to_target_on_rest = false;
    uint32_t _last_tick          = 0;
    uitk::color::AnimateRgb_t _color_anim;
};

/**
 * @brief
 *
 */
class LeftNeonLight : public NeonLight {
public:
    LeftNeonLight() : NeonLight(6)
    {
    }

private:
    void set_rgb_color_impl(uint8_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void refresh_rgb_impl() override;
};

/**
 * @brief
 *
 */
class RightNeonLight : public NeonLight {
public:
    RightNeonLight() : NeonLight(6)
    {
    }

private:
    void set_rgb_color_impl(uint8_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void refresh_rgb_impl() override;
};

}  // namespace stackchan::addon
