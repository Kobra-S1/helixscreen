// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sensor_enum_names.h"

#include <string>
#include <string_view>
#include <vector>

namespace helix::sensors {

/// @brief What an on/off input means to generic consumers.
enum class InputSensorRole {
    NONE = 0, ///< A plain input with no generic meaning (shown as "Other")
    DOOR = 1, ///< A door, lid or cover switch: the door-open print-start gate reads it
};

/// @brief Kind of Klipper object an input comes from.
enum class InputSensorType {
    GCODE_BUTTON = 1, ///< [gcode_button] (state: "PRESSED" / "RELEASED")
};

/// @brief Configuration for one on/off input
struct InputSensorConfig {
    std::string klipper_name; ///< Full Klipper name (e.g., "gcode_button frontDoor")
    std::string sensor_name;  ///< Short name (e.g., "frontDoor")
    InputSensorType type = InputSensorType::GCODE_BUTTON;
    InputSensorRole role = InputSensorRole::NONE;
    bool enabled = true;
    /// A door switch reads closed while pressed. Inverted, it reads closed while released.
    bool inverted = false;

    InputSensorConfig() = default;

    InputSensorConfig(std::string klipper_name_, std::string sensor_name_, InputSensorType type_)
        : klipper_name(std::move(klipper_name_)), sensor_name(std::move(sensor_name_)),
          type(type_) {}
};

/// @brief Runtime state for one on/off input
struct InputSensorState {
    bool pressed = false;   ///< Last reported state
    bool reported = false;  ///< A state has arrived since discovery
    bool available = false; ///< Present in the current Klipper config
};

inline constexpr EnumName<InputSensorRole> kInputSensorRoles[] = {
    {InputSensorRole::NONE, "other", "Other"},
    {InputSensorRole::DOOR, "door", "Door"},
};

[[nodiscard]] inline std::string input_role_to_string(InputSensorRole role) {
    return enum_id(kInputSensorRoles, role);
}

[[nodiscard]] inline InputSensorRole input_role_from_string(const std::string& str) {
    return enum_from_id(kInputSensorRoles, str);
}

[[nodiscard]] inline std::string input_type_to_string(InputSensorType /*type*/) {
    return "gcode_button";
}

/// @brief Which edges of an input the printer's own config already acts on: a
/// gcode_button's press_gcode / release_gcode holding more than whitespace.
struct InputConfigReaction {
    bool on_press = false;
    bool on_release = false;
};

/// @brief Whether the printer itself reacts when this door opens, so a screen
/// warning would repeat it. A door opens on release, or on press when inverted.
[[nodiscard]] bool firmware_reacts_to_opening(const InputConfigReaction& reaction, bool inverted);

/// @brief The role an input gets before the user picks one: Door when its name
/// says door, lid or cover (any case), else none.
[[nodiscard]] InputSensorRole default_input_role(std::string_view sensor_name);

/// @brief Whether a Door input reads open. An input with no reported state, or
/// any role other than Door, never does.
[[nodiscard]] bool input_reads_open(const InputSensorConfig& config, const InputSensorState& state);

/// @brief The doors in @p now that are not in @p before: the ones that just
/// opened, each owed one mid-print warning.
[[nodiscard]] std::vector<std::string> doors_newly_open(const std::vector<std::string>& before,
                                                        const std::vector<std::string>& now);

} // namespace helix::sensors
