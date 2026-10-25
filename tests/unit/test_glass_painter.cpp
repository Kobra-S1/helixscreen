// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_swatch.h"

#include "../lvgl_test_fixture.h"
#include "glass_painter.h"
#include "glass_shading.h"
#include "theme_manager.h"

#include <cstring>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::glass_family_of;
using helix::ui::glass_mark;
using helix::ui::GlassFamily;

namespace {

bool same(lv_color_t a, lv_color_t b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

lv_style_t* role(StyleRole r) {
    return ThemeManager::instance().get_style(r);
}

/// The object's pixels as LVGL renders them now.
std::vector<uint8_t> snapshot(lv_obj_t* obj) {
    lv_draw_buf_t* snap = lv_snapshot_take(obj, LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snap != nullptr);
    std::vector<uint8_t> px(snap->data, snap->data + snap->header.stride * snap->header.h);
    lv_draw_buf_destroy(snap);
    return px;
}

/// A named comparison, so a failure prints a bool rather than every pixel.
bool same_pixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return a == b;
}

lv_obj_t* make_card(lv_obj_t* parent) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, role(StyleRole::Card), LV_PART_MAIN);
    lv_obj_set_size(card, 200, 120);
    lv_obj_update_layout(card);
    return card;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "glass_family_of reads the shared theme style", "[glass]") {
    ThemeManager::instance().init();

    lv_obj_t* primary = lv_button_create(test_screen());
    lv_obj_add_style(primary, role(StyleRole::ButtonPrimary), LV_PART_MAIN);
    CHECK(glass_family_of(primary, LV_PART_MAIN) == GlassFamily::Raised);

    lv_obj_t* default_button = lv_button_create(test_screen());
    lv_obj_add_style(default_button, role(StyleRole::Button), LV_PART_MAIN);
    CHECK(glass_family_of(default_button, LV_PART_MAIN) == GlassFamily::Raised);

    for (StyleRole flat :
         {StyleRole::ButtonGhost, StyleRole::ButtonOutline, StyleRole::ButtonTransparent}) {
        CAPTURE(static_cast<int>(flat));
        lv_obj_t* b = lv_button_create(test_screen());
        lv_obj_add_style(b, role(flat), LV_PART_MAIN);
        CHECK(glass_family_of(b, LV_PART_MAIN) == GlassFamily::None);
    }

    lv_obj_t* card = lv_obj_create(test_screen());
    lv_obj_add_style(card, role(StyleRole::Card), LV_PART_MAIN);
    CHECK(glass_family_of(card, LV_PART_MAIN) == GlassFamily::Surface);

    lv_obj_t* dialog = lv_obj_create(test_screen());
    lv_obj_add_style(dialog, role(StyleRole::Dialog), LV_PART_MAIN);
    CHECK(glass_family_of(dialog, LV_PART_MAIN) == GlassFamily::Surface);

    lv_obj_t* plain = lv_obj_create(test_screen());
    CHECK(glass_family_of(plain, LV_PART_MAIN) == GlassFamily::None);

    SECTION("a marker wins over the styles") {
        glass_mark(primary, GlassFamily::None);
        CHECK(glass_family_of(primary, LV_PART_MAIN) == GlassFamily::None);
        glass_mark(plain, GlassFamily::Surface);
        CHECK(glass_family_of(plain, LV_PART_MAIN) == GlassFamily::Surface);
        glass_mark(plain, GlassFamily::Recessed);
        CHECK(glass_family_of(plain, LV_PART_MAIN) == GlassFamily::Recessed);
    }
    SECTION("a style toggled off does not count") {
        lv_obj_t* toggled = lv_obj_create(test_screen());
        lv_obj_add_style(toggled, role(StyleRole::Card), LV_PART_MAIN);
        lv_obj_style_set_disabled(toggled, role(StyleRole::Card), LV_PART_MAIN, true);
        CHECK(glass_family_of(toggled, LV_PART_MAIN) == GlassFamily::None);
    }
}

TEST_CASE("glass_rewrite_fill uses the fill's own color", "[glass]") {
    lv_draw_fill_dsc_t dsc;
    lv_draw_fill_dsc_init(&dsc);
    dsc.opa = LV_OPA_COVER;
    dsc.color = lv_color_hex(0xFF00FF);
    helix::ui::glass_rewrite_fill(&dsc, GlassFamily::Raised, true, false);
    const helix::ui::GlassStops want =
        helix::ui::glass_stops(GlassFamily::Raised, lv_color_hex(0xFF00FF), true, false);
    CHECK(static_cast<int>(dsc.grad.dir) == LV_GRAD_DIR_VER);
    REQUIRE(dsc.grad.stops_count == want.count);
    CHECK(same(dsc.grad.stops[0].color, want.color[0]));
    CHECK(same(dsc.grad.stops[3].color, want.color[3]));

    lv_draw_fill_dsc_t pressed;
    lv_draw_fill_dsc_init(&pressed);
    pressed.opa = LV_OPA_COVER;
    pressed.color = lv_color_hex(0xFF00FF);
    helix::ui::glass_rewrite_fill(&pressed, GlassFamily::Raised, true, true);
    CHECK(pressed.grad.stops_count == 2);
}

TEST_CASE("glass_rewrite_fill leaves translucent fills and existing gradients alone", "[glass]") {
    lv_draw_fill_dsc_t translucent;
    lv_draw_fill_dsc_init(&translucent);
    translucent.opa = 128;
    translucent.color = lv_color_hex(0x3A7CC8);
    lv_draw_fill_dsc_t before = translucent;
    helix::ui::glass_rewrite_fill(&translucent, GlassFamily::Raised, true, false);
    CHECK(std::memcmp(&before, &translucent, sizeof(before)) == 0);

    lv_draw_fill_dsc_t graded;
    lv_draw_fill_dsc_init(&graded);
    graded.opa = LV_OPA_COVER;
    graded.color = lv_color_hex(0x3A7CC8);
    const lv_color_t two[2] = {lv_color_hex(0x112233), lv_color_hex(0x445566)};
    lv_grad_init_stops(&graded.grad, two, nullptr, nullptr, 2);
    lv_grad_horizontal_init(&graded.grad);
    before = graded;
    helix::ui::glass_rewrite_fill(&graded, GlassFamily::Surface, true, false);
    CHECK(std::memcmp(&before, &graded, sizeof(before)) == 0);
}

TEST_CASE("glass_rewrite_border draws the family's rim over the part's color", "[glass]") {
    const lv_color_t base = lv_color_hex(0x3A7CC8);
    lv_draw_border_dsc_t dsc;
    lv_draw_border_dsc_init(&dsc);
    dsc.color = lv_color_hex(0x000000);
    helix::ui::glass_rewrite_border(&dsc, GlassFamily::Raised, base, true);
    CHECK(same(dsc.color, helix::ui::glass_rim_color(GlassFamily::Raised, base, true)));

    dsc.color = lv_color_hex(0x123456);
    helix::ui::glass_rewrite_border(&dsc, GlassFamily::None, base, true);
    CHECK(same(dsc.color, lv_color_hex(0x123456)));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "glass painter renders a card differently, and detach restores it exactly",
                 "[glass]") {
    ThemeManager::instance().init();
    lv_obj_t* card = make_card(test_screen());

    const std::vector<uint8_t> flat = snapshot(card);
    helix::ui::glass_attach(card);
    REQUIRE(helix::ui::glass_attached(card));
    const std::vector<uint8_t> glass = snapshot(card);
    CHECK_FALSE(same_pixels(glass, flat));

    helix::ui::glass_detach(card);
    CHECK_FALSE(helix::ui::glass_attached(card));
    CHECK_FALSE(lv_obj_has_flag(card, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS));
    CHECK(same_pixels(snapshot(card), flat));
}

TEST_CASE_METHOD(LVGLTestFixture, "glass attach is idempotent and keeps a flag it did not set",
                 "[glass]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    const uint32_t events = lv_obj_get_event_count(obj);
    helix::ui::glass_attach(obj);
    helix::ui::glass_attach(obj);
    CHECK(lv_obj_get_event_count(obj) == events + 1);
    helix::ui::glass_detach(obj);
    CHECK(lv_obj_get_event_count(obj) == events);
    CHECK(lv_obj_has_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS));

    lv_obj_t* label = lv_label_create(test_screen());
    helix::ui::glass_attach(label);
    CHECK_FALSE(helix::ui::glass_attached(label));
}

TEST_CASE_METHOD(LVGLTestFixture, "glass shades a locally colored card and spares a marked one",
                 "[glass]") {
    ThemeManager::instance().init();
    ThemeManager::instance().set_dark_mode(true);
    lv_obj_t* swatch = make_card(test_screen());
    lv_obj_set_style_bg_color(swatch, lv_color_hex(0xD94848), LV_PART_MAIN);
    lv_obj_t* marked = make_card(test_screen());
    lv_obj_set_style_bg_color(marked, lv_color_hex(0xD94848), LV_PART_MAIN);
    glass_mark(marked, GlassFamily::None);

    const std::vector<uint8_t> swatch_flat = snapshot(swatch);
    const std::vector<uint8_t> marked_flat = snapshot(marked);
    helix::ui::glass_attach(swatch);
    helix::ui::glass_attach(marked);
    const std::vector<uint8_t> swatch_glass = snapshot(swatch);
    CHECK_FALSE(same_pixels(swatch_glass, swatch_flat));
    CHECK(same_pixels(snapshot(marked), marked_flat));

    // Near the glow's corner the fill is the local color lightened, not the theme's card color.
    const size_t stride = swatch_glass.size() / 120;
    const size_t at = 12 * stride + 12 * 4;
    const lv_color_t corner =
        lv_color_make(swatch_glass[at + 2], swatch_glass[at + 1], swatch_glass[at]);
    CAPTURE(int(corner.red), int(corner.green), int(corner.blue));
    CHECK(corner.red > 0xD9);
    CHECK(corner.red > corner.green + 0x40);
    CHECK(corner.green == corner.blue);
}

// ============================================================================
// Theme styles in glass mode
// ============================================================================

namespace {

/// Returns the theme to flat, dark mode on scope exit.
struct GlassThemeRestore {
    ~GlassThemeRestore() {
        ThemeManager::instance().set_glass(false);
        ThemeManager::instance().set_dark_mode(true);
    }
};

int32_t style_num(StyleRole r, lv_style_prop_t prop) {
    lv_style_value_t v{};
    if (lv_style_get_prop(role(r), prop, &v) != LV_STYLE_RES_FOUND) {
        return -1;
    }
    return v.num;
}

lv_color_t style_color(StyleRole r, lv_style_prop_t prop) {
    lv_style_value_t v{};
    lv_style_get_prop(role(r), prop, &v);
    return v.color;
}

/// Every property glass may touch, on every role it touches.
std::vector<int64_t> glass_touched_props() {
    std::vector<int64_t> out;
    for (StyleRole r :
         {StyleRole::Button, StyleRole::Card, StyleRole::Dialog, StyleRole::Pressed}) {
        for (lv_style_prop_t prop :
             {LV_STYLE_SHADOW_WIDTH, LV_STYLE_SHADOW_OFFSET_Y, LV_STYLE_SHADOW_OPA,
              LV_STYLE_BORDER_WIDTH, LV_STYLE_BORDER_OPA, LV_STYLE_RADIUS}) {
            out.push_back(style_num(r, prop));
        }
        for (lv_style_prop_t prop :
             {LV_STYLE_SHADOW_COLOR, LV_STYLE_BORDER_COLOR, LV_STYLE_BG_COLOR}) {
            out.push_back(static_cast<int64_t>(lv_color_to_u32(style_color(r, prop))));
        }
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "set_glass gives the shared styles glass shadows",
                 "[glass][theme]") {
    auto& tm = ThemeManager::instance();
    tm.init();
    GlassThemeRestore restore;
    tm.set_glass(false);
    tm.set_dark_mode(true);

    tm.set_glass(true);
    CHECK(tm.is_glass());
    CHECK(tm.current_palette().glass);
    CHECK(style_num(StyleRole::Button, LV_STYLE_SHADOW_WIDTH) == 12);
    CHECK(style_num(StyleRole::Button, LV_STYLE_SHADOW_OFFSET_Y) == 5);
    CHECK(style_num(StyleRole::Button, LV_STYLE_SHADOW_OPA) == 102);
    CHECK(style_num(StyleRole::Card, LV_STYLE_SHADOW_WIDTH) == 10);
    CHECK(style_num(StyleRole::Dialog, LV_STYLE_SHADOW_OFFSET_Y) == 4);
    CHECK(style_num(StyleRole::Card, LV_STYLE_BORDER_WIDTH) >= 1);
    CHECK(style_num(StyleRole::Pressed, LV_STYLE_SHADOW_WIDTH) == 6);
    CHECK(lv_color_to_u32(style_color(StyleRole::Card, LV_STYLE_SHADOW_COLOR)) ==
          lv_color_to_u32(lv_color_black()));
}

TEST_CASE_METHOD(LVGLTestFixture, "set_glass(false) restores exactly the flat styles",
                 "[glass][theme]") {
    auto& tm = ThemeManager::instance();
    tm.init();
    GlassThemeRestore restore;
    tm.set_glass(false);
    tm.set_dark_mode(true);
    const std::vector<int64_t> flat = glass_touched_props();

    tm.set_glass(true);
    REQUIRE(glass_touched_props() != flat);
    tm.set_glass(false);
    CHECK_FALSE(tm.is_glass());
    CHECK(glass_touched_props() == flat);
}

TEST_CASE_METHOD(LVGLTestFixture, "glass survives a dark/light switch and takes the light shadow",
                 "[glass][theme]") {
    auto& tm = ThemeManager::instance();
    tm.init();
    GlassThemeRestore restore;
    tm.set_dark_mode(true);
    tm.set_glass(true);

    tm.set_dark_mode(false);
    CHECK(tm.is_glass());
    CHECK(tm.current_palette().glass);
    CHECK_FALSE(tm.current_palette().dark);
    CHECK(lv_color_to_u32(style_color(StyleRole::Card, LV_STYLE_SHADOW_COLOR)) ==
          lv_color_to_u32(lv_color_hex(0x1E2846)));
    CHECK(style_num(StyleRole::Card, LV_STYLE_SHADOW_OPA) == 56);

    tm.set_palettes(tm.light_palette(), tm.dark_palette());
    CHECK(tm.is_glass());
    CHECK(style_num(StyleRole::Button, LV_STYLE_SHADOW_WIDTH) == 12);
}

// ============================================================================
// The rest of the chrome
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "glass: controls are recessed tracks with raised handles",
                 "[glass]") {
    for (lv_obj_t* control : {lv_switch_create(test_screen()), lv_slider_create(test_screen()),
                              lv_arc_create(test_screen()), lv_bar_create(test_screen())}) {
        CAPTURE(static_cast<const void*>(lv_obj_get_class(control)));
        CHECK(glass_family_of(control, LV_PART_MAIN) == GlassFamily::Recessed);
        CHECK(glass_family_of(control, LV_PART_INDICATOR) == GlassFamily::Raised);
        CHECK(glass_family_of(control, LV_PART_KNOB) == GlassFamily::Raised);
        CHECK(glass_family_of(control, LV_PART_SCROLLBAR) == GlassFamily::None);
    }

    lv_obj_t* checkbox = lv_checkbox_create(test_screen());
    CHECK(glass_family_of(checkbox, LV_PART_INDICATOR) == GlassFamily::Raised);
    CHECK(glass_family_of(checkbox, LV_PART_MAIN) == GlassFamily::None);

    lv_obj_t* keyboard = lv_keyboard_create(test_screen());
    CHECK(glass_family_of(keyboard, LV_PART_ITEMS) == GlassFamily::Raised);
    CHECK(glass_family_of(keyboard, LV_PART_MAIN) == GlassFamily::None);
    lv_obj_t* matrix = lv_buttonmatrix_create(test_screen());
    CHECK(glass_family_of(matrix, LV_PART_ITEMS) == GlassFamily::Raised);

    lv_obj_t* field = lv_textarea_create(test_screen());
    lv_obj_add_style(field, role(StyleRole::InputBg), LV_PART_MAIN);
    CHECK(glass_family_of(field, LV_PART_MAIN) == GlassFamily::Recessed);

    lv_obj_t* dropdown = lv_dropdown_create(test_screen());
    lv_dropdown_open(dropdown);
    lv_obj_t* list = lv_dropdown_get_list(dropdown);
    REQUIRE(list != nullptr);
    CHECK(glass_family_of(list, LV_PART_MAIN) == GlassFamily::Surface);
    lv_dropdown_close(dropdown);

    SECTION("an exempt marker covers every part") {
        lv_obj_t* slider = lv_slider_create(test_screen());
        glass_mark(slider, GlassFamily::None);
        CHECK(glass_family_of(slider, LV_PART_MAIN) == GlassFamily::None);
        CHECK(glass_family_of(slider, LV_PART_KNOB) == GlassFamily::None);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "glass: content and swatches stay flat", "[glass]") {
    lv_obj_t* image = lv_image_create(test_screen());
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    lv_obj_t* chart = lv_chart_create(test_screen());
    lv_obj_t* label = lv_label_create(test_screen());
    for (lv_obj_t* content : {image, canvas, chart, label}) {
        CAPTURE(static_cast<const void*>(lv_obj_get_class(content)));
        for (lv_part_t part : {LV_PART_MAIN, LV_PART_ITEMS, LV_PART_INDICATOR}) {
            CHECK(glass_family_of(content, part) == GlassFamily::None);
        }
    }

    // A swatch painted on a raised button still reads as its filament color.
    lv_obj_t* swatch = lv_button_create(test_screen());
    lv_obj_add_style(swatch, role(StyleRole::Button), LV_PART_MAIN);
    REQUIRE(glass_family_of(swatch, LV_PART_MAIN) == GlassFamily::Raised);
    helix::ui::apply_swatch_color(swatch, 0xD94848, "");
    CHECK(glass_family_of(swatch, LV_PART_MAIN) == GlassFamily::None);
}

TEST_CASE_METHOD(LVGLTestFixture, "glass: an outlined part keeps its own border", "[glass]") {
    ThemeManager::instance().init();
    ThemeManager::instance().set_dark_mode(true);
    lv_obj_t* outlined = make_card(test_screen());
    lv_obj_set_style_bg_opa(outlined, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(outlined, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(outlined, lv_color_hex(0x00FF00), LV_PART_MAIN);
    const std::vector<uint8_t> flat = snapshot(outlined);
    helix::ui::glass_attach(outlined);
    CHECK(same_pixels(snapshot(outlined), flat));
}
