/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "stackchan_display.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <vector>
#include <cstring>
#include <src/misc/cache/lv_cache.h>
#include <settings.h>
#include <lvgl.h>
#include <lvgl_theme.h>
#include <stackchan/stackchan.h>
#include <stackchan/face/face_detector.h>
#include <stackchan/sound_localizer.h>
#include <stackchan/avatar/decorators/decorators.h>
#include "application.h"
#include <assets/lang_config.h>
#include <hal/hal.h>

using namespace stackchan;
using namespace stackchan::avatar;

#define TAG "StackChanAvatarDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

// Have to register themes, so the asset apply can update the text font
void StackChanAvatarDisplay::InitializeLcdThemes()
{
    auto text_font       = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font       = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));        // rgb(255, 255, 255)
    light_theme->set_text_color(lv_color_hex(0x000000));              // rgb(0, 0, 0)
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));   // rgb(224, 224, 224)
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));       // rgb(0, 128, 0)
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));  // rgb(221, 221, 221)
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));     // rgb(255, 255, 255)
    light_theme->set_system_text_color(lv_color_hex(0x000000));       // rgb(0, 0, 0)
    light_theme->set_border_color(lv_color_hex(0x000000));            // rgb(0, 0, 0)
    light_theme->set_low_battery_color(lv_color_hex(0x000000));       // rgb(0, 0, 0)
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));        // rgb(0, 0, 0)
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));              // rgb(255, 255, 255)
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));   // rgb(31, 31, 31)
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));       // rgb(0, 128, 0)
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));  // rgb(34, 34, 34)
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));     // rgb(0, 0, 0)
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));       // rgb(255, 255, 255)
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));            // rgb(255, 255, 255)
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));       // rgb(255, 0, 0)
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

StackChanAvatarDisplay::StackChanAvatarDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                                               bool mirror_y, bool swap_xy)
    : LvglDisplay(), panel_io_(panel_io), panel_(panel)
{
    width_  = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_         = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Draw white screen
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 20;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle      = panel_io_,
        .panel_handle   = panel_,
        .control_handle = nullptr,
        .buffer_size    = static_cast<uint32_t>(width_ * 20),
        .double_buffer  = false,
        .trans_size     = 0,
        .hres           = static_cast<uint32_t>(width_),
        .vres           = static_cast<uint32_t>(height_),
        .monochrome     = false,
        .rotation =
            {
                .swap_xy  = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma     = 1,
                .buff_spiram  = 0,
                .sw_rotate    = 0,
                .swap_bytes   = 1,
                .full_refresh = 0,
                .direct_mode  = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                StackChanAvatarDisplay* display = static_cast<StackChanAvatarDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg                   = this,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);

    // Create boot logo label if not warm boot
    if (GetHAL().getWarmRebootTarget() < 0) {
        ESP_LOGI(TAG, "Create boot logo label");
        Lock();
        {
            uitk::lvgl_cpp::ScreenActive screen;
            screen.setBgColor(lv_color_hex(0x000000));
        }
        GetHAL().bootLogo = std::make_unique<BootLogo>();
        Unlock();
    }

    // Bubble auto-clear timer (fires after speaking ends)
    esp_timer_create_args_t bubble_timer_args = {
        .callback = [](void* arg) {
            auto* display = static_cast<StackChanAvatarDisplay*>(arg);
            display->ClearChatMessages();
        },
        .arg                   = this,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "bubble_clear_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&bubble_timer_args, &bubble_clear_timer_);

    // Thinking timer: fires after 1.5s in listening state to show thinking animation
    esp_timer_create_args_t thinking_timer_args = {
        .callback = [](void* arg) {
            auto* display = static_cast<StackChanAvatarDisplay*>(arg);
            if (display->in_listening_status_) {
                display->SetEmotion("thinking");
            }
        },
        .arg                   = this,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "thinking_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&thinking_timer_args, &thinking_timer_);

    // Robot will be created later in SetupXiaoZhiUI()
}

StackChanAvatarDisplay::~StackChanAvatarDisplay()
{
    ESP_LOGI(TAG, "Destroying StackChanAvatarDisplay");

    if (thinking_timer_ != nullptr) {
        esp_timer_stop(thinking_timer_);
        esp_timer_delete(thinking_timer_);
    }

    if (bubble_clear_timer_ != nullptr) {
        esp_timer_stop(bubble_clear_timer_);
        esp_timer_delete(bubble_clear_timer_);
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }

    auto& stackchan = GetStackChan();
    if (stackchan.hasAvatar()) {
        stackchan.resetAvatar();
    }
}

bool StackChanAvatarDisplay::Lock(int timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void StackChanAvatarDisplay::Unlock()
{
    lvgl_port_unlock();
}

lv_disp_t* StackChanAvatarDisplay::GetLvglDisplay()
{
    return display_;
}

#include <hal/board/hal_bridge.h>

void StackChanAvatarDisplay::SetupUI()
{
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called

    auto& stackchan = GetStackChan();

    if (stackchan.hasAvatar()) {
        ESP_LOGW(TAG, "Avatar already created");
        return;
    }

    DisplayLockGuard lock(this);

    ESP_LOGI(TAG, "Creating Stack-chan Avatar...");

    auto avatar = std::make_unique<DefaultAvatar>();
    avatar->init(lv_screen_active());
    avatar->getPanel()->onClick().connect([]() {
        if (hal_bridge::is_xiaozhi_ready()) {
            hal_bridge::toggle_xiaozhi_chat_state();
        }
    });

    stackchan.attachAvatar(std::move(avatar));
    stackchan.addModifier(std::make_unique<BreathModifier>());
    blink_modifier_id_ = stackchan.addModifier(std::make_unique<BlinkModifier>());
    stackchan.addModifier(std::make_unique<HeadPetModifier>());
    stackchan.addModifier(std::make_unique<ImuEventModifier>());

    preview_image_ = lv_image_create(lv_screen_active());
    lv_obj_set_size(preview_image_, 320, 240);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    // GetHAL().startStackChanAutoUpdate(24);

    FaceDetector::getInstance().start();

    ESP_LOGI(TAG, "Avatar created and started");
}

void StackChanAvatarDisplay::LvglLock()
{
    if (!Lock(30000)) {
        ESP_LOGE("Display", "Failed to lock display");
    }
}

void StackChanAvatarDisplay::LvglUnlock()
{
    Unlock();
}

static void set_left_leds(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 6; i++) {
        GetHAL().setRgbColor(i, r, g, b);
    }
    GetHAL().refreshRgb();
}

void StackChanAvatarDisplay::SetEmotion(const char* emotion)
{
    auto& stackchan = GetStackChan();

    if (!stackchan.hasAvatar() || !emotion) {
        return;
    }

    DisplayLockGuard lock(this);

    ESP_LOGI(TAG, "SetEmotion: %s", emotion);

    auto& avatar = stackchan.avatar();

    // Map emotion string to stackchan::Emotion
    if (strcmp(emotion, "neutral") == 0) {
        avatar.setEmotion(Emotion::Neutral);
    } else if (strcmp(emotion, "happy") == 0) {
        avatar.setEmotion(Emotion::Happy);
    } else if (strcmp(emotion, "laughing") == 0) {
        avatar.setEmotion(Emotion::Happy);
    } else if (strcmp(emotion, "angry") == 0) {
        avatar.setEmotion(Emotion::Angry);
    } else if (strcmp(emotion, "sad") == 0) {
        avatar.setEmotion(Emotion::Sad);
    } else if (strcmp(emotion, "crying") == 0) {
        avatar.setEmotion(Emotion::Sad);
    } else if (strcmp(emotion, "sleepy") == 0) {
        avatar.setEmotion(Emotion::Sleepy);
        avatar.setSpeech("Zzz…");
        is_sleeping_ = true;
        // avatar.mouth().setWeight(10);

        // Stop face tracking
        FaceDetector::getInstance().setEnabled(false);
        if (face_tracking_modifier_id_ >= 0) {
            stackchan.removeModifier(face_tracking_modifier_id_);
            face_tracking_modifier_id_ = -1;
        }
        stackchan.rightNeonLight().setColor(0, 0, 0);

        // Stop idle motion
        ESP_LOGW(TAG, "Stop idle motion");
        if (idle_motion_modifier_id_ >= 0) {
            stackchan.removeModifier(idle_motion_modifier_id_);
            idle_motion_modifier_id_ = -1;
            stackchan.removeModifier(idle_expression_modifier_id_);
            idle_expression_modifier_id_ = -1;
        }

        // Return to default pose
        auto& motion = GetStackChan().motion();
        motion.pitchServo().moveWithSpeed(0, 80);

    } else if (strcmp(emotion, "thinking") == 0) {
        if (speaking_modifier_id_ >= 0) {
            stackchan.removeModifier(speaking_modifier_id_);
            avatar.mouth().setWeight(0);
            speaking_modifier_id_ = -1;
        }

        avatar.setEmotion(Emotion::Doubt);

        if (thinking_modifier_id_ < 0) {
            thinking_modifier_id_ = stackchan.addModifier(std::make_unique<ThinkingModifier>());
        }

        if (in_listening_status_) {
            thinking_led_pending_ = true;
            set_left_leds(50, 25, 0);
        }
    } else if (strcmp(emotion, "doubtful") == 0) {
        avatar.setEmotion(Emotion::Doubt);
    } else if (strcmp(emotion, "surprised") == 0) {
        avatar.setEmotion(Emotion::Surprise);
    } else if (strcmp(emotion, "loving") == 0) {
        avatar.setEmotion(Emotion::Love);
        // Eye delta for Love is subtle (soft squint); the recognisable hearts
        // are this decorator overlay. Same pattern as the touch-pet flow in
        // head_pet.h. 4 s lifetime, 500 ms heart-spawn cadence.
        avatar.removeDecorator(love_decorator_id_);
        love_decorator_id_ = avatar.addDecorator(
            std::make_unique<HeartDecorator>(lv_screen_active(), 4000, 500));
    } else {
        // Brief magenta pip on the left ring is a visible signal that an
        // unrecognised emotion arrived — otherwise this branch is silent and
        // future emoji additions can regress invisibly. The LED gets
        // overwritten by the next state-change; the warning log persists.
        ESP_LOGW(TAG, "Unknown emotion: %s, using NEUTRAL", emotion);
        set_left_leds(40, 0, 40);
        avatar.setEmotion(Emotion::Neutral);
    }

    // Resync blink modifier base eye weights
    auto blink_modifier = static_cast<BlinkModifier*>(stackchan.getModifier(blink_modifier_id_));
    if (blink_modifier) {
        blink_modifier->resyncEyeWeights();
    }
}

void StackChanAvatarDisplay::SetChatMessage(const char* role, const char* content)
{
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }

    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        return;
    }

    // ESP_LOGE(TAG, "SetChatMessage: role=%s, content=%s", role ? role : "null", content ? content : "null");

    DisplayLockGuard lock(this);

    if (strcmp(role, "system") == 0) {
        stackchan.avatar().setSpeech(content);
    } else if (strcmp(role, "assistant") == 0) {
        stackchan.avatar().setSpeech(content);
    }
}

void StackChanAvatarDisplay::ClearChatMessages()
{
    if (bubble_clear_timer_) {
        esp_timer_stop(bubble_clear_timer_);
    }

    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        return;
    }

    DisplayLockGuard lock(this);

    stackchan.avatar().clearSpeech();

    ESP_LOGI(TAG, "Chat messages cleared");
}

void StackChanAvatarDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image)
{
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc          = preview_image_cached_->image_dsc();
    // Set image source and show preview image
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // Scale to fit width
        lv_image_set_scale(preview_image_, 256 * width_ / img_dsc->header.w);
    }

    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(preview_image_);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, 6000 * 1000));
}

void StackChanAvatarDisplay::UpdateStatusBar(bool update_all)
{
}

void StackChanAvatarDisplay::SetTheme(Theme* theme)
{
    ESP_LOGI(TAG, "SetTheme: %s", theme->name().c_str());

    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        ESP_LOGE(TAG, "Avatar is invalid");
        return;
    }

    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font  = lvgl_theme->text_font()->font();

    stackchan.avatar().setSpeechTextFont((void*)text_font);
}

#include <hal/board/hal_bridge.h>
static bool _is_xiaozhi_ready = false;
bool hal_bridge::is_xiaozhi_ready()
{
    return _is_xiaozhi_ready;
}

void StackChanAvatarDisplay::SetStatus(const char* status)
{
    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        ESP_LOGE(TAG, "Avatar is invalid");
        return;
    }

    auto& avatar = stackchan.avatar();
    auto& motion = stackchan.motion();

    DisplayLockGuard lock(this);

    // Face detection is decoupled from chat state — runs whenever
    // Dotty isn't sleeping. The previous gating (enable in STANDBY,
    // disable in everything else) silently killed walk-up greetings
    // whenever the device missed the transition back to STANDBY,
    // because face_detected events stopped reaching the bridge.
    if (!is_sleeping_) {
        FaceDetector::getInstance().setEnabled(true);
    }

    // Phase 3 — face_tracking + idle_motion stay alive across LISTENING /
    // SPEAKING / STANDBY so the head doesn't go dead-eyed mid-chat. Lazy-
    // create on the first SetStatus call (any state); per-state behaviour
    // is driven by the ChatProfile pushed via setChatProfile() further down.
    // Sleep entry still removes both modifiers — see the sleep handler at
    // ~line 374. That remains the only chat-related lifecycle removal point.
    if (!is_sleeping_) {
        if (face_tracking_modifier_id_ < 0) {
            face_tracking_modifier_id_ = stackchan.addModifier(
                std::make_unique<FaceTrackingModifier>());
        }
        if (idle_motion_modifier_id_ < 0) {
            idle_motion_modifier_id_ = stackchan.addModifier(
                std::make_unique<IdleMotionModifier>());
        }
    }

    bool is_idle      = false;
    bool is_listening = false;
    bool is_speaking  = false;
    const ChatProfile* profile_to_push = nullptr;

    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        in_listening_status_ = true;
        is_listening         = true;
        profile_to_push      = &kChatProfileListening;
        if (speaking_modifier_id_ >= 0) {
            stackchan.removeModifier(speaking_modifier_id_);
            avatar.mouth().setWeight(0);
            speaking_modifier_id_ = -1;
        }
        if (thinking_modifier_id_ >= 0) {
            stackchan.removeModifier(thinking_modifier_id_);
            avatar.mouth().setWeight(0);
            thinking_modifier_id_ = -1;
        }

        thinking_led_pending_ = false;
        set_left_leds(0, 50, 0);

        esp_timer_stop(bubble_clear_timer_);
        esp_timer_start_once(bubble_clear_timer_, 2500 * 1000);

        esp_timer_stop(thinking_timer_);

    } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        _is_xiaozhi_ready = true;
        in_listening_status_ = false;
        thinking_led_pending_ = false;
        is_idle              = true;
        profile_to_push      = &kChatProfileIdle;
        esp_timer_stop(thinking_timer_);

        if (speaking_modifier_id_ >= 0) {
            stackchan.removeModifier(speaking_modifier_id_);
            avatar.mouth().setWeight(0);
            speaking_modifier_id_ = -1;
        }
        if (thinking_modifier_id_ >= 0) {
            stackchan.removeModifier(thinking_modifier_id_);
            avatar.mouth().setWeight(0);
            thinking_modifier_id_ = -1;
        }

        set_left_leds(0, 0, 0);

        esp_timer_stop(bubble_clear_timer_);
        esp_timer_start_once(bubble_clear_timer_, 2500 * 1000);

    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        in_listening_status_ = false;
        thinking_led_pending_ = false;
        is_speaking          = true;
        profile_to_push      = &kChatProfileSpeaking;
        esp_timer_stop(thinking_timer_);
        if (thinking_modifier_id_ >= 0) {
            stackchan.removeModifier(thinking_modifier_id_);
            thinking_modifier_id_ = -1;
        }

        if (speaking_modifier_id_ < 0) {
            speaking_modifier_id_ = stackchan.addModifier(std::make_unique<SpeakingModifier>());
        }

        esp_timer_stop(bubble_clear_timer_);

        set_left_leds(0, 0, 50);
    } else {
        avatar.setSpeech(status);
    }

    // Phase 3 — push the chat-state profile to face_tracking. setChatProfile
    // threads through to idle_motion (overlay cadence + amplitude) so both
    // modifiers reflect the new chat state without further plumbing here.
    if (profile_to_push) {
        if (auto* ft = static_cast<FaceTrackingModifier*>(
                stackchan.getModifierByName(FaceTrackingModifier::kName))) {
            ft->setChatProfile(*profile_to_push);
        }
    }

    if (is_idle) {
        // idle_expression is the FACE-expression overlay (separate from
        // gaze) — still IDLE-gated because we don't want random expression
        // changes mid-listening or mid-speaking.
        if (idle_expression_modifier_id_ < 0) {
            idle_expression_modifier_id_ = stackchan.addModifier(
                std::make_unique<IdleExpressionModifier>());
        }
        // Right ring middle pixels (8-10) stay dark in idle. The previous
        // always-on cyan "face-detection mode active" indicator was visual
        // noise — face detector is now permanently on (since fix `8d74dd7`
        // decoupled it from chat state) so a continuous indicator carried
        // no actionable signal. The privacy LEDs at indices 6 and 11 already
        // give the family the "is the camera on?" answer (red on index 11).
        // Future: tie indices 8-10 to face_tracking state (green when a
        // face is actively being tracked) — that's the "is Dotty looking
        // at me right now?" signal worth lighting.
        stackchan.rightNeonLight().setColor(0, 0, 0);

        // Phase 1.2: register the ambient sound localizer once. Its
        // callback fires from the audio input task whenever stereo
        // PCM is read (always, since wake-word is running at idle),
        // emits sound_event(direction) on direction change.
        static bool s_sound_localizer_registered = false;
        if (!s_sound_localizer_registered) {
            s_sound_localizer_registered = true;
            static stackchan::SoundLocalizer s_sound_localizer;
            Application::GetInstance().GetAudioService().OnStereoFrame(
                [](const std::vector<int16_t>& lr) {
                    s_sound_localizer.OnStereoFrame(lr);
                });
        }
    } else {
        // Phase 3: face_tracking + idle_motion are NOT removed here —
        // both stay alive for the whole session and are tuned per chat
        // state via setChatProfile (above). The only chat-related lifecycle
        // removal is the sleep handler (~line 374).
        if (idle_expression_modifier_id_ >= 0) {
            stackchan.removeModifier(idle_expression_modifier_id_);
            idle_expression_modifier_id_ = -1;
        }
        // Clear cyan mode-active LED. The left LED is owned by the chat-state
        // set_left_leds() call earlier in this function, so don't touch it here.
        stackchan.rightNeonLight().setColor(0, 0, 0);
    }

    // Clear sleep state
    if (is_sleeping_) {
        avatar.setSpeech("");
    }
}

void StackChanAvatarDisplay::ShowNotification(const char* notification, int duration_ms)
{
}
