// tests/unit/test_power_loss_sensor.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "power_loss_sensor.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using namespace helix::power_loss;

namespace {

PrinterDiscovery hardware_with_objects(const std::vector<std::string>& names) {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array();
    for (const auto& n : names) {
        objects.push_back(n);
    }
    hw.parse_objects(objects);
    return hw;
}

} // namespace

TEST_CASE("power loss sensor: detected by its object", "[power_loss][sensor]") {
    PrinterDiscovery hw = hardware_with_objects({"power_loss_check"});
    REQUIRE(required_status_objects(hw) == std::vector<std::string>{"power_loss_check"});
}

TEST_CASE("power loss sensor: only the bare name detects", "[power_loss][sensor]") {
    // The firmware also publishes power_loss_check e0..e3, one per extruder,
    // all uninitialised. Detection on a prefix or substring would match those
    // and read a monitor that has never taken a reading.
    PrinterDiscovery hw = hardware_with_objects({"power_loss_check e0", "power_loss_check e1",
                                                 "power_loss_check e2", "power_loss_check e3"});
    REQUIRE(required_status_objects(hw).empty());
}

TEST_CASE("power loss sensor: a printer without the sensor has nothing to subscribe",
          "[power_loss][sensor]") {
    PrinterDiscovery hw = hardware_with_objects({"bed_mesh", "quad_gantry_level"});
    REQUIRE(required_status_objects(hw).empty());
}
