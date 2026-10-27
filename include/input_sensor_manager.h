// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "input_sensor_types.h"
#include "lvgl.h"
#include "sensor_collection.h"
#include "subject_managed_panel.h"

#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix::sensors {

/// The edges a gcode_button's configfile.config section acts on.
[[nodiscard]] InputConfigReaction input_config_reaction(const nlohmann::json& section);

/**
 * @brief Manager for on/off inputs: door switches, buttons, dock sensors.
 *
 * Discovers every Klipper `gcode_button`, tracks its live state, and gives each
 * a role. The role says what an input means to generic consumers; today the one
 * consumer is the door-open print-start gate, which reads the Door inputs. A
 * name containing "door", "lid" or "cover" starts as Door, everything else as
 * Other, and the user's choice is persisted per printer under "input_sensors".
 *
 * Inputs whose meaning belongs to a subsystem (a tool changer's dock and
 * carriage buttons) are listed here as plain inputs; that subsystem keeps
 * owning what they mean.
 *
 * The settings overlay draws one row per input from the indexed subjects
 * input_sensor_{name,state,alert,role,inverted}_<i>, i < kMaxRows, in
 * discovery order; input_sensor_count says how many rows exist.
 *
 * Status JSON format:
 * @code
 * { "gcode_button frontDoor": { "state": "PRESSED" } }
 * @endcode
 */
class InputSensorManager {
  public:
    /// Rows the settings overlay can show. Inputs past this still count for
    /// the door gate; they only go unlisted.
    static constexpr size_t kMaxRows = 16;

    static InputSensorManager& instance();

    InputSensorManager(const InputSensorManager&) = delete;
    InputSensorManager& operator=(const InputSensorManager&) = delete;

    // ---- discovery, status, config ----

    /// @note MUST be called from main LVGL thread (updates subjects directly)
    void discover(const std::vector<std::string>& klipper_objects);

    /// The inputs among @p klipper_objects, in order, with default roles.
    [[nodiscard]] static std::vector<InputSensorConfig>
    inputs_in(const std::vector<std::string>& klipper_objects);

    /// The status objects (and fields) this manager reads, for the subscription.
    [[nodiscard]] static nlohmann::json
    required_status_objects(const std::vector<std::string>& klipper_objects);

    void update_from_status(const nlohmann::json& status);

    /// Reads which inputs the printer's config already acts on (configfile.config).
    void discover_from_config(const nlohmann::json& config_keys);

    /// Whether the printer's own config reacts when this Door input opens. The
    /// mid-print warning skips such a door rather than repeat the printer's own.
    [[nodiscard]] bool firmware_reacts_to_opening(const std::string& klipper_name) const;

    /// @note MUST be called from main LVGL thread (updates subjects directly)
    void load_config(const nlohmann::json& config);
    [[nodiscard]] nlohmann::json save_config() const;
    void load_config_from_file();
    void save_config_to_file();

    // ---- subjects ----

    void init_subjects();
    void deinit_subjects();

    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    // ---- queries ----

    [[nodiscard]] bool has_sensors() const;
    [[nodiscard]] size_t sensor_count() const;
    [[nodiscard]] std::vector<InputSensorConfig> get_sensors() const;
    [[nodiscard]] std::optional<InputSensorState> get_state(const std::string& klipper_name) const;

    /// Whether any input holds the Door role.
    [[nodiscard]] bool has_door_input() const;

    /// Short names of the Door inputs that read open, in discovery order.
    [[nodiscard]] std::vector<std::string> open_doors() const;

    /// Whether the door-open print-start check runs. Until the user sets it, on
    /// exactly when the printer has a Door input.
    [[nodiscard]] bool door_check_enabled() const;

    // ---- configuration ----

    /// @note MUST be called from main LVGL thread (updates subjects directly)
    void set_sensor_role(const std::string& klipper_name, InputSensorRole role);
    void set_sensor_inverted(const std::string& klipper_name, bool inverted);
    void set_door_check_enabled(bool enabled);

    /// The input shown in overlay row @p row; empty when there is none.
    [[nodiscard]] std::string klipper_name_at(size_t row) const;

    /// Test mode: update_from_status() updates subjects synchronously.
    void set_sync_mode(bool enabled);

    void update_subjects_on_main_thread();

    friend class InputSensorManagerTestAccess;

  private:
    InputSensorManager();
    ~InputSensorManager();

    /// @note Main LVGL thread only
    void update_subjects();

    mutable std::recursive_mutex mutex_;
    SensorCollection<InputSensorConfig, InputSensorState> sensors_;
    /// The user's door-check choice; nullopt follows has_door_input().
    std::optional<bool> door_check_;
    /// Klipper names of the Door inputs that read open at the last subject
    /// update, for the one-toast-per-opening rule.
    std::vector<std::string> open_doors_seen_;
    /// Per gcode_button klipper name, the edges its config acts on.
    std::map<std::string, InputConfigReaction> config_reactions_;
    bool sync_mode_ = false;

    bool subjects_initialized_ = false;
    SubjectManager subjects_;
    helix::AsyncLifetimeGuard lifetime_;
    lv_subject_t sensor_count_{};
    lv_subject_t door_check_subject_{};

    struct Row {
        std::string name_id, state_id, alert_id, role_id, inverted_id;
        lv_subject_t name{}, state{}, alert{}, role{}, inverted{};
        char name_buf[64]{};
        char state_buf[32]{};
    };
    std::array<Row, kMaxRows> rows_{};
};

} // namespace helix::sensors
