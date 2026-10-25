// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "hv/json.hpp"

namespace helix {
class Config;
}

/**
 * @file glass_gate.h
 * @brief Whether this board may show the glass appearance at all
 *
 * A board fails outright below 24 bpp (radial gradients band into rings), on a tier without
 * full style effects, and on a software-rotated display. Any other board draws a glass frame
 * offscreen once per board fingerprint and app version, and passes when it fits the budget.
 * The result is stored at kGlassProbeStorePath, beside the screensaver levels.
 */

namespace helix::ui {

/// One probe's result, and the board and app version it was measured on.
struct GlassProbeRecord {
    bool passed = false;
    double frame_ms = 0;
    std::string version;
    std::string board;

    bool operator==(const GlassProbeRecord& o) const {
        return passed == o.passed && frame_ms == o.frame_ms && version == o.version &&
               board == o.board;
    }
};

/// Everything the gate decides from.
struct GlassGateFacts {
    int color_depth = 0;
    bool style_effects_allowed = false;
    bool software_rotated = false;
    std::optional<bool> env_force; ///< HELIX_GLASS
    std::optional<GlassProbeRecord> stored;
    std::string version;
    std::string board;
};

enum class GlassGateVerdict { Pass, Fail, NeedsProbe };

/// HELIX_GLASS wins; then the hard fails; then a record stored for this version and board;
/// otherwise the board must be measured.
GlassGateVerdict decide_glass_gate(const GlassGateFacts& f);

/// HELIX_GLASS: "1" forces a pass, "0" a fail, anything else (or unset) is no override.
std::optional<bool> glass_env_force();

constexpr const char* kGlassProbeStorePath = "/display/glass_probe";

/// The record at `node`, or nullopt unless it is an object with a boolean "passed", a number
/// "frame_ms" and string "version" and "board".
std::optional<GlassProbeRecord> parse_glass_probe(const nlohmann::json* node);
nlohmann::json glass_probe_json(const GlassProbeRecord& r);

std::optional<GlassProbeRecord> load_glass_probe(const helix::Config& c);

/// Writes the record and saves the config file.
void save_glass_probe(helix::Config& c, const GlassProbeRecord& r);

/// A frame may take a third of a 60 Hz refresh.
constexpr double kGlassFrameBudgetMs = 6.0;

/// Draws a representative glass frame at `width` x `height` into an offscreen canvas (one
/// full-size Surface fill and twelve Raised buttons), five times, and returns the median ms.
/// Nothing reaches the display. Main thread.
double measure_glass_frame_ms(int32_t width, int32_t height);

} // namespace helix::ui
