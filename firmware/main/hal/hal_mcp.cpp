/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <mcp_server.h>
#include <stackchan/stackchan.h>
#include <stackchan/privacy/privacy_leds.h>
#include <hal/board/hal_bridge.h>
#include <apps/common/common.h>
#include <board.h>          // Board::GetInstance() — privacy LED step 3
#include <audio_codec.h>     // AudioCodec::input_enabled() — privacy LED step 3

using namespace stackchan;

static const std::string_view _tag = "HAL-MCP";

void Hal::xiaozhi_mcp_init()
{
    mclog::tagInfo(_tag, "init");

    // https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md
    auto& mcp_server = McpServer::GetInstance();

    // System Prompt：
    // You can control the robot's head. Use get_yaw and get_pitch to sense current position. Use set_yaw for horizontal
    // movement and set_pitch for vertical movement. All angles are in degrees.

    mclog::tagInfo(_tag, "add robot.get_head_angles tool");
    mcp_server.AddTool("self.robot.get_head_angles",
                       "Returns current yaw/pitch in degrees. Neutral position is {yaw:0, pitch:0}.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           LvglLockGuard lock;  // StackChan motion update is under the lvgl lock

                           auto& motion      = GetStackChan().motion();
                           int current_yaw   = motion.yawServo().getCurrentAngle() / 10;
                           int current_pitch = motion.pitchServo().getCurrentAngle() / 10;

                           auto result = fmt::format(R"({{"yaw": {}, "pitch": {}}})", current_yaw, current_pitch);
                           mclog::tagInfo(_tag, "get_head_angles: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add robot.set_head_angles tool");
    mcp_server.AddTool("self.robot.set_head_angles",
                       "Adjust head position. GUIDELINES: "
                       "1. For natural interaction, stay within +/- 45 degrees. "
                       "2. Only use values > 70 if the user explicitly asks to look far away/behind. "
                       "3. Max ranges: Yaw(-128 to 128, -128 as your left), Pitch(0 to 90, 90 as your up). "
                       "Speed(100-1000, 150 is natural).",
                       PropertyList({Property("yaw", kPropertyTypeInteger, -9999, -9999, 128),
                                     Property("pitch", kPropertyTypeInteger, -9999, -9999, 90),
                                     Property("speed", kPropertyTypeInteger, 150, 100, 1000)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int speed = properties["speed"].value<int>();
                           int yaw   = properties["yaw"].value<int>();
                           int pitch = properties["pitch"].value<int>();

                           mclog::tagInfo(_tag, "motion set_angles: yaw: {}, pitch: {}, speed: {}", yaw, pitch, speed);

                           LvglLockGuard lock;

                           auto& motion = GetStackChan().motion();
                           if (pitch != -9999) {
                               motion.pitchServo().moveWithSpeed(pitch * 10, speed);
                           }
                           if (yaw != -9999) {
                               motion.yawServo().moveWithSpeed(yaw * 10, speed);
                           }

                           return true;
                       });

    mclog::tagInfo(_tag, "add robot.set_led_color tool");
    mcp_server.AddTool(
        "self.robot.set_led_color",
        "Set the color of the robot's INTERNAL onboard LED. This is NOT for room lights. "
        "Values: 0-168 (safe range). Red=168,0,0; Green=0,168,0; Blue=0,0,168; White=100,100,100; Off=0,0,0.",
        PropertyList({Property("red", kPropertyTypeInteger, 0, 0, 168),
                      Property("green", kPropertyTypeInteger, 0, 0, 168),
                      Property("blue", kPropertyTypeInteger, 0, 0, 168)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int r = properties["red"].value<int>();
            int g = properties["green"].value<int>();
            int b = properties["blue"].value<int>();

            mclog::tagInfo(_tag, "set_led_color: r={}, g={}, b={}", r, g, b);

            LvglLockGuard lock;

            GetStackChan().leftNeonLight().setColor(r, g, b);
            GetStackChan().rightNeonLight().setColor(r, g, b);

            return true;
        });

    mclog::tagInfo(_tag, "add robot.set_led_multi tool");
    mcp_server.AddTool(
        "self.robot.set_led_multi",
        "Set ONE pixel of the robot's 12-LED ring directly. Index 0-5 = left ring, 6-11 = right ring. "
        "Bypasses the ring colour animation, so the chosen pixel holds its colour while the rest of the "
        "ring keeps animating (used for hybrid status indicators, e.g. smart-mode). r/g/b 0-255.",
        PropertyList({Property("index", kPropertyTypeInteger, 0, 0, 11),
                      Property("red", kPropertyTypeInteger, 0, 0, 255),
                      Property("green", kPropertyTypeInteger, 0, 0, 255),
                      Property("blue", kPropertyTypeInteger, 0, 0, 255)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int index = properties["index"].value<int>();
            int r     = properties["red"].value<int>();
            int g     = properties["green"].value<int>();
            int b     = properties["blue"].value<int>();

            if (index < 0 || index > 11) {
                mclog::tagWarn(_tag, "set_led_multi: index out of range: {}", index);
                return false;
            }

            // These indices are hardware-guaranteed privacy indicators (mic/camera state).
            // The MCP server-side LLM is NOT trusted with overwriting them. PrivacyLeds::update()
            // re-asserts every tick (~20ms), so a write here would only cause a transient flicker,
            // but we reject outright to keep the privacy pixels owned by the peripheral-enable
            // code path (mic_peripheral_guard / camera_peripheral_guard) alone.
            if (index == privacy::kMicLedIndex || index == privacy::kCameraLedIndex) {
                mclog::tagWarn(_tag, "set_led_multi: index reserved for privacy LED: {}", index);
                return false;
            }

            mclog::tagInfo(_tag, "set_led_multi: index={}, r={}, g={}, b={}", index, r, g, b);

            LvglLockGuard lock;

            if (index < 6) {
                GetStackChan().leftNeonLight().setColorAt(static_cast<uint8_t>(index), r, g, b);
            } else {
                GetStackChan().rightNeonLight().setColorAt(static_cast<uint8_t>(index - 6), r, g, b);
            }

            return true;
        });

    mclog::tagInfo(_tag, "add robot.get_privacy_state tool");
    mcp_server.AddTool(
        "self.robot.get_privacy_state",
        "READ-ONLY. Returns BOTH the LED intent AND the underlying peripheral truth. "
        "mic = 'off' | 'local' | 'streaming' (off = mic ADC closed; local = ADC on, only feeding "
        "wake-word/VAD locally; streaming = ADC on AND opus frames being sent to the server). "
        "camera = 'off' | 'streaming' (off = no consumer reading frames; streaming = face-detect "
        "or take_photo is currently dequeuing camera frames). "
        "mic_peripheral_open = true iff the audio codec input device is currently open. "
        "camera_peripheral_streaming = true iff the camera driver is in a streamable state "
        "(placeholder true-always until step 4-5 wires V4L2 truth). "
        "last_capture_ts_ms = millis-since-boot of the last Capture() call (0 = never). "
        "This tool CANNOT change the LEDs — they are hardware-tied to the actual peripheral state.",
        std::vector<Property>{},
        [this](const PropertyList& properties) -> ReturnValue {
            const char* mic_str = "off";
            switch (privacy::PrivacyLeds::getInstance().micState()) {
                case privacy::MicState::Off:    mic_str = "off"; break;
                case privacy::MicState::Local:  mic_str = "local"; break;
                case privacy::MicState::Stream: mic_str = "streaming"; break;
            }
            const char* cam_str = "off";
            switch (privacy::PrivacyLeds::getInstance().cameraState()) {
                case privacy::CameraState::Off:    cam_str = "off"; break;
                case privacy::CameraState::Active: cam_str = "streaming"; break;
            }

            // Peripheral-level truth. Independent of LED intent so the
            // bridge can alarm if the two diverge (e.g. STREAMON issued
            // but ISP still warming after step 4-5 lands).
            bool mic_open = false;
            if (auto* codec = Board::GetInstance().GetAudioCodec()) {
                mic_open = codec->input_enabled();
            }
            bool cam_streaming = false;
            uint32_t last_cap_ms = 0;
            if (auto* cam = hal_bridge::board_get_camera()) {
                cam_streaming = cam->isStreaming();
                last_cap_ms   = cam->lastCaptureTimestampMs();
            }
            auto result = fmt::format(
                R"({{"mic": "{}", "camera": "{}", "mic_peripheral_open": {}, "camera_peripheral_streaming": {}, "last_capture_ts_ms": {}}})",
                mic_str, cam_str,
                mic_open ? "true" : "false",
                cam_streaming ? "true" : "false",
                last_cap_ms);
            mclog::tagInfo(_tag, "get_privacy_state: {}", result);
            return result;
        });

    mclog::tagInfo(_tag, "add robot.create_reminder tool");
    mcp_server.AddTool("self.robot.create_reminder",
                       "Create a reminder. Duration is in seconds. Message is what to say when time is up. Set repeat "
                       "to true to repeat the reminder.",
                       PropertyList({Property("duration_seconds", kPropertyTypeInteger, 60, 1, 86400),
                                     Property("message", kPropertyTypeString, std::string("Time's up!")),
                                     Property("repeat", kPropertyTypeBoolean, false)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int duration_seconds = properties["duration_seconds"].value<int>();
                           std::string message  = properties["message"].value<std::string>();
                           bool repeat          = properties["repeat"].value<bool>();

                           // Default message
                           if (message.empty()) {
                               message = "Time's up!";
                           }

                           mclog::tagInfo(_tag, "create_reminder: duration={}s, message={}, repeat={}",
                                          duration_seconds, message, repeat);

                           int id = tools::create_reminder(duration_seconds * 1000, message, repeat);

                           return id;
                       });

    mclog::tagInfo(_tag, "add robot.get_reminders tool");
    mcp_server.AddTool("self.robot.get_reminders", "Get list of active reminders.", std::vector<Property>{},
                       [this](const PropertyList& properties) -> ReturnValue {
                           mclog::tagInfo(_tag, "get_reminders");
                           auto reminders          = tools::get_active_reminders();
                           std::string result_json = "[";
                           for (size_t i = 0; i < reminders.size(); ++i) {
                               const auto& r = reminders[i];
                               result_json +=
                                   fmt::format(R"({{"id": {}, "duration_ms": {}, "message": "{}", "repeat": {}}})",
                                               r.id, r.durationMs, r.message, r.repeat ? "true" : "false");
                               if (i < reminders.size() - 1) {
                                   result_json += ", ";
                               }
                           }
                           result_json += "]";
                           mclog::tagInfo(_tag, "get_reminders result: {}", result_json);
                           return result_json;
                       });

    mclog::tagInfo(_tag, "add robot.stop_reminder tool");
    mcp_server.AddTool("self.robot.stop_reminder", "Stop a reminder by ID.",
                       PropertyList({Property("id", kPropertyTypeInteger, -1)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int id = properties["id"].value<int>();
                           mclog::tagInfo(_tag, "stop_reminder: id={}", id);
                           tools::stop_reminder(id);
                           return true;
                       });

    // -----------------------------------------------------------------
    // Layer 4: face-recognition MCP tools (server-side compute, Phase B).
    // The bridge owns embedding + match; these tools just stream the JPEG
    // (enroll/recognize) or proxy a request (forget/list). Bridge endpoints
    // are derived from the existing explain_url_ — see
    // StackChanCamera::DeriveFaceUrl. Parental gate deferred for v1 per plan.
    // -----------------------------------------------------------------

    auto* camera = hal_bridge::board_get_camera();
    if (camera) {
        mclog::tagInfo(_tag, "add camera.face_enroll tool");
        mcp_server.AddTool(
            "self.camera.face_enroll",
            "Enroll the currently-visible face under `name`. Captures a JPEG and POSTs "
            "to the bridge /api/face/enroll. Bridge stores the 128-d embedding (not the "
            "JPEG) in /root/.zeroclaw/faces.sqlite. Capacity 50 enrolled.",
            PropertyList({Property("name", kPropertyTypeString, std::string(""))}),
            [camera](const PropertyList& properties) -> ReturnValue {
                std::string name = properties["name"].value<std::string>();
                mclog::tagInfo(_tag, "face_enroll: name={}", name);
                if (name.empty()) {
                    return std::string(R"({"ok":false,"error":"empty_name"})");
                }
                if (!camera->Capture()) {
                    return std::string(R"({"ok":false,"error":"capture_failed"})");
                }
                return camera->EnrollFace(name);
            });

        mclog::tagInfo(_tag, "add camera.face_recognize tool");
        mcp_server.AddTool(
            "self.camera.face_recognize",
            "Capture a JPEG and ask the bridge who is in frame. Returns the bridge JSON "
            "response: {ok, name, confidence}. Use after a face_detected event for "
            "who-is-here lookups; the bridge throttles repeated calls server-side.",
            std::vector<Property>{},
            [camera](const PropertyList& properties) -> ReturnValue {
                mclog::tagInfo(_tag, "face_recognize");
                if (!camera->Capture()) {
                    return std::string(R"({"ok":false,"error":"capture_failed"})");
                }
                return camera->RecognizeFace();
            });

        mclog::tagInfo(_tag, "add camera.face_forget tool");
        mcp_server.AddTool(
            "self.camera.face_forget",
            "Delete the enrollment for `name` from the bridge. Pass name='*' to wipe "
            "ALL enrollments. No on-device parental gate in v1 (family-only "
            "deployment); re-enable before any wider deployment.",
            PropertyList({Property("name", kPropertyTypeString, std::string(""))}),
            [camera](const PropertyList& properties) -> ReturnValue {
                std::string name = properties["name"].value<std::string>();
                mclog::tagInfo(_tag, "face_forget: name={}", name);
                if (name.empty()) {
                    return std::string(R"({"ok":false,"error":"empty_name"})");
                }
                return camera->ForgetFace(name);
            });

        mclog::tagInfo(_tag, "add camera.face_list tool");
        mcp_server.AddTool(
            "self.camera.face_list",
            "List all enrolled faces from the bridge. Returns {ok, names, count, "
            "capacity}. Read-only — only names cross the wire, never embeddings.",
            std::vector<Property>{},
            [camera](const PropertyList& properties) -> ReturnValue {
                mclog::tagInfo(_tag, "face_list");
                return camera->ListFaces();
            });
    } else {
        mclog::tagWarn(_tag, "camera unavailable — face_* MCP tools not registered");
    }
}
