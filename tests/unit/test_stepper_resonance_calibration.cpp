// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_stepper_resonance_calibration.cpp
 * @brief The stepper resonance ("Vertical Artifact Compensation") capability:
 *        which printers offer it, and the gcode each one runs.
 */

#include "printer_discovery.h"
#include "stepper_resonance_calibration.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
namespace cal = helix::stepper_resonance_cal;

namespace {

PrinterDiscovery printer_with(std::initializer_list<const char*> extra) {
    json objects = json::array({"gcode_move", "toolhead", "extruder"});
    for (const char* o : extra) {
        objects.push_back(o);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// A Creator 5 Pro on Z-Mod: zmod_color plus the carriage grab buttons.
PrinterDiscovery zmod_c5(bool with_vfa_macro) {
    json objects = json::array({"gcode_move", "toolhead", "extruder", "extruder1", "extruder2",
                                "extruder3", "zmod", "zmod_color"});
    for (int i = 1; i <= 4; ++i) {
        objects.push_back("gcode_button extruder_pos" + std::to_string(i));
        objects.push_back("gcode_button extruder_grab" + std::to_string(i));
    }
    if (with_vfa_macro) {
        objects.push_back("gcode_macro CALIBRATE_VFA");
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

} // namespace

TEST_CASE("stepper resonance cal: a Z-Mod Creator 5 runs CALIBRATE_VFA per tool",
          "[stepper_resonance_cal]") {
    const PrinterDiscovery c5 = zmod_c5(/*with_vfa_macro=*/true);
    CHECK(cal::supported(c5));
    CHECK(cal::per_tool(c5));
    CHECK(cal::restarts_klipper(c5));
    CHECK(cal::calibrate_gcode(c5, 0) == "CALIBRATE_VFA T=0");
    CHECK(cal::calibrate_gcode(c5, 3) == "CALIBRATE_VFA T=3");
    CHECK_FALSE(cal::provider_name(c5).empty());
}

TEST_CASE("stepper resonance cal: a per-tool routine refuses a negative tool",
          "[stepper_resonance_cal]") {
    CHECK(cal::calibrate_gcode(zmod_c5(/*with_vfa_macro=*/true), -1).empty());
}

TEST_CASE("stepper resonance cal: needs the macro and the changer", "[stepper_resonance_cal]") {
    const PrinterDiscovery bare_c5 = zmod_c5(/*with_vfa_macro=*/false);
    CHECK_FALSE(cal::supported(bare_c5));
    CHECK(cal::calibrate_gcode(bare_c5, 0).empty());

    // The macro alone, on a printer no provider claims, is not the routine.
    const PrinterDiscovery stray = printer_with({"gcode_macro CALIBRATE_VFA"});
    CHECK_FALSE(cal::supported(stray));
    CHECK(cal::calibrate_gcode(stray, 0).empty());
}

TEST_CASE("stepper resonance cal: a printer without it answers no everywhere",
          "[stepper_resonance_cal]") {
    const PrinterDiscovery bare = printer_with({});
    CHECK_FALSE(cal::supported(bare));
    CHECK_FALSE(cal::per_tool(bare));
    CHECK_FALSE(cal::restarts_klipper(bare));
    CHECK(cal::calibrate_gcode(bare, 0).empty());
    CHECK(cal::provider_name(bare).empty());
}
