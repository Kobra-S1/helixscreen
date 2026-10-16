// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_op_heater.h"

#include "ui_error_reporting.h"
#include "ui_temperature_utils.h"
#include "ui_timer_guard.h"
#include "ui_update_queue.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "post_op_cooldown_manager.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "temperature_controller.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace helix::ui {

const char* filament_heat_op_name(FilamentHeatOp op) {
    switch (op) {
    case FilamentHeatOp::Load:
        return "load";
    case FilamentHeatOp::Unload:
        return "unload";
    case FilamentHeatOp::Purge:
        return "purge";
    }
    return "filament op";
}

PreheatDecision decide_preheat(const PreheatInputs& in) {
    const int floor = in.floor_c;

    if (in.honor_live_target && in.target_c > 0 &&
        preheat_reached(in.current_c, in.target_c, floor)) {
        return {};
    }
    const int required = filament_op_nozzle_temp(in.material_c.value_or(floor), in.held_c);
    if (preheat_reached(in.current_c, required, floor)) {
        return {};
    }

    int send = in.material_c.value_or(AppConstants::Ams::DEFAULT_LOAD_PREHEAT_TEMP);
    send = std::max(send, floor);
    int wait = filament_op_nozzle_temp(send, in.held_c);
    if (in.ceiling_c > 0) {
        send = std::min(send, in.ceiling_c);
        wait = std::min(wait, in.ceiling_c);
    }
    return {true, send, wait};
}

bool preheat_reached(int current_c, int wait_c, int floor_c) {
    return current_c >= std::max(wait_c - PREHEAT_MARGIN_C, floor_c);
}

const char* preheat_skip_name(PreheatSkip reason) {
    switch (reason) {
    case PreheatSkip::None:
        return "none";
    case PreheatSkip::UserOverride:
        return "allow-cold-load/unload setting";
    case PreheatSkip::BackendSelfHeats:
        return "AMS backend heats for this op";
    }
    return "none";
}

PreheatSkip preheat_skip_reason(FilamentHeatOp op, const FilamentOpPlan* plan,
                                AmsBackend* backend) {
    // The user's claim outranks every detection: their macros heat, or they want
    // a deliberate cold pull (#978). It holds on every tier.
    if (SafetySettingsManager::instance().get_allow_cold_extrude()) {
        return PreheatSkip::UserOverride;
    }
    if (!plan || !backend || plan->tier != FilamentTier::AmsBackend) {
        return PreheatSkip::None;
    }
    if (op == FilamentHeatOp::Load &&
        (plan->ams_call == AmsCall::Load || plan->ams_call == AmsCall::ChangeTool)) {
        // A tool changer's SELECT_TOOL owns its own heat sequence, as the AMS
        // sidebar's load fast path relies on.
        if (backend->supports_auto_heat_on_load() || backend->get_type() == AmsType::TOOL_CHANGER) {
            return PreheatSkip::BackendSelfHeats;
        }
    }
    if (op == FilamentHeatOp::Unload && plan->ams_call == AmsCall::Unload &&
        backend->unload_heats_hotend(plan->ams_arg)) {
        return PreheatSkip::BackendSelfHeats;
    }
    return PreheatSkip::None;
}

FilamentOpHeater* get_filament_op_heater() {
    return PanelWidgetManager::instance().shared_resource<FilamentOpHeater>();
}

void run_with_preheat(PreheatRequest req) {
    FilamentOpHeater* heater = get_filament_op_heater();
    if (!heater) {
        spdlog::warn("{} No FilamentOpHeater — running {} with no preheat", req.log_tag,
                     filament_heat_op_name(req.op));
        if (req.run) {
            req.run(0);
        }
        return;
    }
    heater->run_after_preheat(std::move(req));
}

void report_filament_op_done(FilamentOpTicket ticket, bool succeeded) {
    if (ticket == 0) {
        return;
    }
    helix::ui::queue_update("FilamentOpHeater::op_done", [ticket, succeeded]() {
        if (auto* heater = get_filament_op_heater()) {
            if (succeeded) {
                heater->op_succeeded(ticket);
            } else {
                heater->op_failed(ticket);
            }
        }
    });
}

std::optional<PreheatTarget> preheat_material_for_slot(int slot) {
    AmsBackend* backend = AmsState::instance().get_backend();
    SlotInfo info;
    const SlotInfo* info_ptr = nullptr;
    if (backend && slot >= 0) {
        info = backend->get_slot_info(slot);
        info_ptr = &info;
    }
    const auto ext = AmsState::instance().get_external_spool_info();
    return resolve_load_preheat_material(slot, info_ptr, ext ? &*ext : nullptr);
}

namespace {

bool is_terminal(int action) {
    return action == static_cast<int>(AmsAction::IDLE) ||
           action == static_cast<int>(AmsAction::ERROR);
}

constexpr uint32_t DEADLINE_CHECK_MS = 1000;

} // namespace

FilamentOpHeater::FilamentOpHeater(PrinterState& state, Clock clock)
    : state_(state), clock_(std::move(clock)) {}

FilamentOpHeater::~FilamentOpHeater() {
    // Teardown sends nothing: the printer this heater talked to may already be
    // gone, and the controller with it.
    cancel_deadline_timer();
    pending_.reset();
    temp_observer_.reset();
    target_observer_.reset();
    lifecycle_observer_.reset();
    action_observer_.reset();
}

FilamentOpHeater::TimePoint FilamentOpHeater::now() const {
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

int FilamentOpHeater::read_c(lv_subject_t* subject) const {
    return subject ? temperature::deci_to_degrees(lv_subject_get_int(subject)) : 0;
}

bool FilamentOpHeater::heated_for(FilamentOpTicket ticket) const {
    return ticket != 0 && std::any_of(heated_.begin(), heated_.end(),
                                      [ticket](const Heated& h) { return h.ticket == ticket; });
}

void FilamentOpHeater::forget(FilamentOpTicket ticket) {
    heated_.erase(std::remove_if(heated_.begin(), heated_.end(),
                                 [ticket](const Heated& h) { return h.ticket == ticket; }),
                  heated_.end());
    watches_.erase(std::remove_if(watches_.begin(), watches_.end(),
                                  [ticket](const Watch& w) { return w.ticket == ticket; }),
                   watches_.end());
    if (watches_.empty()) {
        action_observer_.reset();
    }
}

void FilamentOpHeater::restore_prior(int prior_target_c) {
    if (auto* c = get_temperature_controller()) {
        c->set_target(HeaterType::Nozzle, static_cast<double>(prior_target_c), {.toast = false});
    }
}

FilamentOpHeater::Outcome FilamentOpHeater::run_after_preheat(PreheatRequest req) {
    // The target the nozzle had before any heat of ours, when we hold some: a
    // replaced preheat's, or one a scheduled cooldown still owes.
    std::optional<int> inherited;
    if (pending_) {
        const PreheatRequest& waiting = pending_->req;
        if (waiting.op == req.op && waiting.slot == req.slot && waiting.owner == req.owner) {
            spdlog::info("{} {} requested again while its preheat waits — cancelling", req.log_tag,
                         filament_heat_op_name(req.op));
            cancel();
            return Outcome::Cancelled;
        }
        spdlog::info("{} {} replaces the pending {} preheat", req.log_tag,
                     filament_heat_op_name(req.op), filament_heat_op_name(waiting.op));
        inherited = pending_->prior_target_c;
        end_pending(/*run=*/false);
    }
    // A cooldown still pending would turn the nozzle off under this op. The op
    // takes it over instead, so the heat is still cooled once the op is done.
    auto& cooldown = PostOpCooldownManager::instance();
    if (cooldown.has_pending_timer()) {
        cooldown.cancel();
        inherited = 0;
    }

    const FilamentOpTicket ticket = ++next_ticket_;
    // An op that runs on heat already ours, with the nozzle off before it, is the
    // op that heat is cooled down after.
    auto adopt_heat = [this, &inherited](FilamentOpTicket t) {
        if (inherited && *inherited == 0) {
            heated_.push_back(Heated{t, now()});
        }
    };

    if (req.skip != PreheatSkip::None) {
        spdlog::info("{} Skipping preheat for {} — {}", req.log_tag, filament_heat_op_name(req.op),
                     preheat_skip_name(req.skip));
        adopt_heat(ticket);
        req.run(ticket);
        return Outcome::Ran;
    }

    SafetyLimits limits;
    if (auto* api = get_moonraker_api()) {
        limits = api->get_safety_limits();
    }
    const auto& temps = state_.temperature_state();
    const std::string& extruder = temps.active_extruder_name();
    const std::optional<PreheatTarget> material = preheat_material_for_slot(req.slot);
    const bool job_holds = job_holds_machine(state_.get_print_lifecycle());

    PreheatInputs in;
    in.current_c = read_c(state_.get_active_extruder_temp_subject());
    in.target_c = read_c(state_.get_active_extruder_target_subject());
    in.material_c = material ? std::optional<int>(material->temp_c) : std::nullopt;
    in.held_c = static_cast<int>(std::lround(state_.get_active_extruder_last_nonzero_target()));
    in.floor_c = temperature::extrusion_floor_c(limits, extruder);
    in.ceiling_c = temperature::nozzle_max_temp_c(limits, extruder);
    in.honor_live_target = job_holds || req.op == FilamentHeatOp::Purge;
    const PreheatDecision decision = decide_preheat(in);

    if (!decision.heat) {
        spdlog::info("{} Nozzle hot enough for {} ({}°C, target {}°C) — no preheat", req.log_tag,
                     filament_heat_op_name(req.op), in.current_c, in.target_c);
        adopt_heat(ticket);
        req.run(ticket);
        return Outcome::Ran;
    }

    Pending p;
    p.ticket = ticket;
    p.wait_c = decision.wait_c;
    p.floor_c = in.floor_c;
    // Heat taken over from an earlier request or op keeps its "what was it":
    // the target read just now is heat this heater sent.
    p.prior_target_c = inherited ? *inherited : in.target_c;
    p.job_free_at_request = !job_holds;
    p.started = now();
    p.req = std::move(req);
    start_heating(std::move(p), decision.send_c,
                  material ? material->material_name : std::string());
    return Outcome::Heating;
}

void FilamentOpHeater::start_heating(Pending p, int send_c, const std::string& material_name) {
    spdlog::info("{} Preheating to {}°C ({}) for {} of slot {}: waiting for {}°C (prior target "
                 "{}°C)",
                 p.req.log_tag, send_c, material_name.empty() ? "default" : material_name,
                 filament_heat_op_name(p.req.op), p.req.slot, p.wait_c, p.prior_target_c);
    const int wait_c = p.wait_c;
    // A ticket whose op never reported back is dropped once it is older than any
    // op could run.
    const TimePoint stale = now() - 2 * BACKEND_WATCH_TIMEOUT;
    heated_.erase(std::remove_if(heated_.begin(), heated_.end(),
                                 [stale](const Heated& h) { return h.at < stale; }),
                  heated_.end());
    heated_.push_back(Heated{p.ticket, now()});
    pending_ = std::move(p);

    if (auto* c = get_temperature_controller()) {
        c->set_target(HeaterType::Nozzle, static_cast<double>(send_c),
                      {.toast = false, .keep_previous_hot = true});
    }
    if (material_name.empty()) {
        NOTIFY_INFO(lv_tr("Heating to {}°C..."), wait_c);
    } else {
        NOTIFY_INFO(lv_tr("Heating to {}°C for {}..."), wait_c, material_name);
    }

    // The active-extruder subjects and the lifecycle subject are PrinterState's
    // static ones, which die only with the whole PrinterState.
    const SubjectLifetime life = state_.get_subjects_lifetime();
    temp_observer_ = observe_int_sync<FilamentOpHeater>(
        state_.get_active_extruder_temp_subject(), this,
        [](FilamentOpHeater* self, int /*deci*/) { self->on_temperature(); }, life);
    target_observer_ = observe_int_sync<FilamentOpHeater>(
        state_.get_active_extruder_target_subject(), this,
        [](FilamentOpHeater* self, int deci) { self->on_target(deci); }, life);
    lifecycle_observer_ = observe_int_sync<FilamentOpHeater>(
        state_.get_print_lifecycle_subject(), this,
        [](FilamentOpHeater* self, int /*lifecycle*/) { self->on_lifecycle(); }, life);
    cancel_deadline_timer();
    deadline_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<FilamentOpHeater*>(lv_timer_get_user_data(t))->check_deadlines();
        },
        DEADLINE_CHECK_MS, this);
}

void FilamentOpHeater::cancel_deadline_timer() {
    lv_timer_cancel_safe(deadline_timer_);
    deadline_timer_ = nullptr;
}

void FilamentOpHeater::end_pending(bool run) {
    if (!pending_) {
        return;
    }
    Pending ended = std::move(*pending_);
    pending_.reset();
    temp_observer_.reset();
    target_observer_.reset();
    lifecycle_observer_.reset();
    cancel_deadline_timer();
    if (!run) {
        forget(ended.ticket);
        return;
    }
    if (ended.req.run) {
        ended.req.run(ended.ticket);
    }
}

void FilamentOpHeater::abandon(const char* why, const char* toast, bool restore) {
    if (!pending_) {
        return;
    }
    spdlog::info("{} {} — not running {}", pending_->req.log_tag, why,
                 filament_heat_op_name(pending_->req.op));
    NOTIFY_WARNING("{}", toast);
    if (restore) {
        restore_prior(pending_->prior_target_c);
    }
    end_pending(/*run=*/false);
}

void FilamentOpHeater::on_temperature() {
    if (!pending_) {
        return;
    }
    const int current = read_c(state_.get_active_extruder_temp_subject());
    if (!preheat_reached(current, pending_->wait_c, pending_->floor_c)) {
        return;
    }
    // A job that took the machine while we heated owns the nozzle now.
    if (pending_->job_free_at_request && job_holds_machine(state_.get_print_lifecycle())) {
        on_lifecycle();
        return;
    }
    spdlog::info("{} Preheat reached {}°C, running {}", pending_->req.log_tag, current,
                 filament_heat_op_name(pending_->req.op));
    end_pending(/*run=*/true);
}

void FilamentOpHeater::on_target(int target_deci) {
    if (!pending_) {
        return;
    }
    // The target reaches ours some time after the send, so only a drop from a
    // target already seen means the preheat was undone (cleared by the user, a
    // heater fault, a Klipper shutdown). Such a preheat would never complete.
    const int target = temperature::deci_to_degrees(target_deci);
    if (target >= pending_->wait_c - PREHEAT_MARGIN_C) {
        pending_->target_seen = true;
        return;
    }
    if (!pending_->target_seen) {
        return;
    }
    // Whoever took the target away decided where the heater stands.
    abandon("Nozzle target dropped during the preheat", lv_tr("Preheat cancelled"),
            /*restore=*/false);
}

void FilamentOpHeater::on_lifecycle() {
    if (!pending_ || !pending_->job_free_at_request) {
        return;
    }
    if (job_holds_machine(state_.get_print_lifecycle())) {
        // The print owns the heater now.
        abandon("A print took the machine during the preheat", lv_tr("Preheat cancelled"),
                /*restore=*/false);
    }
}

void FilamentOpHeater::check_deadlines() {
    if (!pending_) {
        return;
    }
    const TimePoint t = now();
    if (!pending_->target_seen && t - pending_->started > TARGET_LAND_TIMEOUT) {
        abandon("The nozzle target never arrived", lv_tr("Filament operation timed out"),
                /*restore=*/true);
        return;
    }
    if (t - pending_->started > PREHEAT_TIMEOUT) {
        abandon("The nozzle never reached temperature", lv_tr("Filament operation timed out"),
                /*restore=*/true);
    }
}

bool FilamentOpHeater::cancel() {
    if (!pending_) {
        return false;
    }
    spdlog::info("{} Preheat for {} cancelled", pending_->req.log_tag,
                 filament_heat_op_name(pending_->req.op));
    restore_prior(pending_->prior_target_c);
    NOTIFY_INFO(lv_tr("Preheat cancelled"));
    end_pending(/*run=*/false);
    return true;
}

void FilamentOpHeater::op_succeeded(FilamentOpTicket ticket) {
    if (!heated_for(ticket)) {
        return;
    }
    forget(ticket);
    // A preheat waiting now wants the nozzle hot: the cooldown owed here passes
    // to it, so its cancel or timeout turns the heater off and its own op is
    // cooled down after.
    if (pending_) {
        spdlog::debug("[FilamentOpHeater] Op finished while a preheat waits — the cooldown "
                      "passes to it");
        pending_->prior_target_c = 0;
        return;
    }
    // A job that is starting is about to heat the nozzle itself, and a paused
    // one is waiting to resume at its own temperature.
    if (job_holds_machine(state_.get_print_lifecycle())) {
        spdlog::debug("[FilamentOpHeater] Op finished during a job — no cooldown");
        return;
    }
    spdlog::info("[FilamentOpHeater] Op finished — scheduling the post-op cooldown");
    PostOpCooldownManager::instance().schedule();
}

void FilamentOpHeater::op_failed(FilamentOpTicket ticket) {
    forget(ticket);
}

void FilamentOpHeater::watch_backend_completion(FilamentOpTicket ticket) {
    if (!heated_for(ticket)) {
        return;
    }
    // The action as it stands before the dispatch: a busy left over from an
    // earlier op is not this one starting.
    lv_subject_t* action = AmsState::instance().get_ams_action_subject();
    const int current = action ? lv_subject_get_int(action) : -1;
    watches_.push_back(Watch{ticket, current, false, now() + BACKEND_WATCH_TIMEOUT});
    if (!action_observer_) {
        action_observer_ = observe_int_sync<FilamentOpHeater>(
            action, this, [](FilamentOpHeater* self, int value) { self->on_ams_action(value); },
            AmsState::instance().get_subjects_lifetime());
    }
}

void FilamentOpHeater::on_ams_action(int action) {
    const TimePoint t = now();
    std::vector<FilamentOpTicket> expired;
    std::vector<FilamentOpTicket> done;
    for (auto& w : watches_) {
        if (t > w.deadline) {
            expired.push_back(w.ticket);
            continue;
        }
        if (action == w.last_action) {
            continue; // no edge since this watch last looked
        }
        w.last_action = action;
        if (!is_terminal(action)) {
            w.seen_busy = true;
        } else if (w.seen_busy) {
            // A backend reports IDLE until its firmware starts, so only a
            // terminal edge after a busy one is the end of the op.
            done.push_back(w.ticket);
        }
    }
    for (FilamentOpTicket ticket : expired) {
        forget(ticket);
    }
    for (FilamentOpTicket ticket : done) {
        if (action == static_cast<int>(AmsAction::IDLE)) {
            op_succeeded(ticket);
        } else {
            op_failed(ticket);
        }
    }
}

} // namespace helix::ui
