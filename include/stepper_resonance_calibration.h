// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Stepper resonance calibration ("Vertical Artifact Compensation").
//
// Some firmwares ship a routine that measures the resonance of the XY stepper
// motors themselves and tunes a damping compensation for it, which removes the
// fine, evenly spaced vertical lines a motor's own vibration prints into walls.
// It is not input shaping: input shaping cancels frame ringing after a corner,
// this cancels the motor's ripple at constant speed.
//
// This module is the ONLY place that knows which printers have such a routine
// and how each one runs. The Calibration entry asks these questions and never
// names the firmware. Adding a firmware means adding one Provider to the table
// in stepper_resonance_calibration.cpp - no call site changes.

#include <string>

namespace helix {
class PrinterDiscovery;
}

namespace helix::stepper_resonance_cal {

/// Whether this printer can run the calibration.
bool supported(const PrinterDiscovery& hw);

/// Whether the routine runs with one chosen tool on the carriage, so the user
/// picks which. False for a whole-machine routine, and on an unsupported
/// printer.
bool per_tool(const PrinterDiscovery& hw);

/// Gcode that runs the calibration. @p tool (0-based) picks the tool for a
/// per-tool routine and is ignored otherwise. Empty when the printer has no
/// routine, or a per-tool routine is handed a negative tool.
std::string calibrate_gcode(const PrinterDiscovery& hw, int tool);

/// Whether the routine ends in SAVE_CONFIG, which restarts Klipper: the
/// connection drops when it finishes, and the user must be told before it
/// starts.
bool restarts_klipper(const PrinterDiscovery& hw);

/// The entry's name and what it does, untranslated (pass them through lv_tr()).
const char* entry_name();
const char* entry_description();

/// Human-readable name of the matched firmware, for logging. Empty when none.
std::string provider_name(const PrinterDiscovery& hw);

} // namespace helix::stepper_resonance_cal
