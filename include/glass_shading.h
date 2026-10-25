// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>

/**
 * @file glass_shading.h
 * @brief The colors of the glass appearance, derived from whatever color a widget has
 *
 * Every stop is the base color mixed toward white or black, never another hue, so glass is
 * safe on any theme and on a widget whose color was set locally. Pure: no LVGL objects.
 */

namespace helix::ui {

/// Which glass treatment a widget part gets.
enum class GlassFamily : uint8_t {
    None,     ///< Flat: chrome that opts out, and content whose pixels are data
    Raised,   ///< Something pressed or dragged: buttons, chips, knobs, keycaps
    Surface,  ///< Cards, dialogs, widgets, rows: lit from their own top-left corner
    Recessed, ///< Tracks, troughs and input fields
};

/// Gradient stops for one fill. `frac` is the stop position, 0..255.
struct GlassStops {
    int count = 0;
    lv_color_t color[4]{};
    uint8_t frac[4]{};
};

/// Radius of a Surface's corner light. Fixed in pixels so every card catches the same light
/// whatever its size.
constexpr int32_t kGlassGlowRadiusPx = 420;

/// The fill stops for `family` over `base`. Raised and Recessed run top to bottom; Surface
/// runs out from the top-left corner. None has no stops.
GlassStops glass_stops(GlassFamily family, lv_color_t base, bool dark, bool pressed);

/// Writes glass_stops() into `grad` with the family's geometry: vertical for Raised and
/// Recessed, radial from the fill area's top-left out to kGlassGlowRadiusPx for Surface.
/// Returns false, leaving `grad` untouched, for None.
bool glass_gradient(lv_grad_dsc_t* grad, GlassFamily family, lv_color_t base, bool dark,
                    bool pressed);

/// The 1px rim drawn over `base`: lighter in dark mode, darker in light mode. None returns
/// `base`.
lv_color_t glass_rim_color(GlassFamily family, lv_color_t base, bool dark);

} // namespace helix::ui
