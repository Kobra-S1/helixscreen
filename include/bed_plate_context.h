// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What every plate view asks of the printer before it draws or moves: the plate area in
// G-code space, whether it is round, the envelope a move is clamped to, whether the plate
// may move the head now, and the lift for the next XY move. The Bed tab, the Move tab's
// presets and the home Toolhead widget all read these, so they agree.

#include "motion_presets.h"
#include "printer_motion_state.h"

#include <optional>

namespace helix {

class IMoonrakerAPI;
class PrinterState;

struct BedPlateArea {
    AxisBounds area; ///< preset_area(): the plate in G-code space
    bool circular = false;
};

/// nullopt with no API or a degenerate area.
[[nodiscard]] std::optional<BedPlateArea> current_bed_plate(const PrinterState& ps,
                                                            IMoonrakerAPI* api);

/// The G-code envelope inset by kGcodeEdgeMarginMm: what absolute moves are clamped to.
[[nodiscard]] AxisBounds move_clamp_bounds(const PrinterState& ps);

/// Every axis homed, nav enabled, machine_motion_blocked clear and no external blocking
/// operation: the plate and the position presets may move the head.
[[nodiscard]] bool plate_moves_allowed(PrinterState& ps);

/// The height of a part that may still be on the bed, from get_bed_occupancy_state():
/// nullopt with no state, a clear bed, or an occupying job of unknown height.
[[nodiscard]] std::optional<double> bed_part_height();

/// plate_lift() for a move starting at @p start_z, with the clearance setting, the clamp
/// envelope's z_max and the bed-occupancy part height. Kind::None when Z bounds are unknown.
[[nodiscard]] PlateLift current_plate_lift(const PrinterState& ps, double start_z);

/// How far Park raises the nozzle before it travels.
inline constexpr double kParkZLiftMm = 10.0;

/// Park's lift from @p current_z: the higher of current_z + kParkZLiftMm and the plate
/// floor, capped at the clamp envelope's z_max. Refuse when current_plate_lift() refuses
/// (a part too tall to clear); Kind::None when Z bounds are unknown or the cap leaves no
/// rise. Shared by the Move tab's Park and the home Toolhead widget's Park slot.
[[nodiscard]] PlateLift park_plate_lift(const PrinterState& ps, double current_z);

} // namespace helix
