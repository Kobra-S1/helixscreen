// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace helix {
struct PlatformCapabilities;
}

/**
 * @file board_fingerprint.h
 * @brief One string naming the board a measurement was taken on
 *
 * Anything that measures the hardware once and remembers the result (screensaver levels, the
 * glass probe) keys it on this, so a config moved to another board measures again.
 */

namespace helix::ui {

/// What the board fingerprint is made of.
struct BoardFacts {
    std::string display_backend; ///< "drm", "egl", "fbdev" or "sdl"
    int cores = 0;
    float bogomips = 0.0f;
    int32_t width = 0;
    int32_t height = 0;
    int color_depth = 0;
};

/// "<backend>/<cores>c/<bogomips to the nearest 100>bm/<width>x<height>/<depth>bpp"
std::string board_fingerprint(const BoardFacts& facts);

/// The facts for this run: `caps`, the default display's resolution and the build's color
/// depth. An empty `display_backend` reads as "unknown". Main thread.
BoardFacts current_board_facts(const helix::PlatformCapabilities& caps,
                               const std::string& display_backend);

} // namespace helix::ui
