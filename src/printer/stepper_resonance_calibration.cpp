// SPDX-License-Identifier: GPL-3.0-or-later

#include "stepper_resonance_calibration.h"

#include "printer_discovery.h"
#include "toolchanger_addon.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>

namespace helix::stepper_resonance_cal {
namespace {

/// One firmware that ships a stepper resonance calibration.
struct Provider {
    const char* name;
    /// Identifies the firmware from discovered hardware.
    bool (*detect)(const PrinterDiscovery& hw);
    /// The routine's command.
    const char* macro;
    /// Parameter naming the 0-based tool to run with, or nullptr for a
    /// whole-machine routine.
    const char* tool_param;
    /// The routine ends in SAVE_CONFIG.
    bool restarts_klipper;
};

// --- Z-Mod on the FlashForge Creator 5 Pro -----------------------------------
//
// CALIBRATE_VFA T=<n> (c5pro_generic.cfg) mounts head n, zeroes the gcode
// offset, moves to X130 Y130, runs FlashForge's
// STEPPER_RESONANCE_FACTORY_CALIBRATE ("VFA vertical stripe compensation"),
// parks the head and ends in SAVE_CONFIG.
bool zmod_c5_detect(const PrinterDiscovery& hw) {
    return toolchanger_addon::zmod_c5_present(hw) && hw.has_macro("CALIBRATE_VFA");
}

const Provider kProviders[] = {
    {"Z-Mod Creator 5", zmod_c5_detect, "CALIBRATE_VFA", "T", /*restarts_klipper=*/true},
};

const Provider* match(const PrinterDiscovery& hw) {
    for (const auto& p : kProviders) {
        if (p.detect(hw)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

bool supported(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

bool per_tool(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->tool_param;
}

std::string calibrate_gcode(const PrinterDiscovery& hw, int tool) {
    const Provider* p = match(hw);
    if (!p) {
        return {};
    }
    if (!p->tool_param) {
        return p->macro;
    }
    if (tool < 0) {
        return {};
    }
    return fmt::format("{} {}={}", p->macro, p->tool_param, tool);
}

bool restarts_klipper(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->restarts_klipper;
}

const char* entry_name() {
    return TR_NOOP("Vertical Artifact Compensation");
}

const char* entry_description() {
    return TR_NOOP("Tune the motors to remove fine vertical lines");
}

std::string provider_name(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->name : "";
}

} // namespace helix::stepper_resonance_cal
