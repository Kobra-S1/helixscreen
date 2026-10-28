// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_accel_discovery_wiring.cpp
 * @brief Accelerometers reach AccelSensorManager from configfile sections
 *        (prestonbrown/helixscreen#1262)
 *
 * Klipper accelerometers have no get_status(), so they exist only in
 * configfile.config. The discovery sequence's configfile arm records those
 * sections in PrinterDiscovery; the registry's discover(hw) turns them into
 * manager entries. Left unwired, Settings > Sensors shows an empty list, telemetry
 * reports accel:0 and detect_belt_hardware() sources has_adxl from the same empty
 * manager.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "accel_sensor_manager.h"
#include "moonraker_advanced_api.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using helix::PrinterDiscovery;
using helix::sensors::AccelSensorManager;
using helix::sensors::AccelSensorType;
using helix::sensors::SensorEntry;

namespace {

/// Mock transport that runs the REAL discovery sequence: MoonrakerClientMock
/// overrides discover_printer() with a shortcut, so the base implementation is
/// called directly while send_jsonrpc() still dispatches through the mock.
class AccelDiscoveryClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    void discover_printer_real() {
        MoonrakerClient::discover_printer([]() {}, [](const std::string&) {});
    }
};

PrinterDiscovery discovery_with_config(const json& config) {
    PrinterDiscovery hw;
    hw.parse_config_keys(config);
    return hw;
}

/// Discovering an empty PrinterDiscovery drops every entry.
void reset_accel_manager() {
    helix::ui::UpdateQueue::instance().drain();
    AccelSensorManager::instance().discover(PrinterDiscovery{});
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE("Discovery sequence's configfile arm records accelerometer sections", "[sensors][1262]") {
    LVGLTestFixture fixture;

    AccelDiscoveryClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.discover_printer_real();

    const auto sections = client.hardware().sections_of({helix::hw::ObjectKind::Accelerometer});
    CHECK(std::find(sections.begin(), sections.end(), "adxl345") != sections.end());
}

TEST_CASE("A configfile-only accelerometer reaches AccelSensorManager", "[sensors][1262]") {
    LVGLTestFixture fixture;
    reset_accel_manager();
    auto& mgr = AccelSensorManager::instance();
    REQUIRE(mgr.sensor_count() == 0);

    mgr.discover(discovery_with_config({{"adxl345", json::object()}}));

    REQUIRE(mgr.sensor_count() == 1);
    SensorEntry entry;
    REQUIRE(mgr.find("adxl345", entry));
    CHECK(entry.type == static_cast<uint8_t>(AccelSensorType::ADXL345));
    reset_accel_manager();
}

TEST_CASE("A Beacon onboard accelerometer found only in configfile reaches the manager",
          "[sensors][1262]") {
    LVGLTestFixture fixture;
    reset_accel_manager();
    auto& mgr = AccelSensorManager::instance();

    // The objects pass alone names the probe; the accelerometer is in the config.
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"beacon", "probe"}));
    mgr.discover(hw);
    REQUIRE(mgr.sensor_count() == 0);

    hw.parse_config_keys({{"beacon", {{"accel_scale", "16g"}, {"serial", "/dev/ttyACM0"}}}});
    mgr.discover(hw);

    REQUIRE(mgr.sensor_count() == 1);
    SensorEntry beacon;
    REQUIRE(mgr.find("beacon", beacon));
    CHECK(beacon.type == static_cast<uint8_t>(AccelSensorType::LIS2DW));
    reset_accel_manager();
}

TEST_CASE("Discovery leaves AccelSensorManager empty without an accel section", "[sensors][1262]") {
    LVGLTestFixture fixture;
    reset_accel_manager();
    auto& mgr = AccelSensorManager::instance();

    mgr.discover(discovery_with_config(
        {{"printer", {{"kinematics", "corexy"}}}, {"heater_bed", json::object()}}));

    CHECK(mgr.sensor_count() == 0);
}

TEST_CASE("Repeated discovery does not duplicate accelerometer entries", "[sensors][1262]") {
    LVGLTestFixture fixture;
    reset_accel_manager();
    auto& mgr = AccelSensorManager::instance();
    const PrinterDiscovery hw =
        discovery_with_config({{"adxl345", json::object()}, {"adxl345 bed", json::object()}});

    mgr.discover(hw);
    REQUIRE(mgr.sensor_count() == 2);
    mgr.discover(hw);

    CHECK(mgr.sensor_count() == 2);
    reset_accel_manager();
}

TEST_CASE("detect_belt_hardware reports has_adxl once the manager has discovered",
          "[sensors][1262]") {
    LVGLTestFixture fixture;

    helix::PrinterState state;
    state.init_subjects(false);
    AccelDiscoveryClient client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state);
    MoonrakerAdvancedAPI advanced(client, api);
    reset_accel_manager();

    auto detect = [&]() {
        helix::calibration::BeltTensionHardware out;
        bool done = false;
        advanced.detect_belt_hardware(
            [&](const helix::calibration::BeltTensionHardware& hw) {
                out = hw;
                done = true;
            },
            [](const MoonrakerError&) {});
        REQUIRE(done);
        return out;
    };

    CHECK_FALSE(detect().has_adxl);
    AccelSensorManager::instance().discover(discovery_with_config({{"adxl345", json::object()}}));
    CHECK(detect().has_adxl);
    reset_accel_manager();
}
