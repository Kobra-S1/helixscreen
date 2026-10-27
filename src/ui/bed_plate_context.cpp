// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "bed_plate_context.h"

#include "bed_occupancy_state.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "toolhead_homing.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace helix {

std::optional<BedPlateArea> current_bed_plate(const PrinterState& ps, IMoonrakerAPI* api) {
    if (!api) {
        return std::nullopt;
    }
    const auto area =
        preset_area(ps.motion_state().get_axis_bounds(), ps.motion_state().get_gcode_axis_bounds(),
                    api->hardware().build_volume());
    if (!area.has_x || !area.has_y || area.x_max <= area.x_min || area.y_max <= area.y_min) {
        return std::nullopt;
    }
    return BedPlateArea{area, circular_bed_kinematics(api->hardware().kinematics())};
}

AxisBounds move_clamp_bounds(const PrinterState& ps) {
    return inset_bounds(ps.motion_state().get_gcode_axis_bounds(), kGcodeEdgeMarginMm);
}

bool plate_moves_allowed(PrinterState& ps) {
    return toolhead_is_homed(ps) &&
           lv_subject_get_int(ps.network_state().get_nav_buttons_enabled_subject()) != 0 &&
           lv_subject_get_int(ps.print_state().get_machine_motion_blocked_subject()) == 0 &&
           !ps.is_external_blocking_operation_active();
}

std::optional<double> bed_part_height() {
    const BedOccupancyState* state = get_bed_occupancy_state();
    if (!state || !state->current().occupied) {
        return std::nullopt;
    }
    return state->current().part_height_mm;
}

PlateLift current_plate_lift(const PrinterState& ps, double start_z) {
    const AxisBounds bounds = move_clamp_bounds(ps);
    if (!bounds.has_z) {
        return {};
    }
    return plate_lift(start_z,
                      static_cast<double>(SettingsManager::instance().get_bed_map_clearance_mm()),
                      static_cast<double>(bounds.z_max), bed_part_height());
}

PlateLift park_plate_lift(const PrinterState& ps, double current_z) {
    const PlateLift floor = current_plate_lift(ps, current_z);
    if (floor.kind == PlateLift::Kind::Refuse) {
        return floor;
    }
    const AxisBounds bounds = move_clamp_bounds(ps);
    if (!bounds.has_z) {
        return {};
    }
    double z = current_z + kParkZLiftMm;
    if (floor.kind == PlateLift::Kind::Lift) {
        z = std::max(z, floor.z);
    }
    z = std::min(z, static_cast<double>(bounds.z_max));
    if (z <= current_z) {
        return {};
    }
    return {PlateLift::Kind::Lift, z};
}

namespace {

/// Trailing zeros trimmed, so 43 reads "43" and 5.5 reads "5.5".
std::string format_lift_mm(double mm) {
    char buf[16];
    if (mm == std::floor(mm)) {
        std::snprintf(buf, sizeof(buf), "%.0f", mm);
    } else {
        std::snprintf(buf, sizeof(buf), "%g", mm);
    }
    return buf;
}

} // namespace

std::string plate_lift_readout(double shown_z, const PlateLift& lift) {
    if (lift.kind != PlateLift::Kind::Lift) {
        return {};
    }
    char z_buf[16];
    std::snprintf(z_buf, sizeof(z_buf), "%.2f", shown_z);
    const std::string lift_mm = format_lift_mm(lift.z);
    if (const auto part = bed_part_height()) {
        return fmt::format(fmt::runtime(lv_tr("Z {}, will lift to {}mm: last print is {}mm tall")),
                           z_buf, lift_mm, format_lift_mm(*part));
    }
    return fmt::format(fmt::runtime(lv_tr("Z {}, will lift to {}mm")), z_buf, lift_mm);
}

const char* part_too_tall_text() {
    return lv_tr("The part on the bed is too tall to clear. Remove it, then tap Cleared.");
}

} // namespace helix
