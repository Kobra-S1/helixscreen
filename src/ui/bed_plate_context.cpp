// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "bed_plate_context.h"

#include "bed_occupancy_state.h"
#include "i_moonraker_api.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "toolhead_homing.h"

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

} // namespace helix
