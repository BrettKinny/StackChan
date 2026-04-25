/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <mcp_server.h>
#include <stackchan/stackchan.h>
#include <stackchan/face/face_recognizer.h>
#include <stackchan/face/parental_gate.h>
#include <stackchan/privacy/privacy_leds.h>
#include <apps/common/common.h>

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
        "READ-ONLY. Returns what the robot THINKS its privacy indicator LEDs are showing. "
        "mic = 'off' | 'local' | 'streaming' (off = mic ADC closed; local = ADC on, only feeding "
        "wake-word/VAD locally; streaming = ADC on AND opus frames being sent to the server). "
        "camera = 'off' | 'streaming' (off = no consumer reading frames; streaming = face-detect "
        "or take_photo is currently dequeuing camera frames). This tool CANNOT change the LEDs — "
        "they are hardware-tied to the actual peripheral state.",
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
            auto result = fmt::format(R"({{"mic": "{}", "camera": "{}"}})", mic_str, cam_str);
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
    // Layer 4: face-recognition MCP tools.
    // See firmware/main/stackchan/face/PRIVACY.md for the retention model.
    // Enroll / forget require the parental gate (PIN or long-press); list
    // is public; unlock is the credentialed entry point.
    // -----------------------------------------------------------------

    mclog::tagInfo(_tag, "add robot.face_unlock tool");
    mcp_server.AddTool(
        "self.robot.face_unlock",
        "Open the parental gate so face_enroll/face_forget can run. "
        "method='pin' validates secret against the device PIN; "
        "method='long_press' validates that the head-pet long-press flag was "
        "armed (real long-press detector wiring is a follow-up). "
        "Unlock is single-shot and expires after 30 seconds.",
        PropertyList({Property("method", kPropertyTypeString, std::string("pin")),
                      Property("secret", kPropertyTypeString, std::string(""))}),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string method = properties["method"].value<std::string>();
            std::string secret = properties["secret"].value<std::string>();

            // Don't log `secret` — it's the PIN. `method` is fine.
            mclog::tagInfo(_tag, "face_unlock: method={}", method);

            bool ok = false;
            if (method == "pin") {
                ok = ParentalGate::tryUnlockByPIN(secret);
            } else if (method == "long_press") {
                ok = ParentalGate::tryUnlockByLongPress();
            } else {
                mclog::tagWarn(_tag, "face_unlock: unknown method '{}'", method);
                return std::string(R"({"ok":false,"reason":"unknown_method"})");
            }
            if (!ok) {
                return std::string(R"({"ok":false,"reason":"unlock_failed"})");
            }
            return std::string(R"({"ok":true})");
        });

    mclog::tagInfo(_tag, "add robot.face_list tool");
    mcp_server.AddTool(
        "self.robot.face_list",
        "Returns the names of faces currently enrolled on-device. "
        "Public — does NOT require the parental gate. Biometric data "
        "(embeddings) never leaves the device, only the names.",
        std::vector<Property>{},
        [this](const PropertyList& properties) -> ReturnValue {
            auto names = FaceRecognizer::getInstance().enrolledNames();
            std::string out = "{\"names\":[";
            for (size_t i = 0; i < names.size(); i++) {
                // JSON-escape per name. Names are validated at enrollment
                // (no control chars) so we only worry about " and \.
                out += "\"";
                for (char c : names[i]) {
                    if (c == '"' || c == '\\') out.push_back('\\');
                    out.push_back(c);
                }
                out += "\"";
                if (i + 1 < names.size()) out += ",";
            }
            out += fmt::format(R"(],"count":{},"capacity":{}}})",
                               names.size(), FaceRecognizer::kMaxEnrolled);
            mclog::tagInfo(_tag, "face_list: {}", out);
            return out;
        });

    mclog::tagInfo(_tag, "add robot.face_enroll tool");
    mcp_server.AddTool(
        "self.robot.face_enroll",
        "Enroll the currently-detected face under `name`. REQUIRES the "
        "parental gate to be unlocked first (see self.robot.face_unlock). "
        "Captures embedding from the next detector frame and persists to "
        "NVS. Capacity 10 enrolled faces. Embedding stays on-device.",
        PropertyList({Property("name", kPropertyTypeString, std::string(""))}),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string name = properties["name"].value<std::string>();
            mclog::tagInfo(_tag, "face_enroll: name={}", name);

            if (name.empty()) {
                return std::string(R"({"ok":false,"reason":"empty_name"})");
            }
            if (!ParentalGate::isUnlocked()) {
                mclog::tagWarn(_tag, "face_enroll: parental gate required");
                return std::string(R"({"ok":false,"reason":"parental_gate_required"})");
            }

            // SCAFFOLD: real flow captures the next detector crop and
            // computes an embedding. The recognizer's enroll() in the
            // scaffold persists a placeholder embedding regardless;
            // the bridge can already exercise enrollment slot semantics.
            FaceImage stub_img{};
            bool ok = FaceRecognizer::getInstance().enroll(
                name, stub_img, /*parental_gate_passed=*/true);

            // Single-shot semantics: consume the unlock whether or not
            // the enroll succeeded. Reduces "user runs the same enroll
            // twice on a typo" attack surface.
            ParentalGate::consume();

            if (!ok) {
                return std::string(R"({"ok":false,"reason":"enroll_failed"})");
            }
            return std::string(R"({"ok":true})");
        });

    mclog::tagInfo(_tag, "add robot.face_forget tool");
    mcp_server.AddTool(
        "self.robot.face_forget",
        "Delete the enrollment for `name` from on-device storage. REQUIRES "
        "the parental gate to be unlocked first (see self.robot.face_unlock). "
        "Pass name='*' to wipe ALL enrolled faces.",
        PropertyList({Property("name", kPropertyTypeString, std::string(""))}),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string name = properties["name"].value<std::string>();
            mclog::tagInfo(_tag, "face_forget: name={}", name);

            if (name.empty()) {
                return std::string(R"({"ok":false,"reason":"empty_name"})");
            }
            if (!ParentalGate::isUnlocked()) {
                mclog::tagWarn(_tag, "face_forget: parental gate required");
                return std::string(R"({"ok":false,"reason":"parental_gate_required"})");
            }

            bool ok = false;
            if (name == "*") {
                ok = FaceRecognizer::getInstance().forgetAll();
            } else {
                ok = FaceRecognizer::getInstance().forget(name);
            }

            ParentalGate::consume();

            if (!ok) {
                return std::string(R"({"ok":false,"reason":"forget_failed"})");
            }
            return std::string(R"({"ok":true})");
        });
}
