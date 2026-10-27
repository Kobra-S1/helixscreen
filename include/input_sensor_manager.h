// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "input_sensor_types.h"
#include "sensor_manager.h"

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
 * set_option() is the Invert switch.
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
class InputSensorManager : public SensorManager {
  public:
    /// Rows the settings overlay can show. Inputs past this still count for
    /// the door gate; they only go unlisted.
    static constexpr size_t kMaxRows = 16;

    static InputSensorManager& instance();

    /// Whether the printer's own config reacts when this Door input opens. The
    /// mid-print warning skips such a door rather than repeat the printer's own.
    [[nodiscard]] bool firmware_reacts_to_opening(const std::string& klipper_name) const;

    /// Short names of the doors in @p newly_open (klipper names) owed a warning
    /// while printing @p print_materials: the door check is on, the print has a
    /// material that needs a warm enclosure (door_warning_material()), and the
    /// printer's own config does not already announce that door opening.
    [[nodiscard]] std::vector<std::string>
    mid_print_door_warnings(const std::vector<std::string>& newly_open,
                            const std::vector<std::string>& print_materials) const;

    /// Whether any input holds the Door role.
    [[nodiscard]] bool has_door_input() const;

    /// Short names of the Door inputs that read open, in discovery order.
    [[nodiscard]] std::vector<std::string> open_doors() const;

    /// Whether the door-open print-start check runs. Until the user sets it, on
    /// exactly when the printer has a Door input.
    [[nodiscard]] bool door_check_enabled() const;
    void set_door_check_enabled(bool enabled);

    /// The input shown in overlay row @p row; empty when there is none.
    [[nodiscard]] std::string klipper_name_at(size_t row) const;

    /// Invert: a door switch then reads closed while released.
    void set_option(std::string_view klipper_name, bool inverted) override;

  protected:
    void register_subjects(SubjectManager& subjects) override;
    void refresh_subjects() override;
    bool parse_status(const nlohmann::json& status) override;
    void on_discovered(const PrinterDiscovery& hw) override;
    void on_entries_changed() override;
    void read_entry_extra(SensorEntry& entry, const nlohmann::json& saved) override;
    void write_entry_extra(const SensorEntry& entry, nlohmann::json& out) const override;
    void read_extra_settings(const nlohmann::json& saved) override;
    void write_extra_settings(nlohmann::json& out) const override;

  private:
    friend class InputSensorManagerTestAccess;

    InputSensorManager();
    static Options make_options();

    /// Mutex held: the reading for @p klipper_name, or an unreported one.
    [[nodiscard]] const InputReading& reading_locked(const std::string& klipper_name) const;
    /// Mutex held.
    [[nodiscard]] bool reads_open_locked(const SensorEntry& entry) const;
    [[nodiscard]] bool has_door_locked() const;

    std::map<std::string, InputReading> readings_; ///< by klipper_name
    /// The user's door-check choice; nullopt follows has_door_input().
    std::optional<bool> door_check_;
    /// Names present at the last discovery: only a name new to this set gets a
    /// default role, so a rediscovery never undoes the user's choice.
    std::set<std::string> known_;
    /// Per gcode_button klipper name, the edges its config acts on.
    std::map<std::string, InputConfigReaction> config_reactions_;
    /// A status frame changed an input since the last refresh: a door that now
    /// reads open was opened and may be owed a mid-print toast.
    bool status_frame_ = false;
    /// Klipper names of the Door inputs that read open at the last refresh, for
    /// the one-toast-per-opening rule. Main thread only.
    std::vector<std::string> open_doors_seen_;

    lv_subject_t row_count_{};
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
