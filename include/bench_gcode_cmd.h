// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix::bench {

/// `helix-screen bench-gcode <file> [canvas_rows]`: time what a streamed G-code
/// preview costs on this board, without the UI. Runs the layer index scan, the
/// ghost pass's layer parses (sampled exactly as a canvas of @p canvas_rows rows
/// samples them) and single-layer parses, then prints wall and CPU time for each.
/// Dispatched before config, logging and the instance lock, so it runs beside a
/// live HelixScreen. Returns a process exit code.
int run_bench_gcode(int argc, char** argv);

} // namespace helix::bench
