// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A toolhead glyph's bounds hold what its renderer draws, for every style: the
// PARALLEL path's head hit-test is the glyph, so a loose box takes taps meant
// for the tube and a tight one drops taps on the head.

#include "../lvgl_test_fixture.h"
#include "nozzle_renderer_dispatch.h"
#include "src/ui/ui_filament_path_internal.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using helix::ToolheadStyle;
using namespace helix::ui::fpath;

namespace {

using DrawFn = void (*)(lv_layer_t*, int32_t, int32_t, lv_color_t, int32_t, lv_opa_t);

struct StyleCase {
    ToolheadStyle style;
    const char* name;
    DrawFn draw;
};

const StyleCase STYLES[] = {
    {ToolheadStyle::DEFAULT, "default", draw_nozzle_bambu},
    {ToolheadStyle::A4T, "a4t", draw_nozzle_a4t},
    {ToolheadStyle::ANTHEAD, "anthead", draw_nozzle_anthead},
    {ToolheadStyle::JABBERWOCKY, "jabberwocky", draw_nozzle_jabberwocky},
    {ToolheadStyle::STEALTHBURNER, "stealthburner", draw_nozzle_stealthburner},
    {ToolheadStyle::CREALITY_K1, "k1", draw_nozzle_creality_k1},
    {ToolheadStyle::CREALITY_K2, "k2", draw_nozzle_creality_k2},
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Toolhead bounds hold every style's drawn pixels",
                 "[filament-path][toolhead_tap]") {
    lv_draw_buf_t* buf = lv_draw_buf_create(400, 400, LV_COLOR_FORMAT_ARGB8888, 0);
    REQUIRE(buf != nullptr);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    lv_canvas_set_draw_buf(canvas, buf);

    for (const StyleCase& sc : STYLES) {
        for (int32_t s : {6, 10, 16}) {
            lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
            lv_layer_t layer;
            lv_canvas_init_layer(canvas, &layer);
            sc.draw(&layer, 200, 200, lv_color_hex(0x808080), s, LV_OPA_COVER);
            lv_canvas_finish_layer(canvas, &layer);

            int32_t x1 = 400, y1 = 400, x2 = -1, y2 = -1;
            for (int32_t y = 0; y < 400; y++) {
                const uint8_t* row = buf->data + y * buf->header.stride;
                for (int32_t x = 0; x < 400; x++) {
                    if (row[x * 4 + 3] > 8) {
                        x1 = std::min(x1, x);
                        x2 = std::max(x2, x);
                        y1 = std::min(y1, y);
                        y2 = std::max(y2, y);
                    }
                }
            }
            CAPTURE(sc.name, s, x1, y1, x2, y2);
            REQUIRE(x2 >= 0);
            const GlyphBounds g = toolhead_bounds(sc.style, 200, 200, s);
            CAPTURE(g.left, g.top, g.right, g.bottom);
            // Drawn pixels inside the bounds, and the bounds no more than two
            // pixels outside them.
            CHECK(x1 >= g.left - 1);
            CHECK(x2 <= g.right + 1);
            CHECK(y1 >= g.top - 1);
            CHECK(y2 <= g.bottom + 1);
            CHECK(g.left >= x1 - 2);
            CHECK(g.right <= x2 + 2);
            CHECK(g.top >= y1 - 2);
            CHECK(g.bottom <= y2 + 2);
        }
    }
    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buf);
}
