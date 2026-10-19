// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "action_prompt_manager.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @file action_prompt_modal.h
 * @brief Modal dialog for displaying Klipper action:prompt messages
 *
 * Displays interactive prompts from Klipper macros with dynamic buttons.
 * Buttons can be styled with different colors and grouped for layout.
 *
 * ## Integration Note
 * The component must be registered in main.cpp before use:
 * @code
 * lv_xml_register_component_from_file("action_prompt_modal",
 *     "ui_xml/action_prompt_modal.xml");
 * @endcode
 *
 * ## Usage:
 * @code
 * helix::ui::ActionPromptModal modal;
 * modal.set_gcode_callback([&api](const std::string& gcode) {
 *     api.send_gcode(gcode);
 * });
 * modal.show_prompt(parent, prompt_data);
 * @endcode
 */

// Forward declaration so the modal can grant white-box layout-test access.
struct ActionPromptModalTestAccess;

namespace helix::ui {

/// How the row of regular (non-footer) prompt buttons is laid out.
enum class ButtonRowKind {
    Wrap,     ///< content-sized buttons, row_wrap takes the lines they need
    EqualRow, ///< one non-wrapping line of equal-width cells
    Grid,     ///< equal-width cells, `cols` to a line
};

struct ButtonRowLayout {
    ButtonRowKind kind = ButtonRowKind::Wrap;
    int32_t cols = 0; ///< cells per line; 0 for Wrap
};

/// Measured inputs for choose_button_row_layout(), in pixels.
struct ButtonRowMeasure {
    int32_t count = 0;
    bool all_swatches = false; ///< every button is a label-less color tile
    int32_t widest_label = 0;
    int32_t available = 0; ///< the row's content width
    int32_t gap = 0;       ///< column gap between cells
    int32_t cell_pad = 0;  ///< horizontal padding on each side of a cell's label
    int32_t min_cell = 0;  ///< narrowest equal-width cell that is still a touch target
};

/// Rows of at least this many labelled buttons become a grid.
inline constexpr int32_t kPromptGridMinButtons = 5;

/**
 * @brief Pick the layout for the row of prompt buttons.
 *
 * Four buttons whose labels fit an even share sit on one equal-width line
 * (AFC's "Lane 1".."Lane 4"). Five or more labelled buttons become a grid of
 * three columns, or two when the widest label does not fit a third: a label
 * cut to an ellipsis can lose the suffix that tells two units apart
 * ("HTLF_claymore_1" / "_2"), so the grid gives up a column before it
 * truncates. Swatch rows never become a grid, and share one line only while
 * each tile stays at least `min_cell` wide. Everything else wraps, which never
 * clips. An unmeasured row (available <= 0) wraps.
 */
ButtonRowLayout choose_button_row_layout(const ButtonRowMeasure& m);

/**
 * @brief Modal dialog for Klipper action:prompt messages
 *
 * Dynamically creates buttons based on PromptData from the ActionPromptManager.
 * Supports button colors, grouping, and footer buttons.
 */
class ActionPromptModal : public Modal {
  public:
    /**
     * @brief Callback type for button clicks that send gcode
     */
    using GcodeCallback = std::function<void(const std::string& gcode)>;

    ActionPromptModal();
    ~ActionPromptModal() override;

    // Non-copyable, non-movable (Modal's move operations are deleted; nothing
    // ever moved an ActionPromptModal by value - its owner holds it by
    // unique_ptr)
    ActionPromptModal(const ActionPromptModal&) = delete;
    ActionPromptModal& operator=(const ActionPromptModal&) = delete;
    ActionPromptModal(ActionPromptModal&&) = delete;
    ActionPromptModal& operator=(ActionPromptModal&&) = delete;

    /**
     * @brief Show modal with prompt data
     * @param parent Parent screen for the modal
     * @param data Prompt data from ActionPromptManager
     * @return true if modal was created successfully
     */
    bool show_prompt(lv_obj_t* parent, const PromptData& data);

    /**
     * @brief Set callback for when a button is clicked
     *
     * The callback receives the gcode string associated with the button.
     * After calling the callback, the modal closes automatically.
     *
     * @param callback Function to send gcode to printer
     */
    void set_gcode_callback(GcodeCallback callback);

    // Modal interface
    [[nodiscard]] const char* get_name() const override {
        return "Action Prompt Modal";
    }
    [[nodiscard]] const char* component_name() const override {
        return "action_prompt_modal";
    }

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    /// Points the AFC diagram at this prompt's own text, so a prompt that names no
    /// fault never shows one an earlier modal published, and remembers the value it
    /// replaced.
    void publish_fault_path();
    /// Hands the diagram back to the modal underneath, unless a later modal
    /// published over this prompt's value.
    void restore_fault_path();
    int fault_path_before_ = 0;    ///< afc_fault_segment when this prompt published
    int fault_path_published_ = 0; ///< the value this prompt published
    bool fault_path_held_ = false; ///< published and not yet handed back

    // Grants white-box access to button widgets / containers for layout tests.
    friend struct ::ActionPromptModalTestAccess;

    /**
     * @brief Data passed as user_data to button event callbacks
     *
     * Owns a copy of the gcode string (not a pointer into prompt_data_.buttons)
     * and holds a LifetimeToken to detect if the modal has been destroyed
     * before the queued click event fires (crash #437).
     */
    struct ButtonCallbackData {
        ActionPromptModal* modal;
        std::optional<helix::LifetimeToken> token;
        std::string gcode; // Owned copy, safe from vector reallocation
    };

    // === State ===
    PromptData prompt_data_;
    GcodeCallback gcode_callback_;

    // === Dynamic button tracking ===
    std::vector<lv_obj_t*> created_buttons_;
    std::vector<lv_obj_t*> created_text_labels_;
    std::vector<std::unique_ptr<ButtonCallbackData>> button_callback_data_;

    // === Internal Methods ===
    void populate_content();
    void create_text_lines();
    void create_buttons();
    /// The rows fit_rows() sizes once every button exists.
    struct RowFit {
        lv_obj_t* grid_row = nullptr; ///< the button row, when it is a Grid
        int32_t grid_cols = 0;
        lv_obj_t* wrap_row = nullptr; ///< the button row, when it is content-sized
    };

    /**
     * @brief Size the grid cells and cap the scroll area
     *
     * Grid cells split their row, a wrap-row label wider than its row ends in an
     * ellipsis, and prompt_scroll is capped at the height the
     * card's cap leaves under the header and footer, so text and buttons scroll
     * while the footer stays on the card.
     */
    void fit_rows(const RowFit& rows);

    /**
     * @brief Create a single button inside @p container, laid out for @p kind
     *
     * Wrap buttons are content-sized; EqualRow and Grid buttons are equal-width
     * cells (a Grid cell's width is set later by fit_rows()). Footer buttons
     * ignore @p kind.
     */
    void create_button(const PromptButton& btn, lv_obj_t* container,
                       ButtonRowKind kind = ButtonRowKind::Wrap);
    lv_color_t get_button_color(const std::string& color_name);
    void clear_dynamic_content();

    // === Event Handler ===
    void handle_button_click(const std::string& gcode);

    // === Static Callbacks ===
    static void on_button_cb(lv_event_t* e);
};

/**
 * @brief Show the user why a prompt button's gcode did not run.
 *
 * The modal closes on every button press by design and waits for the firmware
 * to push a replacement prompt, so a macro that aborts leaves the screen empty.
 * This toast fills that gap. It is also what makes the send's
 * `caller_surfaces_errors=true` claim honest: a caller that declares it reports
 * the failure records the message in rpc_error_correlation, which suppresses
 * the independent `!!` GcodeError toast for the same rejection — so dropping
 * this toast would leave the failure with no surface at all. See
 * include/rpc_error_policy.h.
 *
 * Safe to call from the WebSocket background thread - the notification layer
 * marshals to the main thread itself.
 *
 * @param error_message Klipper's message from the failed RPC (MoonrakerError::
 *                      user_message()). Empty falls back to a generic string so
 *                      the toast is never blank.
 */
void report_action_prompt_gcode_failure(const std::string& error_message);

} // namespace helix::ui
