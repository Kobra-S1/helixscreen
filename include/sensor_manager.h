// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "hardware_object_table.h"
#include "lvgl.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class PrinterDiscovery;
}

namespace helix::sensors {

/// One discovered sensor, as every manager sees it.
struct SensorEntry {
    std::string klipper_name; ///< "temperature_sensor chamber"
    std::string display_name; ///< "chamber"
    uint8_t type = 0;         ///< index into the manager's TypeSpec table
    uint8_t role = 0;         ///< the manager's role enum value; 0 is always "none"
    bool enabled = true;
    bool available = false;
};

/// One role a manager has.
struct RoleSpec {
    uint8_t id;              ///< the manager's role enum value
    const char* config_name; ///< persisted string, unchanged from today
    const char* label_key;   ///< translation key for the Settings dropdown
    bool exclusive;          ///< at most one holder
    bool offered;            ///< listed as a dropdown option
};

/// One sensor type a manager knows.
struct TypeSpec {
    uint8_t id;
    const char* config_name; ///< persisted "type" string, unchanged from today
    const char* label_key;
};

enum class RowToggle : uint8_t { None, Enabled, Invert };

/// What one Settings row shows. Plain data: no LVGL.
struct SensorRowView {
    std::string name;
    std::string detail;                   ///< type label or a static reading
    std::string state;                    ///< live state text; empty hides it
    bool alert = false;                   ///< state drawn in the warning style
    std::vector<std::string> role_labels; ///< offered roles, translated; empty hides the dropdown
    int selected_role = -1;      ///< index into role_labels, -1 when the held role is not offered
    std::string held_role_label; ///< shown when selected_role is -1 and a role is held
    RowToggle toggle = RowToggle::None;
    bool toggle_value = false;
};

/**
 * @brief The base of every sensor manager.
 *
 * Owns the sensor list, the lifecycle, threading, sync mode, the count and
 * revision subjects, reconcile against discovery, role assignment, settings
 * persistence and the Settings row model. A derived manager holds only what is
 * its own: its typed readings, its subjects and its domain rules.
 *
 * Threading: discover(), load_settings(), the setters and every subject touch
 * run on the main thread. update_from_status() may run on any thread; it
 * parses under the mutex and defers refresh_subjects() to the main thread.
 * refresh_subjects() runs WITHOUT the mutex: it copies what it needs under a
 * short lock, then sets subjects, because observers can call back into
 * managers and AmsState.
 */
class SensorManager {
  public:
    virtual ~SensorManager();
    SensorManager(const SensorManager&) = delete;
    SensorManager& operator=(const SensorManager&) = delete;

    [[nodiscard]] const char* name() const;

    void init_subjects();
    void deinit_subjects();
    [[nodiscard]] SubjectLifetime subjects_lifetime() const;
    void set_sync_mode(bool enabled);

    /// Reconcile against hw.objects_of(kinds) and hw's classified config
    /// sections, then on_discovered(). No-op when the manager is not fed by
    /// PrinterDiscovery.
    void discover(const PrinterDiscovery& hw);

    /// Default: lock, parse_status(), unlock, publish() if it changed anything.
    virtual void update_from_status(const nlohmann::json& status);

    /// Subscriptions this manager owns beyond hw::status_fields() of its kinds, in
    /// printer.objects.subscribe shape: {object: [fields...] or null}. Probe owns
    /// every probe subscription here (per-type object and fields).
    [[nodiscard]] virtual nlohmann::json extra_subscriptions(const PrinterDiscovery& hw) const;

    void load_settings(); ///< no-op when the manager has no config key
    void save_settings();

    [[nodiscard]] size_t sensor_count() const;
    [[nodiscard]] std::vector<SensorEntry> entries() const;
    [[nodiscard]] bool find(std::string_view klipper_name, SensorEntry& out) const;

    /// Exclusive roles are taken from every other holder.
    virtual void set_role(std::string_view klipper_name, uint8_t role);
    void set_enabled(std::string_view klipper_name, bool enabled);
    virtual void set_option(std::string_view klipper_name, bool value);

    /// Role ids in dropdown order (offered roles only).
    [[nodiscard]] std::vector<uint8_t> offered_roles() const;
    [[nodiscard]] virtual SensorRowView row(size_t index) const;

    [[nodiscard]] lv_subject_t* count_subject();
    [[nodiscard]] lv_subject_t* revision_subject(); ///< bumped on every publish()

  protected:
    struct Options {
        const char* name;
        std::vector<hw::ObjectKind> kinds;
        const char* count_subject;
        const char* revision_subject;
        const char* config_key = nullptr; ///< nullptr: not persisted
        std::vector<RoleSpec> roles;
        std::vector<TypeSpec> types;
        bool keep_missing_state = false;
        bool from_printer_discovery = true;
    };
    explicit SensorManager(Options options);

    virtual void register_subjects(SubjectManager& subjects) = 0; ///< main thread
    virtual void refresh_subjects() = 0;                          ///< main thread, mutex NOT held
    virtual bool parse_status(const nlohmann::json& status) = 0;  ///< mutex held
    virtual void on_discovered() {}                               ///< mutex held; auto roles
    virtual void on_entries_changed() {} ///< mutex held; resize parallel readings
    virtual void read_entry_extra(SensorEntry& /*entry*/, const nlohmann::json& /*saved*/) {}
    virtual void write_entry_extra(const SensorEntry& /*entry*/, nlohmann::json& /*out*/) const {}
    virtual void read_extra_settings(const nlohmann::json& /*saved*/) {}
    virtual void write_extra_settings(nlohmann::json& /*out*/) const {}
    /// Mutex held, lifetime already invalidated: tear down subjects kept outside
    /// the SubjectManager (a DynamicSubjectMap's clear()).
    virtual void on_deinit_subjects() {}

    /// Sync mode: refresh now. Otherwise defer refresh_subjects() through the
    /// lifetime token; dropped once deinit_subjects() has run.
    void publish();

    /// Replace the entries with `discovered`, keeping roles/enabled of names
    /// already known only through load_settings(). For non-PrinterDiscovery
    /// managers (SensorState).
    void reconcile(std::vector<SensorEntry> discovered);

    [[nodiscard]] std::recursive_mutex& mutex() const;
    [[nodiscard]] std::vector<SensorEntry>& entries_locked();
    [[nodiscard]] const std::vector<SensorEntry>& entries_locked() const;
    [[nodiscard]] const Options& options() const;
    [[nodiscard]] bool subjects_initialized() const;

  private:
    friend class SensorManagerTestAccess;

    /// Main thread: sync the count, bump the revision, then refresh_subjects().
    void refresh_on_main_thread();
    /// Mutex held: swap in `found`, carrying role/enabled over from known names.
    void replace_entries_locked(std::vector<SensorEntry> found);
    [[nodiscard]] const RoleSpec* role_spec(uint8_t id) const;
    [[nodiscard]] const char* role_config_name(uint8_t id) const;
    [[nodiscard]] uint8_t role_from_config_name(std::string_view name) const;
    [[nodiscard]] const TypeSpec* type_spec(uint8_t id) const;
    [[nodiscard]] nlohmann::json settings_json_locked() const;
    void apply_settings_locked(const nlohmann::json& saved);

    Options options_;
    mutable std::recursive_mutex mutex_;
    std::vector<SensorEntry> entries_;
    SubjectManager subjects_;
    lv_subject_t count_{};
    lv_subject_t revision_{};
    bool subjects_initialized_ = false;
    bool sync_mode_ = false;
    bool deinit_registered_ = false;
    helix::AsyncLifetimeGuard lifetime_;
};

/// Every sensor manager, in a stable order. The one list.
[[nodiscard]] const std::vector<SensorManager*>& sensor_managers();

/// Keeps a manager's role enum readable at the boundary.
template <typename Role> [[nodiscard]] Role role_of(const SensorEntry& entry) {
    return static_cast<Role>(entry.role);
}

} // namespace helix::sensors
