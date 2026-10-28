/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "drivers/Si12T/Si12T.h"
#include "board/hal_bridge.h"
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <inttypes.h>

static const std::string_view _tag = "HAL-HeadTouch";

// HP-PROBE: diagnostic-only episode logger used to collect pat vs idle-noise
// data before retuning the press rule. Observes samples; never changes the
// gestures. Off in release builds; build with -DHEAD_TOUCH_PROBE=1 to collect
// more data.
#ifndef HEAD_TOUCH_PROBE
#define HEAD_TOUCH_PROBE 0
#endif

// 触摸状态
enum class TouchState { IDLE, TOUCHED, SWIPING };

// 配置参数
struct TouchConfig {
    // MID for 2 samples (100 ms). HIGH x3 (2026-04-28, after MID noise fires
    // at i=[1,2,2] and i=[0,2,1]) missed most light pats: on the 2026-10-20
    // bench, natural fingertip pats lasted 100-210 ms and HIGH x3 caught 4/10,
    // MID x2 catches 8/10. The same bench saw no readings at any level in
    // 18 min idle. Build with HEAD_TOUCH_PROBE=1 to log HP-PROBE episodes again.
    uint8_t touch_threshold = 2;
    int16_t swipe_threshold = 40;  // 使用百分比，范围-100到100
    // Consecutive 50 ms samples of `is_touched()` required before we accept
    // the IDLE→TOUCHED transition. Filters single-sample capacitive blips
    // (RF/EMI/proximity drift) without missing short pats.
    uint8_t debounce_samples = 2;
};

// 触摸数据
struct TouchData {
    uint8_t intensity[3];
    uint32_t timestamp;

    // 计算位置（返回-100到100的整数）
    int16_t get_position() const
    {
        uint16_t total = intensity[0] + intensity[1] + intensity[2];
        if (total == 0) return 0;

        int32_t weighted = intensity[0] * (-100) + intensity[1] * 0 + intensity[2] * 100;
        return static_cast<int16_t>(weighted / total);
    }

    uint8_t get_max_intensity() const
    {
        uint8_t max_val = intensity[0];
        if (intensity[1] > max_val) max_val = intensity[1];
        if (intensity[2] > max_val) max_val = intensity[2];
        return max_val;
    }

    bool is_touched(uint8_t threshold) const
    {
        return get_max_intensity() >= threshold;
    }
};

// 手势识别器类
class GestureRecognizer {
public:
    GestureRecognizer() : current_state(TouchState::IDLE), initial_position(0)
    {
    }

    // 更新状态机，返回识别到的手势
    HeadPetGesture update(const TouchData& data)
    {
        HeadPetGesture gesture = HeadPetGesture::None;
        const bool touched = data.is_touched(config.touch_threshold);

        switch (current_state) {
            case TouchState::IDLE:
                if (touched) {
                    if (touched_samples < config.debounce_samples) {
                        ++touched_samples;
                    }
                    if (touched_samples >= config.debounce_samples) {
                        current_state    = TouchState::TOUCHED;
                        initial_position = data.get_position();
                        gesture          = HeadPetGesture::Press;
                        // mclog::tagInfo(_tag, "Touch detected at position: {}", initial_position);
                    }
                } else {
                    touched_samples = 0;
                }
                break;

            case TouchState::TOUCHED:
                if (!touched) {
                    current_state   = TouchState::IDLE;
                    touched_samples = 0;
                    gesture         = HeadPetGesture::Release;
                } else {
                    // Check for swipe
                    int16_t current_pos = data.get_position();
                    int16_t delta       = current_pos - initial_position;

                    if (delta > config.swipe_threshold) {
                        current_state = TouchState::SWIPING;
                        gesture       = HeadPetGesture::SwipeForward;
                        // mclog::tagInfo(_tag, "Swipe forward detected, delta: {}", delta);
                    } else if (delta < -config.swipe_threshold) {
                        current_state = TouchState::SWIPING;
                        gesture       = HeadPetGesture::SwipeBackward;
                        // mclog::tagInfo(_tag, "Swipe backward detected, delta: {}", delta);
                    }
                }
                break;

            case TouchState::SWIPING:
                if (!touched) {
                    current_state   = TouchState::IDLE;
                    touched_samples = 0;
                    gesture         = HeadPetGesture::Release;
                }
                break;
        }

        return gesture;
    }

    void set_config(const TouchConfig& cfg)
    {
        config = cfg;
    }

private:
    TouchConfig config;
    TouchState current_state;
    int16_t initial_position;
    uint8_t touched_samples = 0;
};

#if HEAD_TOUCH_PROBE
// One episode = first sample with any pad >= 1 (LOW) until all pads are 0.
// One "ep" line per episode (rate-limited) plus a "60s" summary each minute.
class TouchProbe {
public:
    void sample(const TouchData& d, HeadPetGesture g)
    {
        const uint32_t now = d.timestamp;
        if (!win_started) {
            win_started = true;
            win_start   = now;
            rl_start    = now;
        }

        uint8_t pads_mid = 0;
        uint8_t top      = 0;
        for (int p = 0; p < 3; ++p) {
            const uint8_t lv = d.intensity[p] & 0x03;  // 2-bit field, 0..3
            if (lv > top) top = lv;
            if (lv >= 2) ++pads_mid;
        }

        if (g == HeadPetGesture::Press) ++win_presses;

        if (top > 0) {
            if (!active) begin(now);
            ++n;
            for (int p = 0; p < 3; ++p) {
                const uint8_t lv = d.intensity[p] & 0x03;
                if (lv > mx) mx = lv;
                if (lv > pmx[p]) pmx[p] = lv;
                if (lv) ++cnt[lv - 1][p];
            }
            bump(run2, best2, pads_mid >= 1);
            bump(run2x2, best2x2, pads_mid >= 2);
            bump(run3, best3, top >= 3);
            if (g == HeadPetGesture::Press && presses < 255) ++presses;
            // Cap at 30 s so a pad stuck above 0 still produces output.
            if (n >= kMaxSamples) finish(now, true);
        } else if (active) {
            finish(now, false);
        }

        if (now - win_start >= pdMS_TO_TICKS(60000)) {
            ESP_LOGI(kTag, "60s ep=%u press=%u mid=%u high=%u drop=%u", (unsigned)win_eps,
                     (unsigned)win_presses, (unsigned)win_mid, (unsigned)win_high, (unsigned)win_drop);
            win_start = now;
            win_eps = win_presses = win_mid = win_high = win_drop = 0;
        }
    }

private:
    static constexpr const char* kTag      = "HP-PROBE";
    static constexpr uint16_t kMaxSamples  = 600;  // 30 s at 50 ms
    static constexpr uint8_t kMaxLinesPerS = 5;

    static void bump(uint16_t& run, uint16_t& best, bool hit)
    {
        run = hit ? run + 1 : 0;
        if (run > best) best = run;
    }

    void begin(uint32_t now)
    {
        active = true;
        start  = now;
        n = run2 = best2 = run2x2 = best2x2 = run3 = best3 = 0;
        mx = presses = 0;
        for (int p = 0; p < 3; ++p) {
            pmx[p] = 0;
            for (int l = 0; l < 3; ++l) cnt[l][p] = 0;
        }
    }

    void finish(uint32_t now, bool truncated)
    {
        active = false;
        ++win_eps;
        if (mx >= 2) ++win_mid;
        if (mx >= 3) ++win_high;

        if (now - rl_start >= pdMS_TO_TICKS(1000)) {
            rl_start = now;
            rl_lines = 0;
        }
        if (rl_lines >= kMaxLinesPerS) {
            ++pend_drop;
            ++win_drop;
            return;
        }
        ++rl_lines;

        const uint32_t ms = (uint32_t)(now - start) * (uint32_t)portTICK_PERIOD_MS;
        ESP_LOGI(kTag,
                 "ep ms=%" PRIu32 " n=%u max=%u,%u,%u lo=%u,%u,%u mid=%u,%u,%u hi=%u,%u,%u"
                 " run2=%u run2x2=%u run3=%u press=%u%s drop=%u",
                 ms, (unsigned)n, (unsigned)pmx[0], (unsigned)pmx[1], (unsigned)pmx[2], (unsigned)cnt[0][0],
                 (unsigned)cnt[0][1], (unsigned)cnt[0][2], (unsigned)cnt[1][0], (unsigned)cnt[1][1],
                 (unsigned)cnt[1][2], (unsigned)cnt[2][0], (unsigned)cnt[2][1], (unsigned)cnt[2][2],
                 (unsigned)best2, (unsigned)best2x2, (unsigned)best3, (unsigned)presses, truncated ? " cont" : "",
                 (unsigned)pend_drop);
        pend_drop = 0;
    }

    // Current episode
    bool active        = false;
    uint32_t start     = 0;
    uint16_t n         = 0;
    uint8_t mx         = 0;          // max level over all pads
    uint8_t pmx[3]     = {0, 0, 0};  // max level per pad
    uint16_t cnt[3][3] = {};         // samples at [level-1][pad]
    uint16_t run2 = 0, best2 = 0, run2x2 = 0, best2x2 = 0, run3 = 0, best3 = 0;
    uint8_t presses = 0;

    // Rate limit + 60 s window
    bool win_started   = false;
    uint32_t win_start = 0, rl_start = 0;
    uint8_t rl_lines   = 0;
    uint16_t pend_drop = 0;
    uint16_t win_eps = 0, win_presses = 0, win_mid = 0, win_high = 0, win_drop = 0;
};
#endif

static void _head_touch_update_task(void* param)
{
    mclog::tagInfo(_tag, "start update task");

    si12t_handle_t si12t = (si12t_handle_t)param;
    uint8_t touch_result = 0;
    TouchData data;

    GestureRecognizer recognizer;
    HeadPetGesture gesture;
#if HEAD_TOUCH_PROBE
    TouchProbe probe;
#endif

    vTaskDelay(pdMS_TO_TICKS(200));

    while (1) {
        // Read data. A failed I2C read counts as no touch, so it can never
        // fake or stretch a pet (it used to reuse the last reading).
        if (si12t_read_touch_result(si12t, &touch_result) != ESP_OK) {
            touch_result = 0;
        }
        si12t_parse_touch_result_to(touch_result, data.intensity);
        data.timestamp = xTaskGetTickCount();

        // Update and fire event
        gesture = recognizer.update(data);
#if HEAD_TOUCH_PROBE
        probe.sample(data, gesture);
#endif
        if (gesture != HeadPetGesture::None) {
            GetHAL().onHeadPetGesture.emit(gesture);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void Hal::head_touch_init()
{
    mclog::tagInfo(_tag, "init");

    auto i2c_bus = hal_bridge::board_get_i2c_bus();

    si12t_config_t si12t_cfg = {
        .i2c_bus  = i2c_bus,
        .dev_addr = SI12T_GND_ADDRESS,
    };
    static si12t_handle_t si12t;
    si12t_init(&si12t_cfg, &si12t);
    si12t_setup(si12t, SI12T_TYPE_LOW, SI12T_SENSITIVITY_LEVEL_3);

    xTaskCreateWithCaps(_head_touch_update_task, "headtouch", 1024 * 6, si12t, 5, NULL, MALLOC_CAP_SPIRAM);
}
