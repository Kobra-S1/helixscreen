// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_sensor_manager.h"

#include "ui_toast_manager.h"

#include "app_globals.h"
#include "json_utils.h"
#include "print_lifecycle_state.h"
#include "print_start_checks.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "spdlog/fmt/fmt.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace helix::sensors {

namespace {

constexpr uint8_t kDoor = static_cast<uint8_t>(InputSensorRole::DOOR);

bool contains_ci(std::string_view haystack, std::string_view needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) ==
                                  std::tolower(static_cast<unsigned char>(b));
                       }) != haystack.end();
}

/// The words a row shows for an input's state: door wording for a Door input,
/// switch wording for any other.
const char* state_text(InputSensorRole role, const InputReading& reading) {
    if (!reading.reported) {
        return "--";
    }
    if (role == InputSensorRole::DOOR) {
        return input_reads_open(role, reading) ? lv_tr("Opened") : lv_tr("Closed");
    }
    return reading.pressed ? lv_tr("Pressed") : lv_tr("Released");
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

bool input_reads_open(InputSensorRole role, const InputReading& reading) {
    if (role != InputSensorRole::DOOR || !reading.reported) {
        return false;
    }
    // A door switch is held pressed by the closed door.
    return reading.pressed == reading.inverted;
}

InputSensorManager::Options InputSensorManager::make_options() {
    Options o;
    o.name = "InputSensorManager";
    o.kinds = {hw::ObjectKind::Input};
    o.count_subject = "input_sensor_total";
    o.revision_subject = "input_sensor_revision";
    o.config_key = "input_sensors";
    for (const auto& r : kInputSensorRoles) {
        // A door and a top cover can both be Doors, so no role is exclusive.
        o.roles.push_back({static_cast<uint8_t>(r.value), r.id, r.display, /*exclusive=*/false,
                           /*offered=*/r.value == InputSensorRole::DOOR});
    }
    o.types = {{0, "gcode_button", "Button"}};
    return o;
}

InputSensorManager& InputSensorManager::instance() {
    static InputSensorManager instance;
    return instance;
}

InputSensorManager::InputSensorManager() : SensorManager(make_options()) {}

// ============================================================================
// Discovery and status
// ============================================================================

void InputSensorManager::on_entries_changed() {
    for (auto it = readings_.begin(); it != readings_.end();) {
        const auto& name = it->first;
        const bool listed =
            std::any_of(entries_locked().begin(), entries_locked().end(),
                        [&](const SensorEntry& e) { return e.klipper_name == name; });
        it = listed ? std::next(it) : readings_.erase(it);
    }
}

void InputSensorManager::on_discovered(const PrinterDiscovery& hw) {
    std::set<std::string> now;
    config_reactions_.clear();
    for (auto& entry : entries_locked()) {
        now.insert(entry.klipper_name);
        if (known_.count(entry.klipper_name) == 0) {
            entry.role = static_cast<uint8_t>(default_input_role(entry.display_name));
        }
        const nlohmann::json* section = hw.section(entry.klipper_name);
        if (section != nullptr && section->is_object()) {
            const auto reaction = input_config_reaction(*section);
            config_reactions_[entry.klipper_name] = reaction;
            spdlog::debug("[InputSensorManager] {} config acts on press={} release={}",
                          entry.klipper_name, reaction.on_press, reaction.on_release);
        }
    }
    known_ = std::move(now);
    spdlog::info("[InputSensorManager] Discovered {} inputs", entries_locked().size());
}

bool InputSensorManager::parse_status(const nlohmann::json& status) {
    bool changed = false;
    for (const auto& entry : entries_locked()) {
        const auto it = status.find(entry.klipper_name);
        if (it == status.end() || !it->is_object()) {
            continue;
        }
        const auto field = it->find("state");
        if (field == it->end() || !field->is_string()) {
            continue;
        }
        const std::string value = json_util::as_string(*field);
        if (value != "PRESSED" && value != "RELEASED") {
            continue;
        }
        const bool pressed = value == "PRESSED";
        InputReading& reading = readings_[entry.klipper_name];
        if (!reading.reported || reading.pressed != pressed) {
            reading.pressed = pressed;
            reading.reported = true;
            changed = true;
            spdlog::debug("[InputSensorManager] {} -> {}", entry.klipper_name, value);
        }
    }
    status_frame_ = status_frame_ || changed;
    return changed;
}

// ============================================================================
// Reads
// ============================================================================

const InputReading& InputSensorManager::reading_locked(const std::string& klipper_name) const {
    static const InputReading kUnreported;
    const auto it = readings_.find(klipper_name);
    return it != readings_.end() ? it->second : kUnreported;
}

bool InputSensorManager::reads_open_locked(const SensorEntry& entry) const {
    return entry.available &&
           input_reads_open(role_of<InputSensorRole>(entry), reading_locked(entry.klipper_name));
}

bool InputSensorManager::has_door_locked() const {
    return std::any_of(entries_locked().begin(), entries_locked().end(),
                       [](const SensorEntry& e) { return e.role == kDoor; });
}

bool InputSensorManager::has_door_input() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    return has_door_locked();
}

std::vector<std::string> InputSensorManager::open_doors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    std::vector<std::string> open;
    for (const auto& entry : entries_locked()) {
        if (reads_open_locked(entry)) {
            open.push_back(entry.display_name);
        }
    }
    return open;
}

bool InputSensorManager::door_check_enabled() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    return door_check_.value_or(has_door_locked());
}

std::string InputSensorManager::klipper_name_at(size_t row) const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    return row < entries_locked().size() ? entries_locked()[row].klipper_name : std::string{};
}

bool InputSensorManager::firmware_reacts_to_opening(const std::string& klipper_name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    const auto entry =
        std::find_if(entries_locked().begin(), entries_locked().end(),
                     [&](const SensorEntry& e) { return e.klipper_name == klipper_name; });
    const auto it = config_reactions_.find(klipper_name);
    if (entry == entries_locked().end() || it == config_reactions_.end()) {
        return false;
    }
    return sensors::firmware_reacts_to_opening(it->second, reading_locked(klipper_name).inverted);
}

std::vector<std::string>
InputSensorManager::mid_print_door_warnings(const std::vector<std::string>& newly_open,
                                            const std::vector<std::string>& print_materials) const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    std::vector<std::string> warn;
    if (!door_warning_material(door_check_enabled(), !newly_open.empty(), print_materials)) {
        return warn;
    }
    for (const auto& name : newly_open) {
        const auto entry =
            std::find_if(entries_locked().begin(), entries_locked().end(),
                         [&](const SensorEntry& e) { return e.klipper_name == name; });
        if (entry == entries_locked().end()) {
            continue;
        }
        if (firmware_reacts_to_opening(name)) {
            spdlog::debug("[InputSensorManager] {} opened; the printer announces it", name);
            continue;
        }
        warn.push_back(entry->display_name);
    }
    return warn;
}

// ============================================================================
// Edits and settings
// ============================================================================

void InputSensorManager::set_option(std::string_view klipper_name, bool inverted) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        const auto entry =
            std::find_if(entries_locked().begin(), entries_locked().end(),
                         [&](const SensorEntry& e) { return e.klipper_name == klipper_name; });
        if (entry == entries_locked().end()) {
            return;
        }
        readings_[entry->klipper_name].inverted = inverted;
        spdlog::info("[InputSensorManager] {} inverted: {}", entry->klipper_name, inverted);
    }
    publish();
}

void InputSensorManager::set_door_check_enabled(bool enabled) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        door_check_ = enabled;
    }
    spdlog::info("[InputSensorManager] Door check {}", enabled ? "on" : "off");
    publish();
}

void InputSensorManager::read_entry_extra(SensorEntry& entry, const nlohmann::json& saved) {
    const auto inverted = saved.find("inverted");
    if (inverted != saved.end() && inverted->is_boolean()) {
        readings_[entry.klipper_name].inverted = json_util::as_bool(*inverted, false);
    }
}

void InputSensorManager::write_entry_extra(const SensorEntry& entry, nlohmann::json& out) const {
    out["inverted"] = reading_locked(entry.klipper_name).inverted;
}

void InputSensorManager::read_extra_settings(const nlohmann::json& saved) {
    door_check_.reset();
    const auto check = saved.find("door_check");
    if (check != saved.end() && check->is_boolean()) {
        door_check_ = json_util::as_bool(*check, false);
    }
}

void InputSensorManager::write_extra_settings(nlohmann::json& out) const {
    if (door_check_) {
        out["door_check"] = *door_check_;
    }
}

// ============================================================================
// Subjects
// ============================================================================

void InputSensorManager::register_subjects(SubjectManager& subjects) {
    UI_MANAGED_SUBJECT_INT(row_count_, 0, "input_sensor_count", subjects);
    UI_MANAGED_SUBJECT_INT(door_check_subject_, 0, "input_door_check", subjects);
    for (size_t i = 0; i < rows_.size(); ++i) {
        auto& row = rows_[i];
        row.name_id = fmt::format("input_sensor_name_{}", i);
        row.state_id = fmt::format("input_sensor_state_{}", i);
        row.alert_id = fmt::format("input_sensor_alert_{}", i);
        row.role_id = fmt::format("input_sensor_role_{}", i);
        row.inverted_id = fmt::format("input_sensor_inverted_{}", i);
        UI_MANAGED_SUBJECT_STRING(row.name, row.name_buf, "", row.name_id.c_str(), subjects);
        UI_MANAGED_SUBJECT_STRING(row.state, row.state_buf, "", row.state_id.c_str(), subjects);
        UI_MANAGED_SUBJECT_INT(row.alert, 0, row.alert_id.c_str(), subjects);
        UI_MANAGED_SUBJECT_INT(row.role, 0, row.role_id.c_str(), subjects);
        UI_MANAGED_SUBJECT_INT(row.inverted, 0, row.inverted_id.c_str(), subjects);
    }
}

void InputSensorManager::refresh_subjects() {
    struct RowView {
        std::string name;
        const char* state;
        int alert;
        int role;
        int inverted;
    };
    std::vector<RowView> views;
    std::vector<std::string> open;
    int door_check = 0;
    int row_count = 0;
    bool status_frame = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        for (const auto& entry : entries_locked()) {
            const auto role = role_of<InputSensorRole>(entry);
            const InputReading& reading = reading_locked(entry.klipper_name);
            const bool reads_open = reads_open_locked(entry);
            if (views.size() < rows_.size()) {
                views.push_back({entry.display_name, state_text(role, reading), reads_open ? 1 : 0,
                                 static_cast<int>(entry.role), reading.inverted ? 1 : 0});
            }
            if (reads_open) {
                open.push_back(entry.klipper_name);
            }
        }
        row_count = static_cast<int>(views.size());
        door_check = door_check_.value_or(has_door_locked()) ? 1 : 0;
        status_frame = std::exchange(status_frame_, false);
    }

    for (size_t i = 0; i < views.size(); ++i) {
        auto& row = rows_[i];
        const auto& view = views[i];
        if (std::strcmp(lv_subject_get_string(&row.name), view.name.c_str()) != 0) {
            lv_subject_copy_string(&row.name, view.name.c_str());
        }
        if (std::strcmp(lv_subject_get_string(&row.state), view.state) != 0) {
            lv_subject_copy_string(&row.state, view.state);
        }
        if (lv_subject_get_int(&row.alert) != view.alert) {
            lv_subject_set_int(&row.alert, view.alert);
        }
        if (lv_subject_get_int(&row.role) != view.role) {
            lv_subject_set_int(&row.role, view.role);
        }
        if (lv_subject_get_int(&row.inverted) != view.inverted) {
            lv_subject_set_int(&row.inverted, view.inverted);
        }
    }

    // Rows are filled before the count, so a rebuild triggered by the count
    // binds to filled subjects.
    if (lv_subject_get_int(&row_count_) != row_count) {
        lv_subject_set_int(&row_count_, row_count);
    }
    if (lv_subject_get_int(&door_check_subject_) != door_check) {
        lv_subject_set_int(&door_check_subject_, door_check);
    }

    // A door opening mid-print gets one toast per opening (rules in
    // mid_print_door_warnings()). Only a status frame opens a door: a role or
    // Invert change re-reads the same input and only re-seeds the open set.
    const bool printing =
        get_printer_state().print_state().get_print_lifecycle() == PrintState::Printing;
    if (printing && status_frame) {
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
