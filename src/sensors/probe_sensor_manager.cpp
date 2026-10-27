// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "probe_sensor_manager.h"

#include "printer_discovery.h"
#include "spdlog/spdlog.h"
#include "text_io.h"

#include <algorithm>
#include <set>

namespace helix::sensors {

namespace {

constexpr uint8_t kZProbe = static_cast<uint8_t>(ProbeSensorRole::Z_PROBE);

ProbeSensorType type_of(const SensorEntry& entry) {
    return static_cast<ProbeSensorType>(entry.type);
}

// Where each probe type publishes the keys this manager reads, per upstream
// source (table in docs/devel/SENSOR_MANAGEMENT.md). The Cartographer plugin
// nests last_z_result per mode on its own object; the flat keys live on the
// probe object it registers.
std::string status_object(ProbeSensorType type, const std::string& klipper_name) {
    return type == ProbeSensorType::CARTOGRAPHER ? "probe" : klipper_name;
}

// last_query is the result of the last QUERY_PROBE. Beacon publishes none, and
// probe_eddy_current never sets it because it rejects QUERY_PROBE.
bool publishes_last_query(ProbeSensorType type) {
    return type != ProbeSensorType::BEACON && type != ProbeSensorType::EDDY_CURRENT;
}

// One physical probe can register several objects: Klipper's probe modules,
// Beacon and Cartographer all also register the generic probe object. Keep the
// most specific object only, so a single probe reads as one sensor and is
// subscribed once. An eddy object beside a named scanner is dropped too.
std::vector<bool> keep_most_specific(const std::vector<ProbeSensorType>& types) {
    const auto has_type = [&types](ProbeSensorType t) {
        return std::find(types.begin(), types.end(), t) != types.end();
    };
    const bool has_named_scanner =
        has_type(ProbeSensorType::CARTOGRAPHER) || has_type(ProbeSensorType::BEACON);
    const bool has_specific = std::any_of(types.begin(), types.end(), [](ProbeSensorType t) {
        return t != ProbeSensorType::STANDARD;
    });
    std::vector<bool> keep;
    keep.reserve(types.size());
    for (const ProbeSensorType t : types) {
        const bool alias = (has_specific && t == ProbeSensorType::STANDARD) ||
                           (has_named_scanner && t == ProbeSensorType::EDDY_CURRENT);
        keep.push_back(!alias);
    }
    return keep;
}

// Klicky probes register as a plain [probe] and are told apart by their
// deploy/dock macro pair.
bool has_klicky_macros(const std::vector<std::string>& klipper_objects) {
    const std::string macro_prefix = "gcode_macro ";
    std::set<std::string> macros;
    for (const auto& obj : klipper_objects) {
        if (obj.rfind(macro_prefix, 0) == 0 && obj.size() > macro_prefix.size()) {
            macros.insert(obj.substr(macro_prefix.size()));
        }
    }
    return (macros.count("ATTACH_PROBE") && macros.count("DOCK_PROBE")) ||
           (macros.count("_Probe_Deploy") && macros.count("_Probe_Stow"));
}

// Config values are strings in configfile.config.
std::optional<float> configured_z_offset(const nlohmann::json& section) {
    const auto it = section.find("z_offset");
    if (it == section.end()) {
        return std::nullopt;
    }
    if (it->is_string()) {
        return helix::text_io::parse_leading<float>(it->get<std::string>());
    }
    if (it->is_number()) {
        return it->get<float>();
    }
    return std::nullopt;
}

} // namespace

ProbeSensorManager::Options ProbeSensorManager::make_options() {
    Options o;
    o.name = "ProbeSensorManager";
    o.kinds = {hw::ObjectKind::Probe};
    o.count_subject = "probe_count";
    o.revision_subject = "probe_sensor_revision";
    o.config_key = "probe_sensors";
    for (const auto& r : kProbeSensorRoles) {
        if (r.value == ProbeSensorRole::NONE) {
            continue;
        }
        o.roles.push_back(
            {static_cast<uint8_t>(r.value), r.id, r.display, /*exclusive=*/true, /*offered=*/true});
    }
    for (const auto& t : kProbeSensorTypes) {
        o.types.push_back({static_cast<uint8_t>(t.value), t.id, t.display});
    }
    return o;
}

ProbeSensorManager& ProbeSensorManager::instance() {
    static ProbeSensorManager instance;
    return instance;
}

ProbeSensorManager::ProbeSensorManager() : SensorManager(make_options()) {}

// ============================================================================
// Discovery
// ============================================================================

void ProbeSensorManager::on_entries_changed() {
    for (auto it = readings_.begin(); it != readings_.end();) {
        const auto& name = it->first;
        const bool known =
            std::any_of(entries_locked().begin(), entries_locked().end(),
                        [&](const SensorEntry& e) { return e.klipper_name == name; });
        it = known ? std::next(it) : readings_.erase(it);
    }
}

void ProbeSensorManager::on_discovered(const PrinterDiscovery& hw) {
    auto& entries = entries_locked();
    const auto objects = hw.objects_of({hw::ObjectKind::Probe});
    const auto is_object = [&objects](const std::string& name) {
        return std::find(objects.begin(), objects.end(), name) != objects.end();
    };

    // A config-only probe section (load_cell_probe, dockable_probe) describes the
    // generic probe object its module registers. Beside a probe object it refines
    // that probe's type and supplies its z_offset instead of adding a second
    // sensor; with no probe object it is the sensor.
    std::vector<std::pair<std::string, std::string>> alias_sections; ///< {probe, section}
    const auto primary = std::find_if(entries.begin(), entries.end(), [&](const SensorEntry& e) {
        return e.klipper_name == "probe" && is_object(e.klipper_name);
    });
    auto target = primary != entries.end()
                      ? primary
                      : std::find_if(entries.begin(), entries.end(), [&](const SensorEntry& e) {
                            return is_object(e.klipper_name);
                        });
    if (target != entries.end()) {
        const std::string target_name = target->klipper_name;
        for (const auto& entry : entries) {
            if (is_object(entry.klipper_name)) {
                continue;
            }
            alias_sections.emplace_back(target_name, entry.klipper_name);
            if (type_of(*target) == ProbeSensorType::STANDARD &&
                type_of(entry) != ProbeSensorType::STANDARD) {
                target->type = entry.type;
            }
        }
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                           [&](const SensorEntry& e) { return !is_object(e.klipper_name); }),
            entries.end());
    }

    std::vector<ProbeSensorType> types;
    for (const auto& entry : entries) {
        types.push_back(type_of(entry));
    }
    const auto keep = keep_most_specific(types);
    size_t index = 0;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const SensorEntry&) { return !keep[index++]; }),
                  entries.end());

    // Post-discovery refinement: upgrade STANDARD probes to KLICKY when the
    // characteristic deploy/dock macros are present.
    if (has_klicky_macros(hw.printer_objects())) {
        for (auto& entry : entries) {
            if (type_of(entry) == ProbeSensorType::STANDARD) {
                spdlog::debug("[ProbeSensorManager] Upgrading standard probe '{}' to KLICKY "
                              "(deploy/dock macros present)",
                              entry.display_name);
                entry.type = static_cast<uint8_t>(ProbeSensorType::KLICKY);
            }
        }
    }

    on_entries_changed();

    // Seed z_offset from the configfile for probes whose runtime status reports
    // null (flashforge_loadcell); the configfile always has the persisted value.
    for (const auto& entry : entries) {
        std::vector<std::string> sections = {entry.klipper_name};
        for (const auto& [probe, section] : alias_sections) {
            if (probe == entry.klipper_name) {
                sections.push_back(section);
            }
        }
        for (const auto& section_name : sections) {
            const nlohmann::json* section = hw.section(section_name);
            if (section == nullptr || !section->is_object()) {
                continue;
            }
            if (const auto z_offset = configured_z_offset(*section)) {
                readings_[entry.klipper_name].z_offset = *z_offset;
                spdlog::debug("[ProbeSensorManager] Seeded z_offset={:.3f}mm from config for {}",
                              *z_offset, entry.display_name);
                break;
            }
        }
    }

    // A single probe is the Z probe. A saved role, applied by load_settings()
    // afterwards, overrides this.
    const bool held = std::any_of(entries.begin(), entries.end(),
                                  [](const SensorEntry& e) { return e.role == kZProbe; });
    if (entries.size() == 1 && !held) {
        entries.front().role = kZProbe;
        spdlog::info("[ProbeSensorManager] Auto-assigned Z_PROBE role to '{}'",
                     entries.front().display_name);
    }
    spdlog::info("[ProbeSensorManager] Discovered {} probe sensors", entries.size());
}

nlohmann::json ProbeSensorManager::extra_subscriptions(const PrinterDiscovery& hw) const {
    const auto names = hw.objects_of({hw::ObjectKind::Probe});
    std::vector<ProbeSensorType> types;
    for (const auto& name : names) {
        types.push_back(static_cast<ProbeSensorType>(hw::classify_object(name).chip));
    }
    const auto keep = keep_most_specific(types);

    nlohmann::json objects = nlohmann::json::object();
    for (size_t i = 0; i < names.size(); ++i) {
        if (!keep[i]) {
            continue;
        }
        objects[status_object(types[i], names[i])] =
            publishes_last_query(types[i])
                ? nlohmann::json::array({"last_query", "last_z_result", "z_offset"})
                : nlohmann::json::array({"last_z_result", "z_offset"});
    }
    return objects;
}

// ============================================================================
// Status and subjects
// ============================================================================

bool ProbeSensorManager::parse_status(const nlohmann::json& status) {
    bool changed = false;
    for (const auto& entry : entries_locked()) {
        const std::string key = status_object(type_of(entry), entry.klipper_name);
        const auto sensor = status.find(key);
        if (sensor == status.end() || !sensor->is_object()) {
            continue;
        }
        ProbeSensorState& state = readings_[entry.klipper_name];
        const ProbeSensorState before = state;

        // A null or absent field keeps the previous value. Klipper answers a
        // requested key its module lacks with null, so the configfile-seeded
        // z_offset survives on mainline, where no probe module publishes it.
        // The Creality K1/K2 and QIDI forks do, and K1's Z_OFFSET_APPLY_PROBE
        // changes it live.
        const auto z = sensor->find("last_z_result");
        if (z != sensor->end() && z->is_number()) {
            state.last_z_result = z->get<float>();
        }
        const auto offset = sensor->find("z_offset");
        if (offset != sensor->end() && offset->is_number()) {
            state.z_offset = offset->get<float>();
        }
        // Klipper publishes a bool, Cartographer an int.
        const auto query = sensor->find("last_query");
        if (query != sensor->end() && (query->is_boolean() || query->is_number())) {
            state.triggered = query->is_boolean() ? query->get<bool>() : query->get<int>() != 0;
        }

        if (state.last_z_result != before.last_z_result || state.triggered != before.triggered ||
            state.z_offset != before.z_offset) {
            changed = true;
            spdlog::debug("[ProbeSensorManager] Sensor {} updated: last_z_result={:.3f}mm, "
                          "last_query={}, z_offset={:.3f}mm",
                          entry.display_name, state.last_z_result, state.triggered, state.z_offset);
        }
    }
    return changed;
}

const ProbeSensorState* ProbeSensorManager::z_probe_reading_locked() const {
    static const ProbeSensorState kNoReading;
    for (const auto& entry : entries_locked()) {
        if (entry.role != kZProbe || !entry.enabled || !entry.available) {
            continue;
        }
        const auto reading = readings_.find(entry.klipper_name);
        return reading != readings_.end() ? &reading->second : &kNoReading;
    }
    return nullptr;
}

void ProbeSensorManager::register_subjects(SubjectManager& subjects) {
    // -1 = no sensor assigned
    UI_MANAGED_SUBJECT_INT(z_offset_, -1, "probe_z_offset", subjects);
}

void ProbeSensorManager::refresh_subjects() {
    int z_offset = -1;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        if (const auto* reading = z_probe_reading_locked()) {
            z_offset = static_cast<int>(reading->z_offset * 1000.0f);
        }
    }
    lv_subject_set_int(&z_offset_, z_offset);
    spdlog::trace("[ProbeSensorManager] z_offset={}", z_offset);
}

// ============================================================================
// Reads and edits
// ============================================================================

std::vector<ProbeSensorConfig> ProbeSensorManager::get_sensors() const {
    std::vector<ProbeSensorConfig> out;
    for (const auto& entry : entries()) {
        ProbeSensorConfig config(entry.klipper_name, entry.display_name, type_of(entry));
        config.role = role_of<ProbeSensorRole>(entry);
        config.enabled = entry.enabled;
        out.push_back(std::move(config));
    }
    return out;
}

float ProbeSensorManager::get_z_offset() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    const auto* reading = z_probe_reading_locked();
    return reading ? reading->z_offset : 0.0f;
}

lv_subject_t* ProbeSensorManager::get_probe_z_offset_subject() {
    return &z_offset_;
}

void ProbeSensorManager::set_probe_type_override(ProbeSensorType type) {
    bool changed = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        for (auto& entry : entries_locked()) {
            if (type_of(entry) == ProbeSensorType::STANDARD) {
                spdlog::info("[ProbeSensorManager] Overriding probe type from STANDARD to {} "
                             "(printer database)",
                             probe_type_to_string(type));
                entry.type = static_cast<uint8_t>(type);
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        publish();
    } else {
        spdlog::debug("[ProbeSensorManager] No STANDARD probe to override (may already be typed)");
    }
}

} // namespace helix::sensors
