// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "gcode_preview_gate.h"

#include "../catch_amalgamated.hpp"

using helix::ui::decide_preview_gate;
using helix::ui::PreviewGate;
using helix::ui::PreviewGateInputs;
using helix::ui::PreviewRateEntry;

namespace {
constexpr uint64_t kMb = 1024ull * 1024ull;
constexpr double kCc1Rate = 10.0;

PreviewGateInputs cc1(bool printing) {
    PreviewGateInputs in;
    in.learned_rate = kCc1Rate;
    in.printing = printing;
    return in;
}
} // namespace

TEST_CASE("preview gate: a CC1 mid-print shows the thumbnail for a 41MB file",
          "[gcode][preview_gate]") {
    CHECK(decide_preview_gate(41 * kMb, cc1(true)) == PreviewGate::TooExpensive);
    CHECK(decide_preview_gate(41 * kMb, cc1(false)) == PreviewGate::Build);
    CHECK(decide_preview_gate(5 * kMb, cc1(true)) == PreviewGate::Build);
}

TEST_CASE("preview gate: an explicit render mode is the user's call", "[gcode][preview_gate]") {
    PreviewGateInputs in = cc1(true);
    in.auto_mode = false;
    CHECK(decide_preview_gate(41 * kMb, in) == PreviewGate::Build);
}

TEST_CASE("preview gate: RAM refuses before CPU is considered", "[gcode][preview_gate]") {
    PreviewGateInputs in = cc1(false);
    in.memory_safe = false;
    CHECK(decide_preview_gate(1 * kMb, in) == PreviewGate::TooLarge);
    in.auto_mode = false;
    CHECK(decide_preview_gate(1 * kMb, in) == PreviewGate::TooLarge);
}

TEST_CASE("preview gate: no learned rate never refuses on CPU", "[gcode][preview_gate]") {
    PreviewGateInputs in = cc1(true);
    in.learned_rate = 0.0;
    CHECK(decide_preview_gate(500 * kMb, in) == PreviewGate::Build);
}

TEST_CASE("preview rate entry parses only a well-formed, positive rate", "[gcode][preview_gate]") {
    const PreviewRateEntry entry{9.8, "fbdev/2c/0bm/480x272/16bpp", "1.1.0 (probe)"};
    const nlohmann::json node = helix::ui::preview_rate_json(entry);
    CHECK(helix::ui::parse_preview_rate(&node) == entry);

    CHECK_FALSE(helix::ui::parse_preview_rate(nullptr).has_value());
    for (const char* bad :
         {R"({"rate": 0, "board": "b", "version": "v"})",
          R"({"rate": -3.0, "board": "b", "version": "v"})",
          R"({"rate": "9.8", "board": "b", "version": "v"})", R"({"board": "b", "version": "v"})",
          R"({"rate": 9.8, "version": "v"})", R"([1, 2])"}) {
        CAPTURE(bad);
        const nlohmann::json node2 = nlohmann::json::parse(bad, nullptr, false);
        CHECK_FALSE(helix::ui::parse_preview_rate(&node2).has_value());
    }
}

TEST_CASE("a learned preview rate counts only on the board and version that measured it",
          "[gcode][preview_gate]") {
    helix::Config* config = helix::Config::get_instance();
    const PreviewRateEntry entry{9.8, "board-a", "version-a"};
    helix::ui::save_preview_rate(*config, entry);

    CHECK(helix::ui::learned_preview_rate(*config, "board-a", "version-a") == 9.8);
    CHECK(helix::ui::learned_preview_rate(*config, "board-b", "version-a") == 0.0);
    CHECK(helix::ui::learned_preview_rate(*config, "board-a", "version-b") == 0.0);

    config->get_json("/display").erase("gcode_preview_rate");
    CHECK(helix::ui::learned_preview_rate(*config, "board-a", "version-a") == 0.0);
}
