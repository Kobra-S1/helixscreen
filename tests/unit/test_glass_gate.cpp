// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "board_fingerprint.h"
#include "config.h"
#include "glass_gate.h"
#include "platform_capabilities.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ui::decide_glass_gate;
using helix::ui::GlassGateFacts;
using helix::ui::GlassGateVerdict;
using helix::ui::GlassProbeRecord;

namespace {

constexpr const char* kVersion = "1.1.0 (abc)";
constexpr const char* kBoard = "egl/4c/100bm/800x480/32bpp";

/// A board that clears every hard fail and has nothing stored.
GlassGateFacts capable() {
    GlassGateFacts f;
    f.color_depth = 32;
    f.style_effects_allowed = true;
    f.software_rotated = false;
    f.version = kVersion;
    f.board = kBoard;
    return f;
}

GlassProbeRecord record(bool passed, const char* version = kVersion, const char* board = kBoard) {
    return GlassProbeRecord{passed, 3.5, version, board};
}

} // namespace

TEST_CASE("glass gate: a capable board with nothing stored needs a probe", "[glass]") {
    CHECK(decide_glass_gate(capable()) == GlassGateVerdict::NeedsProbe);
}

TEST_CASE("glass gate: the env override beats every other fact", "[glass]") {
    GlassGateFacts f = capable();
    f.color_depth = 16;
    f.env_force = true;
    CHECK(decide_glass_gate(f) == GlassGateVerdict::Pass);

    GlassGateFacts g = capable();
    g.stored = record(true);
    g.env_force = false;
    CHECK(decide_glass_gate(g) == GlassGateVerdict::Fail);
}

TEST_CASE("glass gate: hard fails need no probe", "[glass]") {
    SECTION("16 bpp") {
        GlassGateFacts f = capable();
        f.color_depth = 16;
        f.stored = record(true);
        CHECK(decide_glass_gate(f) == GlassGateVerdict::Fail);
    }
    SECTION("24 bpp is enough") {
        GlassGateFacts f = capable();
        f.color_depth = 24;
        CHECK(decide_glass_gate(f) == GlassGateVerdict::NeedsProbe);
    }
    SECTION("style effects not allowed") {
        GlassGateFacts f = capable();
        f.style_effects_allowed = false;
        f.stored = record(true);
        CHECK(decide_glass_gate(f) == GlassGateVerdict::Fail);
    }
    SECTION("software-rotated display") {
        GlassGateFacts f = capable();
        f.software_rotated = true;
        f.stored = record(true);
        CHECK(decide_glass_gate(f) == GlassGateVerdict::Fail);
    }
}

TEST_CASE("glass gate: a stored record counts only for the same version and board", "[glass]") {
    GlassGateFacts f = capable();

    f.stored = record(true);
    CHECK(decide_glass_gate(f) == GlassGateVerdict::Pass);

    f.stored = record(false);
    CHECK(decide_glass_gate(f) == GlassGateVerdict::Fail);

    f.stored = record(true, "1.0.9 (old)");
    CHECK(decide_glass_gate(f) == GlassGateVerdict::NeedsProbe);

    f.stored = record(true, kVersion, "fbdev/4c/100bm/800x480/32bpp");
    CHECK(decide_glass_gate(f) == GlassGateVerdict::NeedsProbe);

    f.stored = record(false, "1.0.9 (old)");
    CHECK(decide_glass_gate(f) == GlassGateVerdict::NeedsProbe);
}

TEST_CASE("glass gate: HELIX_GLASS parses 1 and 0 only", "[glass]") {
    helix::ScopedEnv env{"HELIX_GLASS"};
    unsetenv("HELIX_GLASS");
    CHECK_FALSE(helix::ui::glass_env_force().has_value());
    setenv("HELIX_GLASS", "1", 1);
    CHECK(helix::ui::glass_env_force() == true);
    setenv("HELIX_GLASS", "0", 1);
    CHECK(helix::ui::glass_env_force() == false);
    for (const char* bad : {"", "yes", "true", "2", " 1", "01"}) {
        CAPTURE(bad);
        setenv("HELIX_GLASS", bad, 1);
        CHECK_FALSE(helix::ui::glass_env_force().has_value());
    }
}

TEST_CASE("glass probe record: malformed nodes are ignored and a written one parses back",
          "[glass]") {
    using json = nlohmann::json;
    CHECK_FALSE(helix::ui::parse_glass_probe(nullptr).has_value());
    for (const char* bad : {
             R"(null)",
             R"([])",
             R"({"passed": true, "frame_ms": 1.0, "version": "v"})",
             R"({"passed": 1, "frame_ms": 1.0, "version": "v", "board": "b"})",
             R"({"passed": true, "frame_ms": "1", "version": "v", "board": "b"})",
             R"({"passed": true, "frame_ms": 1.0, "version": 3, "board": "b"})",
             R"({"passed": true, "frame_ms": 1.0, "version": "v", "board": false})",
         }) {
        CAPTURE(bad);
        const json node = json::parse(bad);
        CHECK_FALSE(helix::ui::parse_glass_probe(&node).has_value());
    }
    const GlassProbeRecord r = record(true);
    const json node = helix::ui::glass_probe_json(r);
    CHECK(helix::ui::parse_glass_probe(&node) == r);
}

TEST_CASE("glass probe record: saved to config loads back from its path", "[glass]") {
    helix::Config* config = helix::Config::get_instance();
    CHECK(std::string(helix::ui::kGlassProbeStorePath) == "/display/glass_probe");

    const GlassProbeRecord r{false, 9.25, "9.9.9 (probe)", "fbdev/2c/1000bm/480x272/32bpp"};
    helix::ui::save_glass_probe(*config, r);
    CHECK(helix::ui::load_glass_probe(*config) == r);

    config->get_json("/display").erase("glass_probe");
    CHECK_FALSE(helix::ui::load_glass_probe(*config).has_value());
}

TEST_CASE("board facts come from the capabilities and the build", "[glass]") {
    helix::PlatformCapabilities caps;
    caps.cpu_cores = 4;
    caps.bogomips = 1234.0f;
    const helix::ui::BoardFacts facts = helix::ui::current_board_facts(caps, "");
    CHECK(facts.display_backend == "unknown");
    CHECK(facts.cores == 4);
    CHECK(facts.bogomips == 1234.0f);
    CHECK(facts.color_depth == LV_COLOR_DEPTH);
    CHECK(helix::ui::current_board_facts(caps, "egl").display_backend == "egl");
}

TEST_CASE_METHOD(LVGLTestFixture, "glass probe draws offscreen and reports a time", "[glass]") {
    const uint32_t children_before = lv_obj_get_child_count(lv_layer_sys());
    const double ms = helix::ui::measure_glass_frame_ms(320, 240);
    CHECK(ms > 0.0);
    CHECK(lv_obj_get_child_count(lv_layer_sys()) == children_before);
    CHECK(helix::ui::measure_glass_frame_ms(0, 240) == 0.0);
}
