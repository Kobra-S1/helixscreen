// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Layout tests for ActionPromptModal's regular-button row.
//
// choose_button_row_layout() picks the row's layout: <= 3 buttons stay
// content-sized and wrap; 4 whose labels fit an even share make one
// equal-width row (AFC's "Lane 1".."Lane 4", #1043); 5 or more labelled
// buttons make a grid of equal cells, three across or two when the widest
// label does not fit a third. Text and buttons scroll together in
// prompt_scroll while the footer stays on the card. The invariant these tests
// defend is that no label is ever clipped and the footer never leaves the card.
//
// Tagged [ui_integration] (NOT hidden) — it shows real widgets and forces a
// real LVGL layout pass, which is the only way the wrap actually manifests.

#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "display_settings_manager.h"
#include "lvgl/src/misc/lv_area_private.h" // lv_area_is_in
#include "theme_manager.h"

#include <algorithm>
#include <vector>

#include "../catch_amalgamated.hpp"

// White-box accessor (declared friend in action_prompt_modal.h) — avoids adding
// _for_testing() methods to the production class ([L065]/[L088]).
struct ActionPromptModalTestAccess {
    static const std::vector<lv_obj_t*>& buttons(const helix::ui::ActionPromptModal& m) {
        return m.created_buttons_;
    }
};

namespace {

helix::PromptData make_regular_prompt(int n_buttons) {
    helix::PromptData data;
    data.title = "Select Lane";
    data.text_lines.push_back("Pick a lane to calibrate");
    for (int i = 0; i < n_buttons; ++i) {
        helix::PromptButton btn;
        btn.label = "Lane " + std::to_string(i + 1);
        btn.gcode = "AFC_CALIBRATION LANE=lane" + std::to_string(i + 1);
        btn.color = "primary";
        btn.is_footer = false;
        data.buttons.push_back(std::move(btn));
    }
    return data;
}

// Z-Mod's color picker as its CHANGE_ZCOLOR macro sends it (#1806): a text
// line, 24 label-less swatches and a footer Cancel.
helix::PromptData make_zmod_color_prompt() {
    using helix::ActionPromptManager;
    static const char* const kHex[24] = {
        "ffffff", "fff245", "def578", "21cc3d", "167a4b", "156682", "24e4a0", "7bd9f0",
        "4caaf8", "2e54dd", "48358c", "a341f7", "f435f6", "d584de", "fa6173", "f82d29",
        "805003", "f9903b", "fcebd7", "d5c5a1", "b17c38", "8c8c89", "bebebe", "1b1b1b"};
    helix::PromptData data;
    data.title = "Select color";
    data.text_lines.push_back("Spool 1: PETG-CF/");
    for (const char* hex : kHex) {
        data.buttons.push_back(ActionPromptManager::parse_button_spec(
            std::string("_ |CHANGE_ZCOLOR SLOT=1 HEX=") + hex + "|" + hex));
    }
    data.buttons.push_back(ActionPromptManager::parse_button_spec("Cancel|RESPOND|primary"));
    data.buttons.back().is_footer = true;
    return data;
}

// Drives the display to a size and refreshes the layout tokens for a scope.
class ScopedGeometry {
  public:
    ScopedGeometry(int32_t w, int32_t h)
        : disp_(lv_display_get_default()), w0_(lv_display_get_horizontal_resolution(disp_)),
          h0_(lv_display_get_vertical_resolution(disp_)) {
        lv_display_set_resolution(disp_, w, h);
        theme_manager_refresh_layout_constants(disp_);
    }
    ~ScopedGeometry() {
        lv_display_set_resolution(disp_, w0_, h0_);
        theme_manager_refresh_layout_constants(disp_);
    }
    ScopedGeometry(const ScopedGeometry&) = delete;
    ScopedGeometry& operator=(const ScopedGeometry&) = delete;

  private:
    lv_display_t* disp_;
    int32_t w0_;
    int32_t h0_;
};

// AFC_CALIBRATION's unit picker on a six-unit AFC, as
// AFC_functions.py#cmd_AFC_CALIBRATION sends it: one button per unit in
// alternating primary/secondary, then the "All Lanes" and Cancel footer buttons.
helix::PromptData make_afc_calibration_prompt() {
    using helix::ActionPromptManager;
    static const char* const kUnits[] = {"Turtle_1",        "Vivid_1", "EMU_1",
                                         "HTLF_claymore_1", "AMS_1",   "Ace2_1"};
    helix::PromptData data;
    data.title = "AFC Calibration";
    data.text_lines.push_back(
        "The following prompts will lead you through the calibration of your AFC unit(s). "
        "First, select a unit to calibrate. *All values will be automatically updated in the "
        "appropriate config sections.");
    int index = 0;
    for (const char* unit : kUnits) {
        data.buttons.push_back(ActionPromptManager::parse_button_spec(
            std::string(unit) + "|UNIT_CALIBRATION UNIT=" + unit + "|" +
            (index++ % 2 == 0 ? "primary" : "secondary")));
    }
    data.buttons.push_back(
        ActionPromptManager::parse_button_spec("All Lanes in all units|ALL_CALIBRATION|secondary"));
    data.buttons.back().is_footer = true;
    data.buttons.push_back(ActionPromptManager::parse_button_spec(
        "Cancel|RESPOND TYPE=command MSG=action:prompt_end|warning"));
    data.buttons.back().is_footer = true;
    return data;
}

class ActionPromptLayoutFixture : public LVGLUITestFixture {
  public:
    ActionPromptLayoutFixture() {
        prev_animations_ = helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
        modal_.set_gcode_callback([](const std::string&) { /* no-op */ });
    }
    ~ActionPromptLayoutFixture() override {
        helix::DisplaySettingsManager::instance().set_animations_enabled(prev_animations_);
    }

    helix::ui::ActionPromptModal modal_;
    bool prev_animations_ = true;
};

} // namespace

// ----------------------------------------------------------------------------
// 4 regular buttons must share a single row (the R2 bug case).
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture, "ActionPromptModal: 4 regular buttons fit on one row",
                 "[action_prompt][layout][ui_integration]") {
    auto data = make_regular_prompt(4);
    REQUIRE(modal_.show_prompt(test_screen(), data));

    // Force a synchronous layout pass on the whole tree so geometry settles.
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 4);

    // All four buttons must be on the SAME visual row. If the 4th wraps, its y
    // jumps down by ~one button-height (>= ~48px) — far outside this tolerance.
    int32_t y0 = lv_obj_get_y(btns[0]);
    for (size_t i = 1; i < btns.size(); ++i) {
        int32_t yi = lv_obj_get_y(btns[i]);
        INFO("button[" << i << "] y=" << yi << " vs button[0] y=" << y0);
        REQUIRE(std::abs(yi - y0) <= 4);
    }

    modal_.hide();
}

// ----------------------------------------------------------------------------
// Regression: 3 regular buttons still share one row AND stay content-sized
// (NOT forced to equal width). Guards against regressing the <= 3 path.
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: 3 regular buttons stay content-sized on one row",
                 "[action_prompt][layout][ui_integration]") {
    // Short labels of unequal length: they fit on one content-sized row (as the
    // legacy <= 3 path always has), and content-sizing yields unequal widths —
    // the >= 4 equal-width path would instead force them identical.
    helix::PromptData data;
    data.title = "Confirm";
    data.text_lines.push_back("Choose an action");
    data.buttons.push_back({"OK", "RESUME", "primary", "", false, -1});
    data.buttons.push_back({"Retry", "RETRY", "secondary", "", false, -1});
    data.buttons.push_back({"Cancel", "CANCEL", "error", "", false, -1});

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 3);

    // One row: all share (approximately) the same y.
    int32_t y0 = lv_obj_get_y(btns[0]);
    for (size_t i = 1; i < btns.size(); ++i) {
        REQUIRE(std::abs(lv_obj_get_y(btns[i]) - y0) <= 4);
    }

    // Content-sized: the short and long labels must NOT be equal width.
    int32_t w_ok = lv_obj_get_width(btns[0]);
    int32_t w_long = lv_obj_get_width(btns[1]);
    INFO("w(OK)=" << w_ok << " w(Retry the whole operation)=" << w_long);
    REQUIRE(w_long > w_ok);

    modal_.hide();
}

// ----------------------------------------------------------------------------
// The real AFC calibration prompt: four short lane buttons plus a longer
// "Calibrate All". Whichever layout the fit check picks, no label may be
// clipped — that is the invariant, not the row count.
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: AFC calibration buttons are never clipped",
                 "[action_prompt][layout][ui_integration]") {
    // Mirrors src/printer/ams_backend_mock.cpp's simulated AFC_CALIBRATION prompt.
    helix::PromptData data;
    data.title = "AFC Calibration";
    data.text_lines.push_back("Select a lane to calibrate, or calibrate all lanes.");
    for (int i = 1; i <= 4; ++i) {
        data.buttons.push_back({"Lane " + std::to_string(i),
                                "AFC_CALIBRATION LANE=lane" + std::to_string(i), "primary", "",
                                false, -1});
    }
    data.buttons.push_back({"Calibrate All", "AFC_CALIBRATION ALL=1", "secondary", "", false, -1});
    data.buttons.push_back({"Cancel", "AFC_CALIBRATION CANCEL=1", "error", "", true, -1});

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 6); // 5 regular + 1 footer

    // Every regular button must be able to show its label in full.
    for (size_t i = 0; i < 5; ++i) {
        lv_obj_t* label = lv_obj_get_child(btns[i], 0);
        REQUIRE(label != nullptr);
        INFO("button[" << i << "] \"" << data.buttons[i].label << "\" width="
                       << lv_obj_get_width(btns[i]) << " label width=" << lv_obj_get_width(label));
        REQUIRE(lv_obj_get_width(btns[i]) >= lv_obj_get_width(label));
    }

    modal_.hide();
}

// ----------------------------------------------------------------------------
// Many buttons with labels too long to share one row must WRAP, not clip.
//
// The >= 4 rule above is a button *count* test, but the property that actually
// matters is whether the labels fit. A preheat macro that emits 7 material
// presets ("PLA 220/60" ...) gets ~40px per equal-width cell on a 480px card,
// so every label is clipped to a couple of glyphs. Wrapping is the correct
// answer once the row genuinely cannot hold them.
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: long labels spread over several lines instead of clipping",
                 "[action_prompt][layout][ui_integration]") {
    helix::PromptData data;
    data.title = "Preheat for Load";
    data.text_lines.push_back("Preheat filament and choose a material");
    for (const char* label : {"PLA 220/60", "PETG 240/80", "ABS 250/100", "ASA 260/100",
                              "TPU 230/50", "PC 280/110", "Nylon 260/80"}) {
        data.buttons.push_back(
            {label, std::string("SET_MATERIAL M=") + label, "primary", "", false, -1});
    }

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 7);

    // The property under test: every button is wide enough to actually show its
    // label. Under the equal-width single-row layout each cell is far narrower
    // than its text, which is exactly the reported clipping.
    for (size_t i = 0; i < btns.size(); ++i) {
        lv_obj_t* label = lv_obj_get_child(btns[i], 0);
        REQUIRE(label != nullptr);
        int32_t w_btn = lv_obj_get_width(btns[i]);
        int32_t w_lbl = lv_obj_get_width(label);
        INFO("button[" << i << "] \"" << data.buttons[i].label << "\" width=" << w_btn
                       << " label width=" << w_lbl);
        REQUIRE(w_btn >= w_lbl);
    }

    // Not fitting on one row is the direct consequence: they must occupy at
    // least two distinct rows rather than being squeezed into one.
    int32_t y0 = lv_obj_get_y(btns[0]);
    bool any_wrapped = false;
    for (size_t i = 1; i < btns.size(); ++i) {
        if (std::abs(lv_obj_get_y(btns[i]) - y0) > 4) {
            any_wrapped = true;
        }
    }
    REQUIRE(any_wrapped);

    modal_.hide();
}

// A hex in the third field (or the legacy fourth) fills the button with that
// color; a field that is not a color leaves the themed default
// (prestonbrown/helixscreen#1806).
TEST_CASE_METHOD(ActionPromptLayoutFixture, "ActionPromptModal: hex button fills with the color",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    using helix::ActionPromptManager;
    helix::PromptData data;
    data.title = "Colors";
    data.buttons = {
        ActionPromptManager::parse_button_spec("Standard|GCODE_CMD_1|error"),
        ActionPromptManager::parse_button_spec("HEX Dark (White Text)|GCODE_CMD_2|ff5722"),
        ActionPromptManager::parse_button_spec("HEX Light (Black Text)|GCODE_CMD_3|#e0e0e0"),
        ActionPromptManager::parse_button_spec("Legacy|GCODE_CMD_4|primary|21cc3d"),
        ActionPromptManager::parse_button_spec("Garbage|GCODE_CMD_5|zzzzzz"),
    };

    REQUIRE(modal_.show_prompt(test_screen(), data));
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 5);

    auto bg = [&](size_t i) { return lv_obj_get_style_bg_color(btns[i], LV_PART_MAIN); };
    CHECK(lv_color_eq(bg(0), theme_manager_get_color("danger")));
    CHECK(lv_color_eq(bg(1), lv_color_hex(0xff5722)));
    CHECK(lv_color_eq(bg(2), lv_color_hex(0xe0e0e0)));
    CHECK(lv_color_eq(bg(3), lv_color_hex(0x21cc3d)));
    CHECK(lv_color_eq(bg(4), theme_manager_get_color("primary")));

    modal_.hide();
}

// A Z-Mod swatch tile carries no text, so it needs an outline to stay visible
// when its color matches the card behind it.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: a label-less hex button is a swatch",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    using helix::ActionPromptManager;
    helix::PromptData data;
    data.title = "Select color";
    data.buttons = {
        ActionPromptManager::parse_button_spec(
            "_ |CHANGE_ZCOLOR SLOT=1 TYPE=PLA HEX=ffffff HIDE=0|ffffff"),
        ActionPromptManager::parse_button_spec("Named|GCODE|e0e0e0"),
    };

    REQUIRE(modal_.show_prompt(test_screen(), data));
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 2);

    CHECK(lv_color_eq(lv_obj_get_style_bg_color(btns[0], LV_PART_MAIN), lv_color_hex(0xffffff)));
    CHECK(lv_obj_get_style_border_width(btns[0], LV_PART_MAIN) > 0);
    CHECK(lv_obj_get_style_border_width(btns[1], LV_PART_MAIN) == 0);

    modal_.hide();
}

// 24 swatches cannot share one row as touch targets, so the picker wraps into
// a grid of square tiles and the footer stays on the card.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: a Z-Mod color picker wraps into a grid of touch targets",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    REQUIRE(modal_.show_prompt(test_screen(), make_zmod_color_prompt()));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 25);
    const int32_t side = theme_manager_get_spacing("button_height_sm");
    REQUIRE(side > 0);

    lv_area_t card;
    lv_obj_get_coords(dialog, &card);
    bool wrapped = false;
    const int32_t y0 = lv_obj_get_y(btns[0]);
    for (size_t i = 0; i < 24; ++i) {
        lv_area_t a;
        lv_obj_get_coords(btns[i], &a);
        CAPTURE(i, lv_area_get_width(&a), lv_area_get_height(&a));
        CHECK(lv_area_get_width(&a) == side);
        CHECK(lv_area_get_height(&a) == side);
        CHECK(a.x1 >= card.x1);
        CHECK(a.x2 <= card.x2);
        CHECK(a.y2 <= card.y2);
        if (lv_obj_get_y(btns[i]) != y0) {
            wrapped = true;
        }
    }
    CHECK(wrapped);

    lv_area_t footer;
    lv_obj_get_coords(btns[24], &footer);
    CHECK(footer.y1 >= card.y1);
    CHECK(footer.y2 <= card.y2);

    modal_.hide();
}

// ----------------------------------------------------------------------------
// choose_button_row_layout(): the row layout decision, without widgets
// ----------------------------------------------------------------------------
TEST_CASE("choose_button_row_layout picks wrap, one row or a grid", "[action_prompt][layout]") {
    using helix::ui::ButtonRowKind;
    using helix::ui::choose_button_row_layout;
    // A 480px card's button row: 448px across, 8px gaps, 8px cell padding.
    helix::ui::ButtonRowMeasure m;
    m.available = 448;
    m.gap = 8;
    m.cell_pad = 8;
    m.widest_label = 60;
    m.min_cell = 32;

    SECTION("three or fewer wrap") {
        m.count = 3;
        CHECK(choose_button_row_layout(m).kind == ButtonRowKind::Wrap);
    }
    SECTION("four that fit an even share make one row") {
        m.count = 4;
        const auto layout = choose_button_row_layout(m);
        CHECK(layout.kind == ButtonRowKind::EqualRow);
        CHECK(layout.cols == 4);
    }
    SECTION("four that do not fit an even share wrap") {
        m.count = 4;
        m.widest_label = 110; // + 16 padding > (448 - 24) / 4 = 106
        CHECK(choose_button_row_layout(m).kind == ButtonRowKind::Wrap);
    }
    SECTION("five labelled buttons make a three-column grid") {
        m.count = helix::ui::kPromptGridMinButtons;
        const auto layout = choose_button_row_layout(m);
        CHECK(layout.kind == ButtonRowKind::Grid);
        CHECK(layout.cols == 3);
    }
    SECTION("a label too wide for a third drops the grid to two columns") {
        m.count = 6;
        m.widest_label = 130; // + 16 > (448 - 16) / 3 = 144
        const auto layout = choose_button_row_layout(m);
        CHECK(layout.kind == ButtonRowKind::Grid);
        CHECK(layout.cols == 2);
    }
    SECTION("the widest label exactly filling a third keeps three columns") {
        m.count = 6;
        m.widest_label = 144 - 16;
        CHECK(choose_button_row_layout(m).cols == 3);
    }
    SECTION("a label too wide even for half still gets two columns, ellipsized") {
        m.count = 6;
        m.widest_label = 400;
        const auto layout = choose_button_row_layout(m);
        CHECK(layout.kind == ButtonRowKind::Grid);
        CHECK(layout.cols == 2);
    }
    SECTION("a row of swatches stays one line of tiles") {
        m.count = 6;
        m.all_swatches = true;
        m.widest_label = 0;
        const auto layout = choose_button_row_layout(m);
        CHECK(layout.kind == ButtonRowKind::EqualRow);
        CHECK(layout.cols == 6);
    }
    SECTION("swatches too many for touch-sized cells wrap") {
        m.count = 24;
        m.all_swatches = true;
        m.widest_label = 0; // (448 - 184) / 24 = 11 < 32
        CHECK(choose_button_row_layout(m).kind == ButtonRowKind::Wrap);
    }
    SECTION("an unmeasured row wraps") {
        m.count = 6;
        m.available = 0;
        CHECK(choose_button_row_layout(m).kind == ButtonRowKind::Wrap);
    }
}

// ----------------------------------------------------------------------------
// AFC_CALIBRATION on a six-unit AFC: a grid of unit buttons, every one
// reachable by scrolling, and both footer buttons on the card.
// ----------------------------------------------------------------------------
namespace {

lv_area_t coords_of(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a;
}

void check_afc_calibration_fits(helix::ui::ActionPromptModal& modal, lv_obj_t* screen) {
    REQUIRE(modal.show_prompt(screen, make_afc_calibration_prompt()));
    lv_obj_t* dialog = modal.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(screen);

    const auto& btns = ActionPromptModalTestAccess::buttons(modal);
    REQUIRE(btns.size() == 8); // six units + two footer buttons
    const lv_area_t card = coords_of(dialog);
    const lv_area_t screen_area = coords_of(screen);
    CHECK(lv_area_is_in(&card, &screen_area, 0));

    // The footer never leaves the card.
    for (size_t i = 6; i < 8; ++i) {
        const lv_area_t footer = coords_of(btns[i]);
        CAPTURE(i, card.y2, footer.y2);
        CHECK(lv_area_is_in(&footer, &card, 0));
    }

    // A grid: equal cells, at most three to a line, the first line full.
    const int32_t w0 = lv_obj_get_width(btns[0]);
    std::vector<int32_t> line_ys;
    for (size_t i = 0; i < 6; ++i) {
        CAPTURE(i, lv_obj_get_width(btns[i]), w0);
        CHECK(lv_obj_get_width(btns[i]) == w0);
        const int32_t y = coords_of(btns[i]).y1;
        if (std::find(line_ys.begin(), line_ys.end(), y) == line_ys.end()) {
            line_ys.push_back(y);
        }
    }
    CHECK(line_ys.size() >= 2);
    CHECK(line_ys.size() <= 3);

    // Every unit button can be scrolled fully into the visible scroll area.
    lv_obj_t* scroll = lv_obj_find_by_name(dialog, "prompt_scroll");
    REQUIRE(scroll != nullptr);
    for (size_t i = 0; i < 6; ++i) {
        lv_obj_scroll_to_view_recursive(btns[i], LV_ANIM_OFF);
        lv_obj_update_layout(screen);
        const lv_area_t area = coords_of(scroll);
        const lv_area_t btn = coords_of(btns[i]);
        CAPTURE(i, area.y1, area.y2, btn.y1, btn.y2);
        CHECK(lv_area_is_in(&btn, &area, 0));
        CHECK(lv_area_is_in(&btn, &card, 0));
    }
    modal.hide();
}

} // namespace

TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: six-unit AFC calibration fits a 480x272 card",
                 "[action_prompt][layout][ui_integration]") {
    ScopedGeometry micro(480, 272);
    check_afc_calibration_fits(modal_, test_screen());
}

TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: six-unit AFC calibration fits an 800x480 card",
                 "[action_prompt][layout][ui_integration]") {
    ScopedGeometry medium(800, 480);
    check_afc_calibration_fits(modal_, test_screen());
}
