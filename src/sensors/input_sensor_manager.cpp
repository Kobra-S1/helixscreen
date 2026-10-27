// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_sensor_manager.h"

#include "ui_toast_manager.h"

#include "app_globals.h"
#include "config.h"
#include "json_utils.h"
#include "print_lifecycle_state.h"
#include "print_start_checks.h"
#include "printer_state.h"
#include "spdlog/fmt/fmt.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// Status arrives on libhv's thread; subject updates are deferred to the main
// thread through lifetime_.token().defer() and dropped once deinit_subjects() runs.

namespace helix::sensors {

namespace {

constexpr const char* kGcodeButtonPrefix = "gcode_button ";
constexpr const char* kConfigKey = "input_sensors";

bool contains_ci(std::string_view haystack, std::string_view needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) ==
                                  std::tolower(static_cast<unsigned char>(b));
                       }) != haystack.end();
}

/// The words a row shows for an input's state: door wording for a Door input,
/// switch wording for any other.
const char* state_text(const InputSensorConfig& config, const InputSensorState* state) {
    if (!state || !state->reported) {
        return "--";
    }
    if (config.role == InputSensorRole::DOOR) {
        return input_reads_open(config, *state) ? lv_tr("Opened") : lv_tr("Closed");
    }
    return state->pressed ? lv_tr("Pressed") : lv_tr("Released");
}

} // namespace

InputSensorRole default_input_role(std::string_view sensor_name) {
    for (const char* word : {"door", "lid", "cover"}) {
        if (contains_ci(sensor_name, word)) {
            return InputSensorRole::DOOR;
        }
    }
    return InputSensorRole::NONE;
}

std::vector<std::string> doors_newly_open(const std::vector<std::string>& before,
                                          const std::vector<std::string>& now) {
    std::vector<std::string> opened;
    for (const auto& name : now) {
        if (std::find(before.begin(), before.end(), name) == before.end()) {
            opened.push_back(name);
        }
    }
    return opened;
}

bool firmware_reacts_to_opening(const InputConfigReaction& reaction, bool inverted) {
    return inverted ? reaction.on_press : reaction.on_release;
}

InputConfigReaction input_config_reaction(const nlohmann::json& section) {
    const auto acts = [&section](const char* key) {
        const auto it = section.find(key);
        if (it == section.end() || !it->is_string()) {
            return false;
        }
        const auto& text = it->get_ref<const std::string&>();
        return std::any_of(text.begin(), text.end(),
                           [](char c) { return !std::isspace(static_cast<unsigned char>(c)); });
    };
    return {acts("press_gcode"), acts("release_gcode")};
}

bool input_reads_open(const InputSensorConfig& config, const InputSensorState& state) {
    if (config.role != InputSensorRole::DOOR || !state.reported) {
        return false;
    }
    // A door switch is held pressed by the closed door.
    return state.pressed == config.inverted;
}

// ============================================================================
// Singleton
// ============================================================================

InputSensorManager& InputSensorManager::instance() {
    static InputSensorManager instance;
    return instance;
}

InputSensorManager::InputSensorManager() = default;

InputSensorManager::~InputSensorManager() = default;

// ============================================================================
// Discovery, Status and Config
// ============================================================================

std::vector<InputSensorConfig>
InputSensorManager::inputs_in(const std::vector<std::string>& klipper_objects) {
    const std::string prefix = kGcodeButtonPrefix;
    std::vector<InputSensorConfig> inputs;
    for (const auto& name : klipper_objects) {
        if (name.size() > prefix.size() && name.rfind(prefix, 0) == 0) {
            InputSensorConfig config(name, name.substr(prefix.size()),
                                     InputSensorType::GCODE_BUTTON);
            config.role = default_input_role(config.sensor_name);
            inputs.push_back(std::move(config));
        }
    }
    return inputs;
}

nlohmann::json
InputSensorManager::required_status_objects(const std::vector<std::string>& klipper_objects) {
    nlohmann::json objects = nlohmann::json::object();
    for (const auto& input : inputs_in(klipper_objects)) {
        objects[input.klipper_name] = nlohmann::json::array({"state"});
    }
    return objects;
}

void InputSensorManager::discover(const std::vector<std::string>& klipper_objects) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sensors_.reconcile(inputs_in(klipper_objects));
    // A state carried over from an earlier discovery stays reported; a new
    // input starts unreported until its first status frame.
    spdlog::info("[InputSensorManager] Discovered {} inputs", sensors_.size());
    for (const auto& s : sensors_) {
        spdlog::debug("[InputSensorManager]   {} (role: {})", s.klipper_name,
                      input_role_to_string(s.role));
    }
    update_subjects();
}

void InputSensorManager::update_from_status(const nlohmann::json& status) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    bool any_changed = false;
    for (const auto& sensor : sensors_) {
        const auto it = status.find(sensor.klipper_name);
        if (it == status.end() || !it->is_object()) {
            continue;
        }
        const auto field = it->find("state");
        if (field == it->end() || !field->is_string()) {
            continue;
        }
        const std::string value = field->get<std::string>();
        bool pressed = false;
        if (value == "PRESSED") {
            pressed = true;
        } else if (value != "RELEASED") {
            continue;
        }
        auto& state = sensors_.state_at(sensor.klipper_name);
        if (!state.reported || state.pressed != pressed) {
            state.pressed = pressed;
            state.reported = true;
            any_changed = true;
            spdlog::debug("[InputSensorManager] {} -> {}", sensor.klipper_name, value);
        }
    }

    if (!any_changed) {
        return;
    }
    if (sync_mode_) {
        update_subjects();
    } else {
        lifetime_.token().defer("InputSensorManager::update_from_status", [] {
            InputSensorManager::instance().update_subjects_on_main_thread();
        });
    }
}

void InputSensorManager::discover_from_config(const nlohmann::json& config_keys) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    config_reactions_.clear();
    if (!config_keys.is_object()) {
        return;
    }
    const std::string prefix = kGcodeButtonPrefix;
    for (auto it = config_keys.begin(); it != config_keys.end(); ++it) {
        if (it.key().rfind(prefix, 0) == 0 && it->is_object()) {
            const auto reaction = input_config_reaction(*it);
            config_reactions_[it.key()] = reaction;
            spdlog::debug("[InputSensorManager] {} config acts on press={} release={}", it.key(),
                          reaction.on_press, reaction.on_release);
        }
    }
}

bool InputSensorManager::firmware_reacts_to_opening(const std::string& klipper_name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto* sensor = sensors_.find(klipper_name);
    const auto it = config_reactions_.find(klipper_name);
    if (!sensor || it == config_reactions_.end()) {
        return false;
    }
    return sensors::firmware_reacts_to_opening(it->second, sensor->inverted);
}

std::vector<std::string>
InputSensorManager::mid_print_door_warnings(const std::vector<std::string>& newly_open,
                                            const std::vector<std::string>& print_materials) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> warn;
    if (!door_warning_material(door_check_enabled(), !newly_open.empty(), print_materials)) {
        return warn;
    }
    for (const auto& name : newly_open) {
        const auto* sensor = sensors_.find(name);
        if (!sensor) {
            continue;
        }
        if (firmware_reacts_to_opening(name)) {
            spdlog::debug("[InputSensorManager] {} opened; the printer announces it", name);
            continue;
        }
        warn.push_back(sensor->sensor_name);
    }
    return warn;
}

void InputSensorManager::load_config(const nlohmann::json& config) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sensors_.apply_json(config, input_role_from_string);
    if (const auto sensors = config.find("sensors");
        sensors != config.end() && sensors->is_array()) {
        for (const auto& entry : *sensors) {
            const auto name = entry.find("klipper_name");
            const auto inverted = entry.find("inverted");
            if (name == entry.end() || inverted == entry.end()) {
                continue;
            }
            if (auto* sensor = sensors_.find(json_util::as_string(*name))) {
                sensor->inverted = json_util::as_bool(*inverted, sensor->inverted);
            }
        }
    }
    door_check_.reset();
    if (const auto check = config.find("door_check");
        check != config.end() && check->is_boolean()) {
        door_check_ = check->get<bool>();
    }
    update_subjects();
}

nlohmann::json InputSensorManager::save_config() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto config = sensors_.to_json(input_role_to_string, input_type_to_string);
    auto& entries = config["sensors"];
    for (size_t i = 0; i < entries.size() && i < sensors_.size(); ++i) {
        entries[i]["inverted"] = sensors_.configs()[i].inverted;
    }
    if (door_check_) {
        config["door_check"] = *door_check_;
    }
    return config;
}

void InputSensorManager::load_config_from_file() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Config* cfg = Config::get_instance();
    if (const json* saved = cfg->try_get_json(cfg->df() + kConfigKey)) {
        load_config(*saved);
    } else {
        door_check_.reset();
        update_subjects();
    }
}

void InputSensorManager::save_config_to_file() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Config* cfg = Config::get_instance();
    cfg->get_json(cfg->df() + kConfigKey) = save_config();
    cfg->save();
}

// ============================================================================
// Initialization
// ============================================================================

void InputSensorManager::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    UI_MANAGED_SUBJECT_INT(sensor_count_, 0, "input_sensor_count", subjects_);
    UI_MANAGED_SUBJECT_INT(door_check_subject_, 0, "input_door_check", subjects_);
    for (size_t i = 0; i < rows_.size(); ++i) {
        auto& row = rows_[i];
        row.name_id = fmt::format("input_sensor_name_{}", i);
        row.state_id = fmt::format("input_sensor_state_{}", i);
        row.alert_id = fmt::format("input_sensor_alert_{}", i);
        row.role_id = fmt::format("input_sensor_role_{}", i);
        row.inverted_id = fmt::format("input_sensor_inverted_{}", i);
        UI_MANAGED_SUBJECT_STRING(row.name, row.name_buf, "", row.name_id.c_str(), subjects_);
        UI_MANAGED_SUBJECT_STRING(row.state, row.state_buf, "", row.state_id.c_str(), subjects_);
        UI_MANAGED_SUBJECT_INT(row.alert, 0, row.alert_id.c_str(), subjects_);
        UI_MANAGED_SUBJECT_INT(row.role, 0, row.role_id.c_str(), subjects_);
        UI_MANAGED_SUBJECT_INT(row.inverted, 0, row.inverted_id.c_str(), subjects_);
    }

    subjects_initialized_ = true;

    StaticSubjectRegistry::instance().register_deinit(
        "InputSensorManager", []() { InputSensorManager::instance().deinit_subjects(); });

    update_subjects();
}

void InputSensorManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    lifetime_.invalidate();
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

// ============================================================================
// Queries
// ============================================================================

bool InputSensorManager::has_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return !sensors_.empty();
}

size_t InputSensorManager::sensor_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_.size();
}

std::vector<InputSensorConfig> InputSensorManager::get_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_.configs();
}

std::optional<InputSensorState>
InputSensorManager::get_state(const std::string& klipper_name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto* state = sensors_.state(klipper_name);
    return state ? std::optional(*state) : std::nullopt;
}

bool InputSensorManager::has_door_input() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_.find_by_role(InputSensorRole::DOOR) != nullptr;
}

std::vector<std::string> InputSensorManager::open_doors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> open;
    for (const auto& sensor : sensors_) {
        const auto* state = sensors_.state(sensor.klipper_name);
        if (state && state->available && input_reads_open(sensor, *state)) {
            open.push_back(sensor.sensor_name);
        }
    }
    return open;
}

bool InputSensorManager::door_check_enabled() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return door_check_.value_or(sensors_.find_by_role(InputSensorRole::DOOR) != nullptr);
}

std::string InputSensorManager::klipper_name_at(size_t row) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return row < sensors_.size() ? sensors_.configs()[row].klipper_name : std::string{};
}

// ============================================================================
// Configuration
// ============================================================================

void InputSensorManager::set_sensor_role(const std::string& klipper_name, InputSensorRole role) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Door is not exclusive: a printer may have a front door and a top cover.
    if (auto* sensor = sensors_.find(klipper_name)) {
        sensor->role = role;
        spdlog::info("[InputSensorManager] Role for {} set to {}", klipper_name,
                     input_role_to_string(role));
        update_subjects();
    }
}

void InputSensorManager::set_sensor_inverted(const std::string& klipper_name, bool inverted) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (auto* sensor = sensors_.find(klipper_name)) {
        sensor->inverted = inverted;
        spdlog::info("[InputSensorManager] {} inverted: {}", klipper_name, inverted);
        update_subjects();
    }
}

void InputSensorManager::set_door_check_enabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    door_check_ = enabled;
    spdlog::info("[InputSensorManager] Door check {}", enabled ? "on" : "off");
    update_subjects();
}

void InputSensorManager::set_sync_mode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sync_mode_ = enabled;
}

void InputSensorManager::update_subjects_on_main_thread() {
    update_subjects();
}

// ============================================================================
// Private
// ============================================================================

void InputSensorManager::update_subjects() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!subjects_initialized_) {
        return;
    }

    const auto& configs = sensors_.configs();
    for (size_t i = 0; i < rows_.size(); ++i) {
        auto& row = rows_[i];
        if (i >= configs.size()) {
            continue;
        }
        const auto& config = configs[i];
        const auto* state = sensors_.state(config.klipper_name);
        const char* text = state_text(config, state);
        if (std::strcmp(lv_subject_get_string(&row.name), config.sensor_name.c_str()) != 0) {
            lv_subject_copy_string(&row.name, config.sensor_name.c_str());
        }
        if (std::strcmp(lv_subject_get_string(&row.state), text) != 0) {
            lv_subject_copy_string(&row.state, text);
        }
        const int alert = state && input_reads_open(config, *state) ? 1 : 0;
        if (lv_subject_get_int(&row.alert) != alert) {
            lv_subject_set_int(&row.alert, alert);
        }
        const int role = static_cast<int>(config.role);
        if (lv_subject_get_int(&row.role) != role) {
            lv_subject_set_int(&row.role, role);
        }
        const int inverted = config.inverted ? 1 : 0;
        if (lv_subject_get_int(&row.inverted) != inverted) {
            lv_subject_set_int(&row.inverted, inverted);
        }
    }

    // Rows are filled before the count, so a rebuild triggered by the count
    // binds to filled subjects.
    const int count = static_cast<int>(std::min(configs.size(), rows_.size()));
    if (lv_subject_get_int(&sensor_count_) != count) {
        lv_subject_set_int(&sensor_count_, count);
    }
    const int check = door_check_enabled() ? 1 : 0;
    if (lv_subject_get_int(&door_check_subject_) != check) {
        lv_subject_set_int(&door_check_subject_, check);
    }

    // A door opening mid-print gets one toast per opening (rules in
    // mid_print_door_warnings()).
    std::vector<std::string> open;
    for (const auto& sensor : sensors_) {
        const auto* state = sensors_.state(sensor.klipper_name);
        if (state && state->available && input_reads_open(sensor, *state)) {
            open.push_back(sensor.klipper_name);
        }
    }
    const bool printing =
        get_printer_state().print_state().get_print_lifecycle() == PrintState::Printing;
    if (printing) {
        for (const auto& name :
             mid_print_door_warnings(doors_newly_open(open_doors_seen_, open),
                                     get_printer_state().print_state().print_materials())) {
            const std::string msg =
                fmt::format(fmt::runtime(lv_tr("Door opened during print: {}")), name);
            ToastManager::instance().show(ToastSeverity::WARNING, msg.c_str(), 6000);
        }
    }
    open_doors_seen_ = std::move(open);
}

} // namespace helix::sensors
