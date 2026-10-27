// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_stepper_resonance_cal_modal.h"

#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"

#include "app_globals.h"
#include "display_numbering.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_executor.h"
#include "printer_state.h"
#include "stepper_resonance_calibration.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <memory>
#include <string>

namespace helix::ui {

namespace cal = helix::stepper_resonance_cal;

namespace {

bool machine_busy() {
    return lv_subject_get_int(
               get_printer_state().print_state().get_machine_motion_blocked_subject()) != 0;
}

std::string confirm_message(const PrinterDiscovery& hw) {
    std::string message = lv_tr("The printer moves the toolhead and measures its motors.");
    if (cal::restarts_klipper(hw)) {
        message += " ";
        message += lv_tr("It saves the result and restarts Klipper when it finishes.");
    }
    return message;
}

void on_row_clicked(lv_event_t*) {
    const PrinterDiscovery& hw = get_printer_state().get_discovery();
    if (!cal::supported(hw)) {
        return;
    }
    if (machine_busy()) {
        NOTIFY_WARNING(lv_tr("Cannot calibrate while printing"));
        return;
    }
    const std::string message = confirm_message(hw);
    const size_t tools = helix::ToolState::instance().tools().size();
    if (cal::per_tool(hw) && tools > 1) {
        if (!StepperResonanceCalModal::show_owned(tools, message.c_str())) {
            NOTIFY_ERROR(lv_tr("Failed to show confirmation dialog"));
        }
        return;
    }
    modal_confirm(lv_tr(cal::entry_name()), message.c_str(), ModalSeverity::Warning, lv_tr("Start"),
                  []() { start_stepper_resonance_cal(0); });
}

} // namespace

bool StepperResonanceCalModal::show_owned(size_t tool_count, const char* message) {
    const char* attrs[] = {"message", message, nullptr};
    auto modal = std::make_unique<StepperResonanceCalModal>(tool_count);
    if (!modal->show(lv_screen_active(), attrs)) {
        return false; // the unique_ptr frees the never-shown instance
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return true;
}

void StepperResonanceCalModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    lv_obj_t* dropdown = find_widget("tool_dropdown");
    if (!dropdown) {
        return;
    }
    std::string options;
    for (size_t i = 0; i < tool_count_; ++i) {
        if (i > 0) {
            options += "\n";
        }
        options += tool_label(static_cast<int>(i));
    }
    lv_dropdown_set_options(dropdown, options.c_str());
    lv_dropdown_set_selected(dropdown, 0);
}

void StepperResonanceCalModal::on_ok() {
    lv_obj_t* dropdown = find_widget("tool_dropdown");
    const int tool = dropdown ? static_cast<int>(lv_dropdown_get_selected(dropdown)) : 0;
    hide();
    start_stepper_resonance_cal(tool);
}

void start_stepper_resonance_cal(int tool) {
    // A print can start while the dialog is open (Mainsail, the job queue).
    if (machine_busy()) {
        NOTIFY_WARNING(lv_tr("Cannot calibrate while printing"));
        return;
    }
    const PrinterDiscovery& hw = get_printer_state().get_discovery();
    const std::string gcode = cal::calibrate_gcode(hw, tool);
    auto* api = get_moonraker_api();
    if (gcode.empty() || !api) {
        spdlog::warn("[StepperResonanceCal] Nothing to send (gcode='{}', api={})", gcode,
                     static_cast<const void*>(api));
        return;
    }
    spdlog::info("[StepperResonanceCal] Starting {} ({})", gcode, cal::provider_name(hw));
    NOTIFY_INFO(lv_tr("Calibrating..."));
    // Resolved here: the error callback runs on the WebSocket thread.
    const MacroHostEffect effect =
        cal::restarts_klipper(hw) ? MacroHostEffect::Restarts : MacroHostEffect::None;
    api->execute_gcode(
        gcode, nullptr,
        [effect](const MoonrakerError& err) {
            switch (classify_macro_rpc_failure(effect, err)) {
            case MacroFailureReport::ExpectedRestart:
                // The routine's closing SAVE_CONFIG drops the rpc. Armed now,
                // when the restart happens, not at the send minutes earlier.
                spdlog::info("[StepperResonanceCal] Klipper restarted after the calibration ({})",
                             err.message);
                begin_expected_klippy_restart("Firmware restarting...");
                return;
            case MacroFailureReport::ExpectedHalt:
                return;
            case MacroFailureReport::Error:
                break;
            }
            spdlog::error("[StepperResonanceCal] Calibration failed: {}", err.message);
            helix::ui::notify_error_tr(TR_NOOP("Calibration failed: {}"), err);
        },
        IMoonrakerAPI::kMacroTimeoutMs);
}

void init_stepper_resonance_cal_row_handler() {
    register_xml_callbacks({{"on_stepper_resonance_cal_clicked", on_row_clicked}});
}

} // namespace helix::ui
