// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Golden snapshot of how printer objects are discovered, bucketed, subscribed and
// handed to the sensor managers: one document per mock persona plus a kitchen-sink
// object list that holds a name for every classification row and every place the
// copies disagree. Files live under tests/fixtures/sensor_discovery_golden/.
// HELIX_WRITE_GOLDENS=1 rewrites them from current behavior.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../test_helpers/golden_json.h"
#include "../test_helpers/mock_personas.h"
#include "accel_sensor_manager.h"
#include "filament_sensor_manager.h"
#include "humidity_sensor_manager.h"
#include "input_sensor_manager.h"
#include "load_cell_manager.h"
#include "moonraker_client_mock.h"
#include "moonraker_discovery_sequence.h"
#include "printer_discovery.h"
#include "probe_sensor_manager.h"
#include "temperature_sensor_manager.h"
#include "width_sensor_manager.h"

#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::FilamentSensorManager;
using helix::MoonrakerDiscoverySequence;
using helix::PrinterDiscovery;
using helix::test::check_against_golden;
using nlohmann::json;

namespace {

constexpr const char* kGoldenDir = "sensor_discovery_golden";

json buckets_of(const PrinterDiscovery& hw) {
    json b;
    b["heaters"] = hw.heaters();
    b["sensors"] = hw.sensors();
    b["fans"] = hw.fans();
    b["leds"] = hw.leds();
    b["load_cells"] = hw.load_cells();
    b["width_sensor_objects"] = hw.width_sensor_objects();
    b["filament_sensor_names"] = hw.filament_sensor_names();
    b["printer_objects"] = hw.printer_objects();
    b["has_probe"] = hw.has_probe();
    b["has_accelerometer"] = hw.has_accelerometer();
    b["has_filament_sensors"] = hw.has_filament_sensors();
    b["has_width_sensors"] = hw.has_width_sensors();
    b["has_led"] = hw.has_led();
    b["has_led_effects"] = hw.has_led_effects();
    b["has_fan_feedback"] = hw.has_fan_feedback();
    b["has_speaker"] = hw.has_speaker();
    return b;
}

template <typename Cfg>
auto add_enabled(json& entry, const Cfg& s, int) -> decltype((void)s.enabled, void()) {
    entry["enabled"] = s.enabled;
}
template <typename Cfg> void add_enabled(json&, const Cfg&, long) {}

template <typename Cfg, typename RoleFn, typename TypeFn>
json entries_of(const std::vector<Cfg>& sensors, RoleFn role, TypeFn type) {
    json out = json::array();
    for (const auto& s : sensors) {
        json entry = {{"klipper_name", s.klipper_name},
                      {"sensor_name", s.sensor_name},
                      {"role", role(s)},
                      {"type", type(s)}};
        add_enabled(entry, s, 0);
        out.push_back(std::move(entry));
    }
    return out;
}

json manager_entry(json entries) {
    return {{"count", entries.size()}, {"entries", std::move(entries)}};
}

/// Feeds every sensor manager the way init_subsystems_from_hardware() does, plus the
/// configfile-driven calls the discovery sequence makes when a configfile is present.
json run_managers(const PrinterDiscovery& hw, const json* configfile) {
    using namespace helix::sensors;
    auto& fsm = FilamentSensorManager::instance();
    auto& tsm = TemperatureSensorManager::instance();
    auto& lcm = LoadCellManager::instance();
    auto& psm = ProbeSensorManager::instance();
    auto& hsm = HumiditySensorManager::instance();
    auto& wsm = WidthSensorManager::instance();
    auto& ism = InputSensorManager::instance();
    auto& asm_ = AccelSensorManager::instance();

    helix::FilamentSensorManagerTestAccess::reset(fsm);
    tsm.set_sync_mode(true);
    lcm.set_sync_mode(true);
    psm.set_sync_mode(true);
    hsm.set_sync_mode(true);
    wsm.set_sync_mode(true);
    ism.set_sync_mode(true);

    // Empty inputs clear whatever the previous persona left behind.
    tsm.discover({});
    lcm.discover({});
    psm.discover({});
    hsm.discover({});
    wsm.discover({});
    ism.discover({});
    ism.discover_from_config(json::object());
    asm_.discover_from_config(json::object());

    fsm.discover_sensors(hw.filament_sensor_names());
    tsm.discover(helix::temperature_sensor_objects(hw));
    lcm.discover(hw.load_cells());
    psm.discover(hw.printer_objects());
    hsm.discover(hw.printer_objects());
    wsm.discover(hw.width_sensor_objects());
    ism.discover(hw.printer_objects());
    if (configfile != nullptr) {
        psm.discover_from_config(*configfile);
        asm_.discover_from_config(*configfile);
        ism.discover_from_config(*configfile);
    }

    json m;
    m["filament"] = manager_entry(entries_of(
        fsm.get_sensors(), [](const auto& s) { return std::string(role_to_config_string(s.role)); },
        [](const auto& s) { return std::string(type_to_config_string(s.type)); }));
    m["temperature"] = manager_entry(entries_of(
        tsm.get_sensors(), [](const auto& s) { return temp_role_to_string(s.role); },
        [](const auto& s) { return temp_type_to_string(s.type); }));
    m["load_cell"] = manager_entry(entries_of(
        lcm.get_sensors_sorted(), [](const auto& s) { return load_cell_role_to_string(s.role); },
        [](const auto&) { return std::string(); }));
    m["probe"] = manager_entry(entries_of(
        psm.get_sensors(), [](const auto& s) { return probe_role_to_string(s.role); },
        [](const auto& s) { return probe_type_to_string(s.type); }));
    m["humidity"] = manager_entry(entries_of(
        hsm.get_sensors(), [](const auto& s) { return humidity_role_to_string(s.role); },
        [](const auto& s) { return humidity_type_to_string(s.type); }));
    m["width"] = manager_entry(entries_of(
        wsm.get_sensors(), [](const auto& s) { return width_role_to_string(s.role); },
        [](const auto& s) { return width_type_to_string(s.type); }));
    m["input"] = manager_entry(entries_of(
        ism.get_sensors(), [](const auto& s) { return input_role_to_string(s.role); },
        [](const auto& s) { return input_type_to_string(s.type); }));
    m["accel"] = manager_entry(entries_of(
        asm_.get_sensors(), [](const auto& s) { return accel_role_to_string(s.role); },
        [](const auto& s) { return accel_type_to_string(s.type); }));

    // Leave nothing behind for the next test in this process.
    fsm.discover_sensors({});
    tsm.discover({});
    lcm.discover({});
    psm.discover({});
    hsm.discover({});
    wsm.discover({});
    ism.discover({});
    ism.discover_from_config(json::object());
    asm_.discover_from_config(json::object());
    return m;
}

json snapshot(const PrinterDiscovery& hw, const json* configfile) {
    json doc;
    doc["buckets"] = buckets_of(hw);
    doc["subscriptions"] = MoonrakerDiscoverySequence::build_subscription_objects(hw);
    doc["managers"] = run_managers(hw, configfile);
    return doc;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "sensor discovery golden: every mock persona",
                 "[sensor_golden]") {
    for (const auto& persona : helix::mock::kPersonas) {
        DYNAMIC_SECTION(persona.id) {
            helix::test::PersonaEnv env(persona.id);
            MoonrakerClientMock client(persona.type);

            json configfile;
            client.send_jsonrpc(
                "printer.objects.query",
                {{"objects", json::object({{"configfile", json::array({"config"})}})}},
                [&configfile](const json& r) {
                    configfile = r["result"]["status"]["configfile"]["config"];
                });
            REQUIRE(configfile.is_object());

            check_against_golden(kGoldenDir, std::string(persona.id),
                                 snapshot(client.hardware(), &configfile));
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "sensor discovery golden: kitchen sink object list",
                 "[sensor_golden]") {
    const std::string path = helix::test::golden_path(kGoldenDir, "kitchen_sink_objects");
    std::ifstream in(path);
    INFO("missing: " << path);
    REQUIRE(in.good());
    json objects = json::parse(in, nullptr, false);
    REQUIRE(objects.is_array());

    PrinterDiscovery hw;
    hw.parse_objects(objects);
    check_against_golden(kGoldenDir, "kitchen_sink", snapshot(hw, nullptr));
}
