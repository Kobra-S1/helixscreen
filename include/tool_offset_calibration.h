// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Automatic tool offset calibration.
//
// klipper-toolchanger ships a CALIBRATE_TOOL_OFFSETS macro
// (examples/calibrate-offsets.cfg) that does the whole job in one blocking
// gcode: heats every nozzle, locates the calibration sensor with the first
// tool, then for every other tool selects it, probes it against the sensor and
// writes the result into that tool's gcode_x/y/z_offset with SET_TOOL_PARAMETER
// + SAVE_TOOL_PARAMETER. Nothing in the macro persists: the staged parameters
// wait for a SAVE_CONFIG, which is exactly what the per-tool offset save path
// (helix::tool_offsets, ToolState's dirty tracking) already handles.
//
// This module is the ONLY place that decides which printers calibrate their
// tool offsets and how. Three procedures exist: the macro above, run by the
// tool offset panel; a printer's own multi-step guided flow (a Snapmaker U1's),
// which the Calibration entry starts instead; and a changer firmware's macro
// that hands the whole run to a printer-side action:prompt
// (toolchanger_addon::resolve_offset_calibration), which the entry sends
// without opening the panel. Callers ask these questions and never name the
// firmware. There is no per-tool entry point: the macro is
// all-or-nothing, so this module offers exactly one gcode.
//
// Nothing is parsed off the console. Everything the screen needs is already in
// subscribed status: which tool is on the carriage (`toolchanger`.tool_number,
// ToolState's active tool), which tools have been measured (the macro's
// SET_TOOL_PARAMETER writes land on the `tool T<n>` objects and reach
// ToolState through helix::tool_offsets), and when the run is over (the rpc
// completes). The panel shows the run's status line and marks the row for the
// tool currently on the carriage; it infers no per-tool progress beyond that,
// because the macro reports none.

#include <string>

namespace helix {
class PrinterDiscovery;
}

namespace helix::tool_offset_calibration {

/// The macro's command name. klipper-toolchanger's example config defines it
/// as [gcode_macro CALIBRATE_TOOL_OFFSETS]; its presence is the capability.
inline constexpr const char* kMacro = "CALIBRATE_TOOL_OFFSETS";

/// Whether this printer can calibrate its tool offsets automatically: a tool
/// changer that defines the macro, or a printer with its own guided flow. False
/// on every single-toolhead printer and on a tool changer whose config never
/// adopted the example macro.
bool supported(const PrinterDiscovery& hw);

/// The guided device flow (ui_guided_device_flow.h) the Tool Offsets entry
/// starts on this printer, or an empty string when the macro panel runs. A
/// printer's own flow may be a setup step its offsets depend on rather than a
/// measurement (an INDX seats a tool by hand for its first Z home).
std::string guided_flow(const PrinterDiscovery& hw);

/// Gcode the Tool Offsets entry sends instead of opening the panel, or an empty
/// string when the panel (or a guided flow) runs. The command opens the
/// printer's own action:prompt, which confirms and starts the measurement, and
/// answers before anything moves: nothing on this side can follow the run, and
/// the panel's completion and result rows would claim a finish that has not
/// happened.
std::string printer_prompt_gcode(const PrinterDiscovery& hw);

/// What the Tool Offsets entry does on this printer, untranslated (pass it
/// through lv_tr()).
const char* entry_description(const PrinterDiscovery& hw);

/// Gcode that calibrates every tool, or an empty string when the printer has
/// no such procedure. Blocking on the printer: Moonraker's printer.gcode.script
/// answers when the macro finishes, so the rpc's completion IS the run's.
/// That completion is the only progress followed: which tool is under the
/// probe, and in what order, is the macro's business.
std::string calibrate_all_gcode(const PrinterDiscovery& hw);

} // namespace helix::tool_offset_calibration
