// src/ui/print_control_buttons.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_control_buttons.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h" // NOTIFY_WARNING / NOTIFY_ERROR
#include "ui_event_safety.h"    // LVGL_SAFE_EVENT_CB_BEGIN / END
#include "ui_resume_dispatch.h"
#include "ui_timer_guard.h" // lv_timer_cancel_safe
#include "ui_update_queue.h"

#include "abort_manager.h"
#include "app_globals.h" // get_printer_state()
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h" // lv_tr
#include "moonraker_error.h"
#include "observer_factory.h"
#include "print_job_ref.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix::ui {

PrintControlButtons& PrintControlButtons::instance() {
    static PrintControlButtons s_instance;
    return s_instance;
}

void PrintControlButtons::init_subjects() {
    if (subjects_initialized_)
        return;

    UI_MANAGED_SUBJECT_STRING(primary_icon_subject_, primary_icon_buf_, "\xF3\xB0\x8F\xA4",
                              "print_control_primary_icon", subjects_);
    UI_MANAGED_SUBJECT_STRING(primary_label_subject_, primary_label_buf_, kControlLabelPause,
                              "print_control_primary_label", subjects_);
    UI_MANAGED_SUBJECT_INT(primary_enabled_subject_, 0, "print_control_primary_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(stop_enabled_subject_, 0, "print_control_stop_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(pending_action_subject_, 0, "print_pending_action", subjects_);

    register_xml_callbacks({
        {"on_print_control_primary", [](lv_event_t*) { instance().handle_primary_button(); }},
        {"on_print_control_stop", [](lv_event_t*) { instance().handle_stop_button(); }},
    });

    // print_state_enum is a static global subject — no SubjectLifetime needed.
    // RAW_PRINT_STATE_OK: TWO observers on purpose. This one clears the
    // optimistic pending action when the PRINTER's own state moves; the
    // lifecycle observer below recomputes the buttons. Collapsing them would
    // clear a pending Pause on the Idle -> Preparing edge, before the printer
    // has answered.
    print_state_observer_ = observe<int>(
        get_printer_state().print_state().get_print_state_enum_subject(), this,
        [](PrintControlButtons* self, int) {
            self->settle_cancel_from_job_state(
                get_printer_state().print_state().get_print_job_state());
            // A real state change clears any optimistic
            // pending action; the new state is authoritative.
            // Otherwise just recompute the buttons.
            if (self->pending_action_ != PendingAction::None)
                self->clear_pending_action();
            else
                self->recompute();
        },
        get_printer_state().get_subjects_lifetime());

    // The lifecycle is the axis the affordance contract is written against, and
    // it moves independently of print_state_enum: during a host-side pre-start
    // block the printer keeps reporting the previous job, so nothing else here
    // would re-evaluate for the entire window.
    print_lifecycle_observer_ = observe<int>(
        get_printer_state().print_state().get_print_lifecycle_subject(), this,
        [](PrintControlButtons* self, int) {
            self->release_cancel_if_job_moved_on();
            self->recompute();
        },
        get_printer_state().get_subjects_lifetime());

    // Self-register cleanup so subjects/observer are torn down before lv_deinit().
    StaticSubjectRegistry::instance().register_deinit("PrintControlButtons", []() {
        auto& self = PrintControlButtons::instance();
        // release() (NOT reset()) is correct here: this runs pre-lv_deinit when
        // the observed subject is already being destroyed by its own owner.
        self.print_state_observer_.release();
        self.print_lifecycle_observer_.release();
        self.teardown_subjects();
    });

    subjects_initialized_ = true;
    recompute();
}

ControlButtonView PrintControlButtons::current_view() const {
    auto& state = get_printer_state();
    auto& macros = StandardMacros::instance();

    ControlButtonInputs in;
    // RAW_PRINT_STATE_OK: ControlButtonInputs carries BOTH axes on purpose -
    // job_state (the wire) and lifecycle - because "who holds the job" and "what
    // may the user do" are different questions. See print_control_view.cpp.
    in.job_state = state.print_state().get_print_job_state();
    in.lifecycle = state.print_state().get_print_lifecycle();
    in.has_preparing_job = state.print_state().has_preparing_job();
    in.pending = pending_action_;
    in.pause_available = !macros.get(StandardMacroSlot::Pause).is_empty();
    in.resume_available = !macros.get(StandardMacroSlot::Resume).is_empty();
    in.cancel_available = !macros.get(StandardMacroSlot::Cancel).is_empty();
    in.start_sent = state.print_state().preparing_start_sent();
    in.cancel_requested = cancel_requested_;
    return compute_control_button_view(in);
}

void PrintControlButtons::recompute() {
    if (!subjects_initialized_)
        return;

    ControlButtonView v = current_view();

    std::snprintf(primary_icon_buf_, sizeof(primary_icon_buf_), "%s", v.primary_icon);
    std::snprintf(primary_label_buf_, sizeof(primary_label_buf_), "%s", v.primary_label);
    lv_subject_copy_string(&primary_icon_subject_, primary_icon_buf_);
    lv_subject_copy_string(&primary_label_subject_, primary_label_buf_);
    lv_subject_set_int(&primary_enabled_subject_, v.primary_enabled ? 1 : 0);
    lv_subject_set_int(&stop_enabled_subject_, v.stop_enabled ? 1 : 0);
    lv_subject_set_int(&pending_action_subject_, static_cast<int>(pending_action_));
}

// PrintControlButtons is an app-lifetime singleton, so the stateless callbacks
// below reach back via PrintControlButtons::instance() rather than capturing
// `this` — the established safe pattern (mirrors PrintStatusPanel's old
// get_global_print_status_panel() usage). No use-after-free is possible.
void PrintControlButtons::handle_primary_button() {
    if (!api_) {
        spdlog::warn("[PrintControl] No API - cannot dispatch primary action");
        return;
    }
    // RAW_PRINT_STATE_OK: picks which macro to send; see below.
    auto state = get_printer_state().print_state().get_print_job_state();
    auto& macros = StandardMacros::instance();

    // RAW_PRINT_STATE_OK: chooses WHICH macro to send. Pause is meaningless
    // before the printer holds the job, and print_control_view already refuses
    // the button during Preparing.
    if (state == helix::PrintJobState::PRINTING) {
        if (macros.get(StandardMacroSlot::Pause).is_empty()) {
            NOTIFY_WARNING(lv_tr("Pause macro not configured"));
            return;
        }
        spdlog::info("[PrintControl] Pausing print");
        start_pending_action(PendingAction::Pausing);
        // suppress_auto_toast=true: the on_error below surfaces a contextual
        // toast; the generic RPC_ERROR auto-toast + Klipper's `!!` broadcast
        // for the same root cause would be redundant noise.
        macros.execute(
            StandardMacroSlot::Pause, api_, []() { spdlog::info("[PrintControl] Pause sent"); },
            [](const MoonrakerError& err) {
                spdlog::error("[PrintControl] Pause failed: {}", err.message);
                helix::ui::notify_error_tr(TR_NOOP("Failed to pause print: {}"), err);
                PrintControlButtons::instance().clear_pending_action();
            },
            /*timeout_ms=*/0, /*suppress_auto_toast=*/true);
        // RAW_PRINT_STATE_OK: Resume is only meaningful once the PRINTER says
        // it is paused.
    } else if (state == helix::PrintJobState::PAUSED) {
        request_resume();
    }
}

void PrintControlButtons::request_resume() {
    if (!api_) {
        spdlog::warn("[PrintControl] No API - cannot resume");
        return;
    }
    auto& macros = StandardMacros::instance();
    if (macros.get(StandardMacroSlot::Resume).is_empty()) {
        NOTIFY_WARNING(lv_tr("Resume macro not configured"));
        return;
    }
    spdlog::info("[PrintControl] Resuming print");
    start_pending_action(PendingAction::Resuming);
    // dispatch_prepared_resume runs the backend prepare_for_resume → Resume
    // chain. The optimistic spinner spans the whole window; only clear on
    // failure (success waits on the PrinterState observer confirmation, which
    // clears the pending action when state transitions to Printing).
    helix::ui::dispatch_prepared_resume(
        api_, "[PrintControl]", []() { PrintControlButtons::instance().clear_pending_action(); });
}

void PrintControlButtons::handle_stop_button() {
    spdlog::info("[PrintControl] Stop clicked - confirming");
    if (helix::AbortManager::instance().is_aborting()) {
        NOTIFY_WARNING(lv_tr("Abort already in progress"));
        return;
    }
    cancel_modal_.set_on_confirm([]() { PrintControlButtons::instance().confirm_stop(); });
    cancel_modal_.show(lv_screen_active());
}

void PrintControlButtons::confirm_stop() {
    auto& state = get_printer_state();
    // The same decision that enabled the button picks the mechanism, so the
    // two can never disagree about which one applies.
    switch (current_view().stop_route) {
    case StopRoute::RetirePreparing:
        // Nothing has reached the printer, and every path that sends a start
        // (PrintPreparationManager's choke points, the reprint in
        // PrintStartController) first checks that its job is still the one
        // being prepared, so withdrawing it here is what stops it. Sending
        // CANCEL_PRINT instead would reach an idle printer, which AbortManager
        // reads as an immediate success.
        spdlog::info("[PrintControl] Stop confirmed while preparing - cancelling the start");
        // The notification and the heater cooldown belong to the
        // preparing-exit observer, which sees every retirement rather than
        // just this one path.
        state.print_state().retire_preparing(helix::PreparingExit::Cancelled);
        return;
    case StopRoute::CancelSentStart:
        cancel_sent_start();
        return;
    case StopRoute::Abort:
        break;
    }

    spdlog::info("[PrintControl] Stop confirmed - starting AbortManager");
    // AbortManager handles its own UI state (progress modal, button states).
    helix::AbortManager::instance().start_abort();
}

void PrintControlButtons::cancel_sent_start() {
    if (!api_) {
        NOTIFY_ERROR(lv_tr("Failed to cancel: {}"), lv_tr("Not connected to printer"));
        return;
    }
    // Klipper cannot interrupt a running macro. The cancel queues behind the
    // print-start sequence and runs before the first line of the file, so the
    // sequence finishes and the print never begins. Until the printer reports
    // how the job ended, it stays preparing: it is not cancelled yet.
    const int epoch =
        lv_subject_get_int(get_printer_state().print_state().get_preparing_epoch_subject());
    spdlog::info("[PrintControl] Stop confirmed after the start was sent - cancelling on the "
                 "printer (preparing epoch {})",
                 epoch);
    cancel_requested_ = true;
    cancel_epoch_ = epoch;
    cancel_rpc_done_ = false;
    recompute();
    NOTIFY_INFO(lv_tr("Cancel sent. The printer finishes its start sequence, then stops "
                      "before printing."));

    api_->job().cancel_print(
        [epoch]() {
            queue_update("PrintControlButtons::cancel_sent_start.ok", [epoch]() {
                auto& self = PrintControlButtons::instance();
                if (!self.cancel_requested_ || self.cancel_epoch_ != epoch) {
                    return; // already settled by the printer's job state
                }
                spdlog::info("[PrintControl] Printer answered the cancel of a sent start");
                self.cancel_rpc_done_ = true;
                self.release_cancel_if_job_moved_on();
            });
        },
        [epoch](const MoonrakerError& err) {
            queue_update("PrintControlButtons::cancel_sent_start.err", [epoch, err]() {
                auto& self = PrintControlButtons::instance();
                if (!self.cancel_requested_ || self.cancel_epoch_ != epoch) {
                    return;
                }
                if (err.is_transport_loss()) {
                    // The cancel may still be queued behind the start sequence:
                    // a print-start macro can outlast any RPC ceiling. Stop stays
                    // off, so a second tap cannot queue another CANCEL_PRINT, and
                    // the printer's job state settles it.
                    spdlog::warn("[PrintControl] Cancel of a sent start unanswered ({}) - "
                                 "waiting on the printer's job state",
                                 err.message);
                    NOTIFY_INFO(lv_tr("The printer has not answered the cancel yet. It "
                                      "stops once its start sequence finishes."));
                    self.cancel_rpc_done_ = true;
                    self.release_cancel_if_job_moved_on();
                    return;
                }
                spdlog::error("[PrintControl] Cancel of a sent start refused: {}", err.message);
                self.clear_cancel_request();
                NOTIFY_ERROR(lv_tr("Failed to cancel: {}"), err.localized_message());
            });
        });
}

void PrintControlButtons::clear_cancel_request() {
    cancel_requested_ = false;
    cancel_epoch_ = 0;
    cancel_rpc_done_ = false;
    recompute();
}

void PrintControlButtons::release_cancel_if_job_moved_on() {
    // The printer took the job (Confirmed) or something else retired it. Its
    // own state now carries the outcome, so Stop belongs to that job.
    const int now =
        lv_subject_get_int(get_printer_state().print_state().get_preparing_epoch_subject());
    if (cancel_requested_ && cancel_rpc_done_ && now != cancel_epoch_) {
        clear_cancel_request();
    }
}

void PrintControlButtons::settle_cancel_from_job_state(helix::PrintJobState job_state) {
    if (!cancel_requested_) {
        return;
    }
    auto& ps = get_printer_state().print_state();
    const bool still_ours = lv_subject_get_int(ps.get_preparing_epoch_subject()) == cancel_epoch_;
    // RAW_PRINT_STATE_OK: how the PRINTER ended the start a cancel is queued
    // behind. STANDBY is not an answer: SDCARD_PRINT_FILE passes through it
    // while loading the file, before the queued cancel can run.
    switch (job_state) {
    case helix::PrintJobState::CANCELLED:
    case helix::PrintJobState::COMPLETE:
    case helix::PrintJobState::ERROR:
        // The printer ended our job itself. Its terminal state reaches the
        // completion observer, which reports it once.
        if (still_ours) {
            ps.retire_preparing(helix::PreparingExit::EndedByPrinter);
        }
        clear_cancel_request();
        break;
    // RAW_PRINT_STATE_OK: the printer holding the job, as it reports it.
    case helix::PrintJobState::PRINTING:
    case helix::PrintJobState::PAUSED:
        // Holding the job while the cancel is still unanswered is the expected
        // path: the cancel runs next and ends it. Holding it after the printer
        // answered means the cancel ran before the start reached the printer,
        // and this print is running.
        if (cancel_rpc_done_) {
            spdlog::warn("[PrintControl] Printer took the job after answering the cancel - "
                         "the print is running");
            NOTIFY_WARNING(lv_tr("The print started after the cancel was answered. Cancel "
                                 "it again to stop it."));
            clear_cancel_request();
        }
        break;
    default:
        break;
    }
}

void PrintControlButtons::start_pending_action(PendingAction action) {
    clear_pending_action(); // supersede any in-flight action (also deletes prior timer)
    pending_action_ = action;
    // The authoritative clear is the print_state_observer_ above: when the real
    // print state transitions (paused→printing for Resume, printing→paused for
    // Pause) it clears the pending action. This timer is only a last-resort
    // backstop for a silently-lost command.
    //
    // Pause normally lands in <2s (can stretch to ~20s while buffered moves
    // drain) → 25s backstop. Resume on an auto-feed backend (Snapmaker U1) runs
    // AUTO_FEEDING heat+feed+flush + reheat, which takes 40-90s before the
    // paused→printing transition fires — a 25s backstop false-fired a spurious
    // "Resume command timed out" toast on a resume that actually succeeded
    // (#991). Use a 150s ceiling for Resume so the observer wins for any normal
    // recovery and the timer only trips on a genuinely lost command.
    const uint32_t timeout_ms = (action == PendingAction::Resuming) ? 150000u : 25000u;
    pending_action_timeout_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<PrintControlButtons*>(lv_timer_get_user_data(t));
            if (!self)
                return;
            const char* verb =
                (self->pending_action_ == PendingAction::Resuming) ? "Resume" : "Pause";
            spdlog::warn("[PrintControl] {} timed out - clearing pending", verb);
            NOTIFY_WARNING(lv_tr("{} command timed out"), verb);
            self->clear_pending_action();
        },
        timeout_ms, this);
    lv_timer_set_repeat_count(pending_action_timeout_, 1);
    recompute();
}

PrintControlButtons::~PrintControlButtons() {
    // The StaticSubjectRegistry teardown below cancels this timer pre-lv_deinit,
    // but only on a shutdown that actually runs the registry. Cancelling here too
    // means no teardown path leaves pending_action_timeout_ armed on a freed
    // `this`. lv_timer_cancel_safe() self-guards on lv_is_initialized(), which is
    // what makes it safe from a static's destructor (#750, #751, #1173).
    cancel_pending_action_timer();
}

void PrintControlButtons::teardown_subjects() {
    cancel_pending_action_timer();
    cancel_requested_ = false;
    cancel_epoch_ = 0;
    cancel_rpc_done_ = false;
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrintControlButtons::cancel_pending_action_timer() {
    if (pending_action_timeout_) {
        helix::ui::lv_timer_cancel_safe(pending_action_timeout_);
        pending_action_timeout_ = nullptr;
    }
}

void PrintControlButtons::clear_pending_action() {
    cancel_pending_action_timer();
    pending_action_ = PendingAction::None;
    recompute();
}

void PrintControlButtons::notify_printer_error(const std::string& detail) {
    if (pending_action_ == PendingAction::None) {
        return;
    }
    const char* verb = (pending_action_ == PendingAction::Resuming) ? "Resume" : "Pause";
    spdlog::warn("[PrintControl] Klipper error while {} pending — releasing the button: {}", verb,
                 detail);
    clear_pending_action();
}

} // namespace helix::ui
