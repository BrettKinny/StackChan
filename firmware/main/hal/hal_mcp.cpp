/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <mcp_server.h>
#include <stackchan/stackchan.h>
#include <stackchan/modes/state_manager.h>
#include <hal/board/hal_bridge.h>
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
                               motion.pitchServo().moveWithSpeed(pitch * 10, speed, "mcp_set_head_angles");
                           }
                           if (yaw != -9999) {
                               motion.yawServo().moveWithSpeed(yaw * 10, speed, "mcp_set_head_angles");
                           }

                           return true;
                       });

    mclog::tagInfo(_tag, "add robot.set_led_color tool");
    mcp_server.AddTool(
        "self.robot.set_led_color",
        "Set the colour of the LEFT half of the LED ring (state-arc, 6 pixels). "
        "The RIGHT half is reserved for owned status indicators (face, kid_mode, "
        "smart_mode, listening) and is not writable from this tool. Useful for "
        "chat-driven LED play. Values 0-168 per channel.",
        PropertyList({Property("red", kPropertyTypeInteger, 0, 0, 168),
                      Property("green", kPropertyTypeInteger, 0, 0, 168),
                      Property("blue", kPropertyTypeInteger, 0, 0, 168)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int r = properties["red"].value<int>();
            int g = properties["green"].value<int>();
            int b = properties["blue"].value<int>();

            mclog::tagInfo(_tag, "set_led_color (left ring only): r={}, g={}, b={}", r, g, b);

            LvglLockGuard lock;

            GetStackChan().leftNeonLight().setColor(r, g, b);
            // Right ring is owned by StateManager — do not write here.

            return true;
        });

    mclog::tagInfo(_tag, "add robot.set_led_multi tool");
    mcp_server.AddTool(
        "self.robot.set_led_multi",
        "Set ONE pixel of the LEFT state-arc ring directly. Index 0-5 = left ring. "
        "Right-ring indices 6-11 are reserved for owned status indicators "
        "(face, kid_mode, smart_mode, listening) and cannot be written through "
        "this tool. Bypasses the ring colour animation, so the chosen pixel holds "
        "its colour while the rest of the ring keeps animating. r/g/b 0-255.",
        PropertyList({Property("index", kPropertyTypeInteger, 0, 0, 5),
                      Property("red", kPropertyTypeInteger, 0, 0, 255),
                      Property("green", kPropertyTypeInteger, 0, 0, 255),
                      Property("blue", kPropertyTypeInteger, 0, 0, 255)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int index = properties["index"].value<int>();
            int r     = properties["red"].value<int>();
            int g     = properties["green"].value<int>();
            int b     = properties["blue"].value<int>();

            if (index < 0 || index > 5) {
                mclog::tagWarn(_tag, "set_led_multi: index {} not on left ring (0-5); ignoring", index);
                return false;
            }

            mclog::tagInfo(_tag, "set_led_multi: index={}, r={}, g={}, b={}", index, r, g, b);

            LvglLockGuard lock;

            GetStackChan().leftNeonLight().setColorAt(static_cast<uint8_t>(index), r, g, b);

            return true;
        });

    mclog::tagInfo(_tag, "add robot.set_state tool");
    mcp_server.AddTool(
        "self.robot.set_state",
        "Set Dotty's high-level state. Mutually exclusive — exactly one is active. "
        "Valid: idle, talk, story_time, security, sleep, dance. Paints the state "
        "arc across left ring 0-5 and selects the idle-motion profile.",
        PropertyList({Property("state", kPropertyTypeString, std::string("idle"))}),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string s = properties["state"].value<std::string>();
            stackchan::State out;
            if (!stackchan::StateManager::parseState(s.c_str(), out)) {
                mclog::tagWarn(_tag, "set_state: unknown state {}", s);
                return false;
            }
            auto* sm = static_cast<stackchan::StateManager*>(
                GetStackChan().getModifierByName(stackchan::StateManager::kName));
            if (!sm) {
                mclog::tagWarn(_tag, "set_state: StateManager not found in modifier pool");
                return false;
            }
            mclog::tagInfo(_tag, "set_state: {}", s);
            LvglLockGuard lock;
            sm->setState(out);
            return true;
        });

    mclog::tagInfo(_tag, "add robot.set_toggle tool");
    mcp_server.AddTool(
        "self.robot.set_toggle",
        "Set a Dotty toggle on/off. Toggles compose freely with state. "
        "Valid names: kid_mode (warm pink pip on right ring index 8), "
        "smart_mode (orange pip on right ring index 9).",
        PropertyList({Property("name", kPropertyTypeString, std::string("")),
                      Property("enabled", kPropertyTypeBoolean, false)}),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string name = properties["name"].value<std::string>();
            bool enabled     = properties["enabled"].value<bool>();
            auto* sm = static_cast<stackchan::StateManager*>(
                GetStackChan().getModifierByName(stackchan::StateManager::kName));
            if (!sm) {
                mclog::tagWarn(_tag, "set_toggle: StateManager not found in modifier pool");
                return false;
            }
            mclog::tagInfo(_tag, "set_toggle: {}={}", name, enabled);
            LvglLockGuard lock;
            if (name == "kid_mode") {
                sm->setKidMode(enabled);
            } else if (name == "smart_mode") {
                sm->setSmartMode(enabled);
            } else {
                mclog::tagWarn(_tag, "set_toggle: unknown name {}", name);
                return false;
            }
            return true;
        });

    mclog::tagInfo(_tag, "add robot.set_face_identified tool");
    mcp_server.AddTool(
        "self.robot.set_face_identified",
        "Signal that the currently-detected face has been identified by the "
        "server-side VLM/roster pipeline. Lights the right-ring face pixel "
        "(global 6) green for ~4 seconds; refresh by calling again. No-op "
        "if no face is currently detected. The bridge calls this after a "
        "successful room-view identification.",
        std::vector<Property>{},
        [this](const PropertyList& properties) -> ReturnValue {
            auto* sm = static_cast<stackchan::StateManager*>(
                GetStackChan().getModifierByName(stackchan::StateManager::kName));
            if (!sm) {
                mclog::tagWarn(_tag, "set_face_identified: StateManager not found in modifier pool");
                return false;
            }
            mclog::tagInfo(_tag, "set_face_identified");
            LvglLockGuard lock;
            sm->setFaceIdentified();
            return true;
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
}
