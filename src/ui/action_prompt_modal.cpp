// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "action_prompt_modal.h"

#include "ui_afc_fault_path.h"
#include "ui_error_reporting.h"
#include "ui_swatch.h"

#include "color_utils.h"
#include "sound_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace helix::ui {

namespace {

/**
 * @brief Width the button label needs to render in full, in pixels.
 */
int32_t label_width_px(const std::string& label, const lv_font_t* font) {
    lv_point_t size{};
    lv_text_get_size(&size, label.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/**
 * @brief A color tile with no text: Z-Mod's color-picker swatch (#1806).
 */
bool is_swatch(const PromptButton& btn) {
    return btn.label.empty() && helix::parse_hex_color(btn.hex_color).has_value();
}

/**
 * @brief Measure the button row for choose_button_row_layout().
 *
 * The row is width=100% of a card whose own width is a percentage of the
 * screen, so its content width is only real after a layout pass. A row that
 * cannot be measured yet reports available 0, which wraps: the one layout that
 * never clips.
 */
ButtonRowMeasure measure_button_row(lv_obj_t* row, const std::vector<PromptButton>& buttons) {
    ButtonRowMeasure m;
    bool any = false;
    bool all_swatches = true;
    const lv_font_t* font = theme_manager_get_font("font_body");
    for (const auto& btn : buttons) {
        if (btn.is_footer) {
            continue;
        }
        ++m.count;
        any = true;
        all_swatches = all_swatches && is_swatch(btn);
        if (font != nullptr) {
            m.widest_label = std::max(m.widest_label, label_width_px(btn.label, font));
        }
    }
    m.all_swatches = any && all_swatches;
    if (font == nullptr) {
        return m;
    }
    lv_obj_update_layout(row);
    m.available = lv_obj_get_content_width(row);
    m.gap = lv_obj_get_style_pad_column(row, LV_PART_MAIN);
    m.cell_pad = theme_manager_get_spacing("space_sm"); // per side, see create_button()
    // A cell narrower than the smallest button is no longer a touch target:
    // Z-Mod's 24-swatch color picker would become a row of slivers.
    m.min_cell = theme_manager_get_spacing("button_height_sm");
    return m;
}

} // namespace

ButtonRowLayout choose_button_row_layout(const ButtonRowMeasure& m) {
    if (m.available <= 0 || m.count <= 0) {
        return {};
    }
    const auto cell_of = [&m](int32_t cols) { return (m.available - m.gap * (cols - 1)) / cols; };
    const auto fits_share = [&m, &cell_of](int32_t cols) {
        return m.widest_label + 2 * m.cell_pad <= cell_of(cols);
    };
    if (!m.all_swatches && m.count >= kPromptGridMinButtons) {
        return {ButtonRowKind::Grid, fits_share(3) ? 3 : 2};
    }
    if (m.count >= 4 && fits_share(m.count) && cell_of(m.count) >= m.min_cell) {
        return {ButtonRowKind::EqualRow, m.count};
    }
    return {};
}

// ============================================================================
// Construction / Destruction
// ============================================================================

ActionPromptModal::ActionPromptModal() {
    spdlog::debug("[ActionPromptModal] Constructed");
}

ActionPromptModal::~ActionPromptModal() {
    // Modal destructor will call hide() if visible, which invalidates lifetime_
    // Note: No spdlog here - logger may be destroyed before us during shutdown [L010]
}

// ============================================================================
// Public API
// ============================================================================

void ActionPromptModal::set_gcode_callback(GcodeCallback callback) {
    gcode_callback_ = std::move(callback);
}

bool ActionPromptModal::show_prompt(lv_obj_t* parent, const PromptData& data) {
    prompt_data_ = data;
    // Before Modal::show(): the diagram binds its subject when the XML is built.
    publish_fault_path();

    if (!Modal::show(parent)) {
        restore_fault_path();
        return false;
    }

    spdlog::info("[ActionPromptModal] Shown with title: {}", prompt_data_.title);
    return true;
}

// ============================================================================
// Modal Hooks
// ============================================================================

void ActionPromptModal::on_show() {
    populate_content();
}

void ActionPromptModal::publish_fault_path() {
    std::string text;
    for (const auto& line : prompt_data_.text_lines) {
        text += line;
        text += '\n';
    }
    // A prompt re-shown while open keeps the value it found the first time.
    if (!fault_path_held_) {
        fault_path_before_ = afc_fault_path_current();
        fault_path_held_ = true;
    }
    const auto segment = afc_fault_path_publish(text);
    fault_path_published_ = segment ? static_cast<int>(*segment) : 0;
}

void ActionPromptModal::restore_fault_path() {
    if (fault_path_held_) {
        afc_fault_path_restore(fault_path_published_, fault_path_before_);
        fault_path_held_ = false;
    }
}

void ActionPromptModal::on_hide() {
    clear_dynamic_content();
    restore_fault_path();
    spdlog::debug("[ActionPromptModal] on_hide()");
}

// ============================================================================
// Content Population
// ============================================================================

void ActionPromptModal::populate_content() {
    if (!dialog_) {
        return;
    }

    // Set title. kModalTitleWidgetName is the canonical name ui_notification.cpp's
    // duplicate-title suppression looks up, so every modal that can sit on top
    // of the stack must expose its title under it (#1389).
    lv_obj_t* title_label = find_widget(kModalTitleWidgetName);
    if (title_label) {
        lv_label_set_text(title_label, prompt_data_.title.c_str());
    }

    // Error-severity affordance: show the red error icon only for severity "error".
    // The modal instance is reused across shows, so reset both ways to clear any
    // prior error state on a subsequent neutral (action:prompt) show.
    if (lv_obj_t* err_icon = find_widget("icon_error")) {
        if (prompt_data_.severity == "error") {
            lv_obj_remove_flag(err_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(err_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Create text lines
    create_text_lines();

    // Create buttons
    create_buttons();
}

void ActionPromptModal::create_text_lines() {
    lv_obj_t* content_container = find_widget("content_container");
    if (!content_container) {
        spdlog::warn("[ActionPromptModal] content_container not found");
        return;
    }

    // Create a label for each text line
    for (const auto& line : prompt_data_.text_lines) {
        lv_obj_t* label = lv_label_create(content_container);
        lv_label_set_text(label, line.c_str());
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

        // Apply body text styling
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), LV_PART_MAIN);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), LV_PART_MAIN);

        created_text_labels_.push_back(label);
    }

    // Hide content container if no text lines
    if (prompt_data_.text_lines.empty()) {
        lv_obj_add_flag(content_container, LV_OBJ_FLAG_HIDDEN);
    }
}

void ActionPromptModal::create_buttons() {
    lv_obj_t* button_container = find_widget("button_container");
    lv_obj_t* footer_container = find_widget("footer_container");
    lv_obj_t* footer_divider = find_widget("footer_divider");

    if (!button_container) {
        spdlog::warn("[ActionPromptModal] button_container not found");
        return;
    }

    bool has_footer_buttons = false;
    bool has_regular_buttons = false;
    int footer_button_count = 0;

    // The regular buttons are laid out by choose_button_row_layout(). The flow
    // is set for every layout so a reused modal instance never inherits the
    // previous prompt's: a plain row drives the equal-width line, row_wrap
    // (the XML default) the content-sized and grid layouts. Grid cells get
    // their width in fit_rows(), once the card width is final.
    const ButtonRowLayout layout =
        choose_button_row_layout(measure_button_row(button_container, prompt_data_.buttons));
    lv_obj_set_flex_flow(button_container, layout.kind == ButtonRowKind::EqualRow
                                               ? LV_FLEX_FLOW_ROW
                                               : LV_FLEX_FLOW_ROW_WRAP);
    RowFit rows;
    if (layout.kind == ButtonRowKind::Grid) {
        rows.grid_row = button_container;
        rows.grid_cols = layout.cols;
    } else if (layout.kind == ButtonRowKind::Wrap) {
        rows.wrap_row = button_container;
    }

    // Create buttons based on PromptData
    for (const auto& btn : prompt_data_.buttons) {
        if (btn.is_footer) {
            if (footer_container) {
                // Add vertical divider between footer buttons
                if (footer_button_count > 0) {
                    lv_obj_t* divider = lv_obj_create(footer_container);
                    lv_obj_set_size(divider, 1, lv_pct(100));
                    lv_obj_set_style_bg_color(divider, theme_manager_get_color("border"),
                                              LV_PART_MAIN);
                    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
                    lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
                    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
                }
                create_button(btn, footer_container);
                has_footer_buttons = true;
                footer_button_count++;
            }
        } else {
            create_button(btn, button_container, layout.kind);
            has_regular_buttons = true;
        }
    }

    // Show/hide footer based on whether there are footer buttons
    if (footer_container) {
        if (has_footer_buttons) {
            lv_obj_remove_flag(footer_container, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(footer_container, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (footer_divider) {
        if (has_footer_buttons) {
            lv_obj_remove_flag(footer_divider, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(footer_divider, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Hide button container if no regular buttons
    if (!has_regular_buttons) {
        lv_obj_add_flag(button_container, LV_OBJ_FLAG_HIDDEN);
    }

    fit_rows(rows);
}

// DECLARATIVE_OK: measured layout. The grid cells and the scroll area's cap
// depend on the pixels the rest of the prompt leaves, which only a layout pass
// knows.
void ActionPromptModal::fit_rows(const RowFit& rows) {
    lv_obj_t* screen = dialog_ ? lv_obj_get_parent(dialog_) : nullptr;
    lv_obj_t* scroll = find_widget("prompt_scroll");
    if (screen == nullptr || scroll == nullptr) {
        return;
    }
    lv_obj_update_layout(screen);

    if (rows.grid_row != nullptr && rows.grid_cols > 0) {
        const int32_t gap = lv_obj_get_style_pad_column(rows.grid_row, LV_PART_MAIN);
        const int32_t cell =
            (lv_obj_get_content_width(rows.grid_row) - gap * (rows.grid_cols - 1)) / rows.grid_cols;
        const uint32_t n = lv_obj_get_child_count(rows.grid_row);
        for (uint32_t i = 0; i < n; ++i) {
            lv_obj_set_width(lv_obj_get_child(rows.grid_row, static_cast<int32_t>(i)), cell);
        }
    }
    // A content-sized button grows with its label, so one label wider than the row
    // would widen the button past the card. Such a label keeps one line and ends in
    // an ellipsis instead.
    if (rows.wrap_row != nullptr) {
        const int32_t row_w = lv_obj_get_content_width(rows.wrap_row);
        const uint32_t n = lv_obj_get_child_count(rows.wrap_row);
        for (uint32_t i = 0; i < n; ++i) {
            lv_obj_t* button = lv_obj_get_child(rows.wrap_row, static_cast<int32_t>(i));
            lv_obj_t* label = lv_obj_get_child_by_type(button, 0, &lv_label_class);
            const int32_t cap = row_w - lv_obj_get_style_pad_left(button, LV_PART_MAIN) -
                                lv_obj_get_style_pad_right(button, LV_PART_MAIN);
            if (label == nullptr || cap <= 0 || lv_obj_get_width(label) <= cap) {
                continue;
            }
            lv_obj_set_size(
                label, cap,
                lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        }
    }
    lv_obj_update_layout(screen);

    // The card is height=content and clamps at its max_height without
    // shrinking any child, so the room the scroll area may take is that cap
    // less everything else on the card (including any overflow below it).
    const int32_t cap_style = lv_obj_get_style_max_height(dialog_, LV_PART_MAIN);
    const int32_t cap = LV_COORD_IS_PCT(cap_style)
                            ? lv_pct_to_px(cap_style, lv_obj_get_content_height(screen))
                            : cap_style;
    const int32_t content_h =
        lv_obj_get_height(dialog_) + std::max<int32_t>(0, lv_obj_get_scroll_bottom(dialog_));
    const int32_t scroll_h = lv_obj_get_height(scroll);
    const int32_t room = cap - (content_h - scroll_h);

    if (scroll_h > room) {
        // The room can be negative when the header and footer alone fill the
        // card; one button height stays visible so the area can still scroll.
        const int32_t floor_h = theme_manager_get_spacing("button_height");
        lv_obj_set_style_max_height(scroll, std::max(room, floor_h), LV_PART_MAIN);
        spdlog::debug("[ActionPromptModal] Content {}px scrolls in {}px", scroll_h,
                      std::max(room, floor_h));
    }
}

void ActionPromptModal::create_button(const PromptButton& btn, lv_obj_t* container,
                                      ButtonRowKind kind) {
    lv_obj_t* button = lv_button_create(container);

    if (btn.is_footer) {
        // Footer buttons: full-width modal style with dividers
        lv_obj_set_height(button, lv_pct(100));
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
    } else if (kind != ButtonRowKind::Wrap) {
        // Equal-width cells with compact space_sm padding. On an EqualRow,
        // grow=1 with width 0 shares the non-wrapping line (#1043); a Grid cell
        // gets its fixed width from fit_rows() and row_wrap lines them up.
        lv_obj_set_width(button, 0);
        lv_obj_set_height(button, theme_manager_get_spacing("button_height"));
        if (kind == ButtonRowKind::EqualRow) {
            lv_obj_set_flex_grow(button, 1);
        }
        lv_obj_set_style_pad_left(button, theme_manager_get_spacing("space_sm"), LV_PART_MAIN);
        lv_obj_set_style_pad_right(button, theme_manager_get_spacing("space_sm"), LV_PART_MAIN);
        lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    } else if (is_swatch(btn)) {
        // A swatch has no label to size it, so it is a square of the smallest
        // button size and row_wrap lays a color grid out as rows of them. A full
        // button_height square makes Z-Mod's 24-color grid outgrow the card.
        const int32_t side = theme_manager_get_spacing("button_height_sm");
        lv_obj_set_size(button, side, side);
        lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    } else {
        // Content-sized with padding, which row_wrap spreads over as many
        // lines as the labels need.
        lv_obj_set_size(button, LV_SIZE_CONTENT, theme_manager_get_spacing("button_height"));
        lv_obj_set_style_pad_left(button, theme_manager_get_spacing("space_lg"), LV_PART_MAIN);
        lv_obj_set_style_pad_right(button, theme_manager_get_spacing("space_lg"), LV_PART_MAIN);
        lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    }
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    // A hex fill takes priority over the named style.
    const std::optional<uint32_t> fill_rgb = helix::parse_hex_color(btn.hex_color);
    const lv_color_t bg_color = fill_rgb ? lv_color_hex(*fill_rgb) : get_button_color(btn.color);
    if (is_swatch(btn)) {
        // A color tile with no text is a swatch, and wears the swatch edge so a
        // white tile on a light card or a black one on a dark card stays visible.
        lv_obj_set_style_border_width(button, theme_manager_get_spacing("border_width"),
                                      LV_PART_MAIN);
        helix::ui::apply_swatch_color(button, *fill_rgb, "");
    } else {
        lv_obj_set_style_bg_color(button, bg_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    }

    // Create label inside button with contrast-aware text color
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, btn.label.c_str());
    if (kind == ButtonRowKind::Grid) {
        // A label wider than its cell ends in an ellipsis rather than spilling.
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), LV_PART_MAIN);
    lv_obj_set_style_text_color(
        label, theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"), bg_color),
        LV_PART_MAIN);

    // Create callback data with owned copy of gcode string and lifetime token.
    //
    // An empty gcode means DO NOTHING — the button closes the modal and sends
    // no command. This used to fall back to sending the *label*, so a button
    // marked "OK" or "Dismiss" transmitted `OK` to Klipper (#1172). Nothing
    // relies on that fallback: Klipper's own `action_prompt_button` protocol
    // already applies the label-as-gcode convention explicitly in
    // ActionPromptManager::parse_button_spec(), so prompts arriving over the
    // wire are unaffected. Only programmatically built PromptData reaches here
    // with a blank gcode, and there it always meant "no command".
    auto cbd = std::make_unique<ButtonCallbackData>();
    cbd->modal = this;
    cbd->token = lifetime_.token();
    cbd->gcode = btn.gcode;

    // Add click callback with ButtonCallbackData as user_data
    lv_obj_add_event_cb(button, on_button_cb, LV_EVENT_CLICKED, cbd.get());

    // Transfer ownership to the vector (pointer remains stable)
    button_callback_data_.push_back(std::move(cbd));

    created_buttons_.push_back(button);

    spdlog::debug("[ActionPromptModal] Created button: {} (gcode: {}, color: {})", btn.label,
                  btn.gcode.empty() ? "<none>" : btn.gcode,
                  !btn.hex_color.empty() ? btn.hex_color
                  : btn.color.empty()    ? std::string("primary")
                                         : btn.color);
}

lv_color_t ActionPromptModal::get_button_color(const std::string& color_name) {
    // Map Klipper color hints to design tokens
    if (color_name == "primary" || color_name.empty()) {
        return theme_manager_get_color("primary");
    } else if (color_name == "secondary") {
        return theme_manager_get_color("success");
    } else if (color_name == "info") {
        return theme_manager_get_color("info");
    } else if (color_name == "warning") {
        return theme_manager_get_color("warning");
    } else if (color_name == "error") {
        return theme_manager_get_color("danger");
    }

    // Unknown color - default to primary
    spdlog::debug("[ActionPromptModal] Unknown color '{}', using primary", color_name);
    return theme_manager_get_color("primary");
}

void ActionPromptModal::clear_dynamic_content() {
    // Remove click callbacks from each button before freeing their user_data.
    // Without this, LVGL's async teardown can still hold references to the
    // ButtonCallbackData pointers that button_callback_data_.clear() frees,
    // risking use-after-free if any queued event dispatches against a button
    // after on_hide() returns.
    for (lv_obj_t* button : created_buttons_) {
        if (button && lv_obj_is_valid(button)) {
            lv_obj_remove_event_cb(button, on_button_cb);
        }
    }
    created_buttons_.clear();
    created_text_labels_.clear();
    button_callback_data_.clear();
}

// ============================================================================
// Event Handler
// ============================================================================

void ActionPromptModal::handle_button_click(const std::string& gcode) {
    // An empty gcode is a dismiss affordance: close, send nothing. Callers no
    // longer have to smuggle a Klipper comment ("; error-dismiss") through to
    // get a button that does nothing (#1172).
    if (gcode.empty()) {
        spdlog::info("[ActionPromptModal] Dismiss button clicked (no gcode)");
        hide();
        return;
    }

    spdlog::info("[ActionPromptModal] Button clicked, gcode: {}", gcode);

    // Call the gcode callback if set
    if (gcode_callback_) {
        gcode_callback_(gcode);
    }

    // Close the modal
    hide();
}

// ============================================================================
// Static Callbacks
// ============================================================================

void ActionPromptModal::on_button_cb(lv_event_t* e) {
    auto* cbd = static_cast<ButtonCallbackData*>(lv_event_get_user_data(e));
    if (!cbd) {
        spdlog::warn("[ActionPromptModal] Button callback data is null");
        return;
    }

    // Check if the modal is still alive (guards against use-after-free).
    // LifetimeToken is safe to query even after the guard is destroyed (#437).
    if (!cbd->token || cbd->token->expired()) {
        spdlog::debug("[ActionPromptModal] Modal destroyed before button callback fired");
        return;
    }

    SoundManager::instance().play("button_tap");
    cbd->modal->handle_button_click(cbd->gcode);
}

// ============================================================================
// Failure reporting
// ============================================================================

void report_action_prompt_gcode_failure(const std::string& error_message) {
    // Same presentation as every other failed macro in the UI (ui_panel_controls,
    // ui_panel_filament). NOTIFY_ERROR marshals to the main thread itself, which
    // matters here: the RPC error callback fires on the WebSocket thread.
    //
    // Klipper's own wording is the useful part ("Extruder not hot enough"); the
    // generic string only stands in when the transport gave us nothing.
    const std::string detail =
        error_message.empty() ? std::string(lv_tr("Unknown error")) : error_message;
    NOTIFY_ERROR(lv_tr("Macro failed: {}"), detail);
}

} // namespace helix::ui
