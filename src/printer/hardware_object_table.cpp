// SPDX-License-Identifier: GPL-3.0-or-later

#include "hardware_object_table.h"

#include "accel_sensor_types.h"
#include "humidity_sensor_types.h"
#include "klipper_extruder_naming.h"
#include "probe_sensor_types.h"

#include <cctype>

namespace helix::hw {

namespace {

using helix::sensors::AccelSensorType;
using helix::sensors::ProbeSensorType;

enum class Match : uint8_t {
    Exact,        ///< the whole name equals the pattern
    Prefix,       ///< the name starts with the pattern; the rest is the instance
    BareOrPrefix, ///< Exact, or Prefix with the pattern followed by a space
};

/// Which lookups a row answers: objects.list names, configfile sections, or both.
enum class Scope : uint8_t { Both, SectionOnly };

struct Row {
    Match match;
    std::string_view pattern; ///< Prefix patterns carry their trailing space
    ObjectKind kind;
    uint8_t chip;
    Scope scope;
};

constexpr uint8_t chip_of(ProbeSensorType t) {
    return static_cast<uint8_t>(t);
}
constexpr uint8_t chip_of(AccelSensorType t) {
    return static_cast<uint8_t>(t);
}

// Scanned in order; the first row that matches wins. Longer or more specific
// patterns precede shorter ones that would otherwise swallow them.
constexpr Row kRows[] = {
    {Match::Prefix, "stepper_", ObjectKind::Stepper, 0, Scope::Both},
    {Match::Exact, "heater_bed", ObjectKind::Heater, 0, Scope::Both},
    {Match::Prefix, "heater_generic ", ObjectKind::GenericHeater, 0, Scope::Both},
    {Match::Prefix, "temperature_sensor ", ObjectKind::TemperatureSensor, 0, Scope::Both},
    {Match::Prefix, "temperature_fan ", ObjectKind::TemperatureFan, 0, Scope::Both},
    {Match::Prefix, "tmc2240 ", ObjectKind::DriverTemperature, 0, Scope::Both},
    {Match::Prefix, "tmc5160 ", ObjectKind::DriverTemperature, 0, Scope::Both},
    {Match::BareOrPrefix, "mcu", ObjectKind::Mcu, 0, Scope::Both},

    {Match::Exact, "fan_feedback", ObjectKind::FanFeedback, 0, Scope::Both},
    {Match::Exact, "fan", ObjectKind::Fan, 0, Scope::Both},
    {Match::Prefix, "heater_fan ", ObjectKind::HeaterFan, 0, Scope::Both},
    {Match::Prefix, "controller_fan ", ObjectKind::ControllerFan, 0, Scope::Both},
    {Match::Prefix, "fan_generic ", ObjectKind::GenericFan, 0, Scope::Both},

    {Match::Prefix, "led_effect ", ObjectKind::LedEffect, 0, Scope::Both},
    {Match::BareOrPrefix, "neopixel", ObjectKind::Led, 0, Scope::Both},
    {Match::BareOrPrefix, "dotstar", ObjectKind::Led, 0, Scope::Both},
    {Match::Prefix, "led ", ObjectKind::Led, 0, Scope::Both},

    {Match::Prefix, "filament_switch_sensor ", ObjectKind::FilamentSwitch, 0, Scope::Both},
    {Match::Prefix, "filament_motion_sensor ", ObjectKind::FilamentMotion, 0, Scope::Both},
    // A filament system's own sensor objects, published instead of the stock sections.
    {Match::Prefix, "zmod_ifs_switch_sensor ", ObjectKind::BackendFilamentSensor, 0, Scope::Both},
    {Match::Prefix, "zmod_ifs_motion_sensor ", ObjectKind::BackendFilamentSensor, 0, Scope::Both},
    {Match::Exact, "hall_filament_width_sensor", ObjectKind::WidthSensor, 0, Scope::Both},
    {Match::Exact, "tsl1401cl_filament_width_sensor", ObjectKind::WidthSensor, 0, Scope::Both},
    {Match::BareOrPrefix, "load_cell", ObjectKind::LoadCell, 0, Scope::Both},

    {Match::Exact, "probe", ObjectKind::Probe, chip_of(ProbeSensorType::STANDARD), Scope::Both},
    {Match::Exact, "bltouch", ObjectKind::Probe, chip_of(ProbeSensorType::BLTOUCH), Scope::Both},
    {Match::Exact, "smart_effector", ObjectKind::Probe, chip_of(ProbeSensorType::SMART_EFFECTOR),
     Scope::Both},
    {Match::Exact, "cartographer", ObjectKind::Probe, chip_of(ProbeSensorType::CARTOGRAPHER),
     Scope::Both},
    {Match::Exact, "beacon", ObjectKind::Probe, chip_of(ProbeSensorType::BEACON), Scope::Both},
    {Match::Prefix, "probe_eddy_current ", ObjectKind::Probe,
     chip_of(ProbeSensorType::EDDY_CURRENT), Scope::Both},
    {Match::Exact, "load_cell_probe", ObjectKind::Probe, chip_of(ProbeSensorType::LOADCELL),
     Scope::SectionOnly},
    {Match::Exact, "dockable_probe", ObjectKind::Probe, chip_of(ProbeSensorType::STANDARD),
     Scope::SectionOnly},

    {Match::Prefix, "gcode_button ", ObjectKind::Input, 0, Scope::Both},
    {Match::Exact, "power_loss_check", ObjectKind::PowerLossMonitor, 0, Scope::Both},

    // Config sections only: Klipper accelerometers have no get_status().
    {Match::BareOrPrefix, "adxl345", ObjectKind::Accelerometer, chip_of(AccelSensorType::ADXL345),
     Scope::SectionOnly},
    {Match::BareOrPrefix, "lis2dw", ObjectKind::Accelerometer, chip_of(AccelSensorType::LIS2DW),
     Scope::SectionOnly},
    {Match::BareOrPrefix, "lis3dh", ObjectKind::Accelerometer, chip_of(AccelSensorType::LIS3DH),
     Scope::SectionOnly},
    {Match::BareOrPrefix, "mpu9250", ObjectKind::Accelerometer, chip_of(AccelSensorType::MPU9250),
     Scope::SectionOnly},
    {Match::BareOrPrefix, "icm20948", ObjectKind::Accelerometer, chip_of(AccelSensorType::ICM20948),
     Scope::SectionOnly},
};

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

/// Whether @p row matches @p name; sets @p instance to the part after the prefix.
bool row_matches(const Row& row, std::string_view name, std::string_view& instance) {
    switch (row.match) {
    case Match::Exact:
        instance = {};
        return name == row.pattern;
    case Match::Prefix:
        if (!starts_with(name, row.pattern))
            return false;
        instance = name.substr(row.pattern.size());
        return true;
    case Match::BareOrPrefix:
        if (name == row.pattern) {
            instance = {};
            return true;
        }
        if (name.size() > row.pattern.size() && starts_with(name, row.pattern) &&
            name[row.pattern.size()] == ' ') {
            instance = name.substr(row.pattern.size() + 1);
            return true;
        }
        return false;
    }
    return false;
}

/// Whether @p name contains any of @p words (lowercase), ignoring case.
template <size_t N>
bool contains_word_ci(std::string_view name, const std::string_view (&words)[N]) {
    std::string lower(name);
    for (char& ch : lower)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const auto& w : words) {
        if (lower.find(w) != std::string::npos)
            return true;
    }
    return false;
}

/// Precedence fan > LED > speaker; matching is a case-insensitive substring
/// ("caselight", "Enclosure_LEDs" and "fan0" all match).
ObjectKind output_pin_kind(std::string_view pin_name) {
    static constexpr std::string_view kFanWords[] = {"fan"};
    static constexpr std::string_view kLedWords[] = {"light", "led", "lamp"};
    static constexpr std::string_view kSpeakerWords[] = {"beeper", "buzzer", "speaker"};
    if (contains_word_ci(pin_name, kFanWords))
        return ObjectKind::OutputPinFan;
    if (contains_word_ci(pin_name, kLedWords))
        return ObjectKind::OutputPinLed;
    if (contains_word_ci(pin_name, kSpeakerWords))
        return ObjectKind::Speaker;
    return ObjectKind::Unknown;
}

ObjectClass classify(std::string_view name, bool section) {
    ObjectClass out;
    if (name.empty())
        return out;

    if (helix::is_extruder_name(name)) {
        out.kind = ObjectKind::Heater;
        return out;
    }

    static constexpr std::string_view kOutputPin = "output_pin ";
    if (starts_with(name, kOutputPin)) {
        out.instance = std::string(name.substr(kOutputPin.size()));
        out.kind = output_pin_kind(out.instance);
        return out;
    }

    for (const auto& chip : helix::sensors::humidity_sensor_chips()) {
        if (starts_with(name, chip.klipper_prefix)) {
            out.kind = ObjectKind::HumidityChip;
            out.instance = std::string(name.substr(chip.klipper_prefix.size()));
            out.chip = static_cast<uint8_t>(chip.type);
            return out;
        }
    }

    for (const auto& row : kRows) {
        if (!section && row.scope == Scope::SectionOnly)
            continue;
        std::string_view instance;
        if (row_matches(row, name, instance)) {
            out.kind = row.kind;
            out.instance = std::string(instance);
            out.chip = row.chip;
            return out;
        }
    }
    return out;
}

bool is_beacon_chip_name(const nlohmann::json& v) {
    if (!v.is_string())
        return false;
    const std::string& chip = v.get_ref<const std::string&>();
    return chip == "beacon" || chip.rfind("beacon ", 0) == 0;
}

bool beacon_has_accelerometer(const nlohmann::json& body) {
    return body.is_object() && (body.contains("accel_scale") || body.contains("accel_axes_map"));
}

bool resonance_tester_names_beacon(const nlohmann::json& body) {
    if (!body.is_object())
        return false;
    for (const char* field : {"accel_chip", "accel_chip_x", "accel_chip_y"}) {
        const auto it = body.find(field);
        if (it != body.end() && is_beacon_chip_name(*it))
            return true;
    }
    return false;
}

const std::vector<std::string> kNone;
const std::vector<std::string> kHeater = {"temperature", "target", "power"};
const std::vector<std::string> kTemperatureHumidity = {"temperature", "humidity"};
const std::vector<std::string> kTemperatureFan = {"temperature", "target", "speed"};
const std::vector<std::string> kSpeed = {"speed"};
const std::vector<std::string> kValue = {"value"};
const std::vector<std::string> kColorData = {"color_data"};
const std::vector<std::string> kEnabled = {"enabled"};
const std::vector<std::string> kFanFeedback = {
    "fan0_speed", "fan1_speed", "fan2_speed", "fan3_speed", "fan4_speed",
    "fan5_speed", "fan6_speed", "fan7_speed", "fan8_speed", "fan9_speed"};
const std::vector<std::string> kFilamentSensor = {"filament_detected", "enabled",
                                                  "detection_count"};
const std::vector<std::string> kWidth = {"Diameter", "Raw"};
const std::vector<std::string> kForce = {"force_g"};
const std::vector<std::string> kProbe = {"last_query", "last_z_result", "z_offset"};
const std::vector<std::string> kState = {"state"};
const std::vector<std::string> kWhole = {"*"};
const std::vector<std::string> kMcu = {"last_stats"};

} // namespace

ObjectClass classify_object(std::string_view klipper_object) {
    return classify(klipper_object, false);
}

ObjectClass classify_section(std::string_view section, const nlohmann::json& body) {
    if (section == "beacon" && beacon_has_accelerometer(body)) {
        ObjectClass out;
        out.kind = ObjectKind::Accelerometer;
        out.instance = "beacon";
        out.chip = chip_of(AccelSensorType::LIS2DW);
        return out;
    }
    if (section == "resonance_tester") {
        ObjectClass out;
        if (resonance_tester_names_beacon(body)) {
            out.kind = ObjectKind::Accelerometer;
            out.instance = "beacon";
            out.chip = chip_of(AccelSensorType::LIS2DW);
        }
        return out;
    }
    return classify(section, true);
}

const std::vector<std::string>& status_fields(ObjectKind kind) {
    switch (kind) {
    case ObjectKind::Heater:
    case ObjectKind::GenericHeater:
        return kHeater;
    case ObjectKind::TemperatureSensor:
    case ObjectKind::HumidityChip:
    case ObjectKind::DriverTemperature:
        return kTemperatureHumidity;
    case ObjectKind::TemperatureFan:
        return kTemperatureFan;
    case ObjectKind::Fan:
    case ObjectKind::HeaterFan:
    case ObjectKind::ControllerFan:
    case ObjectKind::GenericFan:
        return kSpeed;
    case ObjectKind::OutputPinFan:
    case ObjectKind::OutputPinLed:
        return kValue;
    case ObjectKind::Led:
        return kColorData;
    case ObjectKind::LedEffect:
        return kEnabled;
    case ObjectKind::FanFeedback:
        return kFanFeedback;
    case ObjectKind::FilamentSwitch:
    case ObjectKind::FilamentMotion:
    case ObjectKind::BackendFilamentSensor:
        return kFilamentSensor;
    case ObjectKind::WidthSensor:
        return kWidth;
    case ObjectKind::LoadCell:
        return kForce;
    case ObjectKind::Probe:
        return kProbe;
    case ObjectKind::Input:
        return kState;
    case ObjectKind::PowerLossMonitor:
        return kWhole;
    case ObjectKind::Mcu:
        return kMcu;
    case ObjectKind::Unknown:
    case ObjectKind::Stepper:
    case ObjectKind::Speaker:
    case ObjectKind::Accelerometer:
        return kNone;
    }
    return kNone;
}

bool is_heater(ObjectKind kind) {
    return kind == ObjectKind::Heater || kind == ObjectKind::GenericHeater;
}

bool is_fan(ObjectKind kind) {
    switch (kind) {
    case ObjectKind::Fan:
    case ObjectKind::HeaterFan:
    case ObjectKind::ControllerFan:
    case ObjectKind::GenericFan:
    case ObjectKind::OutputPinFan:
    case ObjectKind::TemperatureFan:
        return true;
    default:
        return false;
    }
}

bool is_led(ObjectKind kind) {
    return kind == ObjectKind::Led || kind == ObjectKind::OutputPinLed;
}

bool is_temperature(ObjectKind kind) {
    switch (kind) {
    case ObjectKind::TemperatureSensor:
    case ObjectKind::TemperatureFan:
    case ObjectKind::GenericHeater:
    case ObjectKind::DriverTemperature:
    case ObjectKind::HumidityChip:
        return true;
    default:
        return false;
    }
}

bool is_filament_sensor(ObjectKind kind) {
    return kind == ObjectKind::FilamentSwitch || kind == ObjectKind::FilamentMotion ||
           kind == ObjectKind::BackendFilamentSensor;
}

std::string_view chip_name(ObjectKind kind, uint8_t chip) {
    switch (kind) {
    case ObjectKind::HumidityChip:
        for (const auto& c : helix::sensors::humidity_sensor_chips()) {
            if (static_cast<uint8_t>(c.type) == chip)
                return c.display_name;
        }
        break;
    case ObjectKind::Probe:
        for (const auto& e : helix::sensors::kProbeSensorTypes) {
            if (static_cast<uint8_t>(e.value) == chip)
                return e.display;
        }
        break;
    case ObjectKind::Accelerometer:
        for (const auto& e : helix::sensors::kAccelSensorTypes) {
            if (static_cast<uint8_t>(e.value) == chip)
                return e.display;
        }
        break;
    default:
        break;
    }
    return {};
}

} // namespace helix::hw
