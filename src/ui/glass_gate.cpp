// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "glass_gate.h"

#include "config.h"
#include "glass_shading.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace helix::ui {

GlassGateVerdict decide_glass_gate(const GlassGateFacts& f) {
    if (f.env_force) {
        return *f.env_force ? GlassGateVerdict::Pass : GlassGateVerdict::Fail;
    }
    if (f.color_depth < 24 || !f.style_effects_allowed || f.software_rotated) {
        return GlassGateVerdict::Fail;
    }
    if (f.stored && f.stored->version == f.version && f.stored->board == f.board) {
        return f.stored->passed ? GlassGateVerdict::Pass : GlassGateVerdict::Fail;
    }
    return GlassGateVerdict::NeedsProbe;
}

std::optional<bool> glass_env_force() {
    const char* v = std::getenv("HELIX_GLASS");
    if (v == nullptr) {
        return std::nullopt;
    }
    if (std::strcmp(v, "1") == 0) {
        return true;
    }
    if (std::strcmp(v, "0") == 0) {
        return false;
    }
    spdlog::warn("[Glass] Ignoring HELIX_GLASS='{}': expected 1 or 0", v);
    return std::nullopt;
}

std::optional<GlassProbeRecord> parse_glass_probe(const nlohmann::json* node) {
    if (node == nullptr || !node->is_object()) {
        return std::nullopt;
    }
    const auto passed = node->find("passed");
    const auto frame_ms = node->find("frame_ms");
    const auto version = node->find("version");
    const auto board = node->find("board");
    if (passed == node->end() || !passed->is_boolean() || frame_ms == node->end() ||
        !frame_ms->is_number() || version == node->end() || !version->is_string() ||
        board == node->end() || !board->is_string()) {
        return std::nullopt;
    }
    GlassProbeRecord r;
    r.passed = passed->get<bool>();
    r.frame_ms = frame_ms->get<double>();
    r.version = version->get<std::string>();
    r.board = board->get<std::string>();
    return r;
}

nlohmann::json glass_probe_json(const GlassProbeRecord& r) {
    return nlohmann::json{
        {"passed", r.passed}, {"frame_ms", r.frame_ms}, {"version", r.version}, {"board", r.board}};
}

std::optional<GlassProbeRecord> load_glass_probe(const helix::Config& c) {
    return parse_glass_probe(c.try_get_json(kGlassProbeStorePath));
}

void save_glass_probe(helix::Config& c, const GlassProbeRecord& r) {
    c.set<nlohmann::json>(kGlassProbeStorePath, glass_probe_json(r));
    c.save();
}

namespace {

constexpr int kProbeRepetitions = 5;
constexpr int kProbeButtons = 12;
constexpr int32_t kProbeButtonW = 140;
constexpr int32_t kProbeButtonH = 46;
constexpr int32_t kProbeRadius = 10;

void fill_glass(lv_layer_t* layer, const lv_area_t& area, GlassFamily family, lv_color_t base) {
    lv_draw_fill_dsc_t dsc;
    lv_draw_fill_dsc_init(&dsc);
    dsc.radius = kProbeRadius;
    dsc.opa = LV_OPA_COVER;
    dsc.color = base;
    glass_gradient(&dsc.grad, family, base, true, false);
    lv_draw_fill(layer, &dsc, &area);
}

void draw_probe_frame(lv_obj_t* canvas, int32_t width, int32_t height) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    const lv_area_t whole{0, 0, width - 1, height - 1};
    fill_glass(&layer, whole, GlassFamily::Surface, lv_color_hex(0x2E3440));
    const int32_t cols = std::max<int32_t>(1, width / (kProbeButtonW + 8));
    for (int i = 0; i < kProbeButtons; ++i) {
        const int32_t x = (i % cols) * (kProbeButtonW + 8);
        const int32_t y = (i / cols) * (kProbeButtonH + 8);
        const lv_area_t btn{x, y, x + kProbeButtonW - 1, y + kProbeButtonH - 1};
        fill_glass(&layer, btn, GlassFamily::Raised, lv_color_hex(0x5E81AC));
    }
    lv_canvas_finish_layer(canvas, &layer);
}

} // namespace

double measure_glass_frame_ms(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    lv_draw_buf_t* buf =
        lv_draw_buf_create(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                           LV_COLOR_FORMAT_NATIVE, LV_STRIDE_AUTO);
    if (buf == nullptr) {
        spdlog::warn("[Glass] Probe could not allocate a {}x{} buffer", width, height);
        return 0;
    }
    // A hidden canvas on the system layer: the layer round-trip needs an object, and a hidden
    // one never reaches the display.
    lv_obj_t* canvas = lv_canvas_create(lv_layer_sys());
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN); // DECLARATIVE_OK: offscreen probe canvas
    lv_canvas_set_draw_buf(canvas, buf);

    std::array<double, kProbeRepetitions> ms{};
    for (double& m : ms) {
        const auto start = std::chrono::steady_clock::now();
        draw_probe_frame(canvas, width, height);
        m = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buf);

    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

} // namespace helix::ui
