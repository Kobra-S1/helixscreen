// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_switcher_print_gate.cpp
 * @brief The home-panel tool switcher answers to the same print gate as every
 *        other filament surface, and never refuses in silence.
 *
 * Two defects are pinned here.
 *
 * 1. handle_tool_selected() showed a "Tool Change During Print" confirmation
 *    while PRINTING and dispatched on confirm — but every AmsSubscriptionBackend
 *    refuses PRINTING unconditionally. The user was warned, consented, tapped
 *    Change Tool, and the pill did not move. The UI must not offer what the
 *    backend will refuse; the gate is helix::ui::print_blocks_filament_op(), the
 *    same predicate the filament panel, the AMS sidebar and the AMS context menu
 *    already use, NOT a tool-change policy of its own.
 *
 * 2. Both request_tool_change() call sites passed no on_error, so a refusal that
 *    DID reach the backend vanished: no toast, no log on that branch. The only
 *    trace was the backend's own warning inside refuse_if_printing().
 *
 * PAUSED stays permitted on every backend that does not self-home — that is the
 * runout / colour-change recovery workflow, and greying it would make the
 * backend relaxation invisible (see test_ams_paused_filament_ops.cpp).
 */

#include "ui_modal.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/tool_switcher_rig.h"
#include "../test_helpers/tool_switcher_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "tool_state.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct ToolSwitcherGateFixture : public LVGLTestFixture {
    ToolSwitcherGateFixture() {
        printer_state.init_subjects(false);
    }

    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(printer_state, s);
    }

    /// A host-side pre-print block: the wire still reads standby.
    void set_preprint_phase(helix::PrintStartPhase phase) {
        printer_state.set_print_start_state(phase, "", 0);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    helix::PrinterState printer_state;
    ToolChangerRig rig{3};
    GateBackend*& backend = rig.backend;
    std::vector<std::string>& warnings = rig.warnings;
    std::vector<std::string>& errors = rig.errors;
};

} // namespace

// ============================================================================
// Defect 2: the refusal has to reach the user
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherGateFixture,
                 "tool switcher surfaces a backend tool-change failure instead of swallowing it",
                 "[ams][tool-switcher][tool-change]") {
    // STANDBY: the print gate lets this through, so the only thing that can go
    // wrong is the backend itself — which is exactly the branch that had no
    // on_error and therefore produced no toast and no log.
    set_print_state(helix::PrintJobState::STANDBY);
    backend->tool_change_should_fail = true;

    ToolSwitcherWidget widget(printer_state);
    ToolSwitcherTestAccess::select_tool(widget, 1);

    CHECK(backend->change_tool_calls == 1);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("Tool change did not complete") != std::string::npos);
}

TEST_CASE_METHOD(ToolSwitcherGateFixture, "a successful tool change raises no error toast",
                 "[ams][tool-switcher][tool-change]") {
    set_print_state(helix::PrintJobState::STANDBY);

    ToolSwitcherWidget widget(printer_state);
    ToolSwitcherTestAccess::select_tool(widget, 1);

    CHECK(backend->change_tool_calls == 1);
    CHECK(errors.empty());
    CHECK(warnings.empty());
}

// ============================================================================
// Defect 1: PRINTING is refused up front, not offered behind a modal
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherGateFixture, "PRINTING refuses the tool change and says why",
                 "[ams][tool-switcher][safety][paused]") {
    set_print_state(helix::PrintJobState::PRINTING);

    ToolSwitcherWidget widget(printer_state);

    const AmsError refusal = ToolSwitcherTestAccess::refusal(widget);
    REQUIRE_FALSE(refusal.success());
    CHECK(refusal.result == AmsResult::WRONG_STATE);
    CHECK(refusal.user_msg == "Cannot run filament operation while printing");
    // This backend permits ops on a paused job, so pausing is the recovery to
    // name — not "finish or cancel", which throws away a savable print.
    CHECK(refusal.suggestion.find("Pause the print") != std::string::npos);

    ToolSwitcherTestAccess::select_tool(widget, 1);

    // Nothing dispatched: the previous code opened a confirmation modal and
    // dispatched on confirm, into a backend that always refuses.
    CHECK(backend->change_tool_calls == 0);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("Cannot run filament operation while printing") != std::string::npos);
}

// ============================================================================
// PAUSED: allowed, except on a backend whose filament macros home themselves
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherGateFixture,
                 "PAUSED still permits the tool change when the backend never homes",
                 "[ams][tool-switcher][safety][paused]") {
    set_print_state(helix::PrintJobState::PAUSED);
    backend->self_homes = false;

    ToolSwitcherWidget widget(printer_state);
    CHECK(ToolSwitcherTestAccess::refusal(widget).success());
}

TEST_CASE_METHOD(ToolSwitcherGateFixture, "PREPARING refuses the tool change on every backend",
                 "[ams][tool-switcher][safety][preparing]") {
    // A host-side pre-start block owns the toolhead and print_stats cannot say
    // so. The backend's self-homing capability is irrelevant here: the app's own
    // block is already moving the toolhead.
    set_print_state(helix::PrintJobState::STANDBY);
    set_preprint_phase(helix::PrintStartPhase::BED_MESH);

    for (bool self_homes : {false, true}) {
        backend->self_homes = self_homes;
        ToolSwitcherWidget widget(printer_state);
        CHECK_FALSE(ToolSwitcherTestAccess::refusal(widget).success());
    }
}

TEST_CASE_METHOD(ToolSwitcherGateFixture, "PAUSED refuses on a self-homing backend (AD5X IFS)",
                 "[ams][tool-switcher][safety][paused]") {
    set_print_state(helix::PrintJobState::PAUSED);
    backend->self_homes = true;

    ToolSwitcherWidget widget(printer_state);

    const AmsError refusal = ToolSwitcherTestAccess::refusal(widget);
    REQUIRE_FALSE(refusal.success());
    CHECK(refusal.user_msg == "Can't move filament while the print is paused");
    // "while printing" on a paused job reads as a bug (bundle JX2FVRB9).
    CHECK(refusal.user_msg.find("printing") == std::string::npos);

    ToolSwitcherTestAccess::select_tool(widget, 1);
    CHECK(backend->change_tool_calls == 0);
    REQUIRE(warnings.size() == 1);
}

// ============================================================================
// The greying has to track the print state LIVE
//
// PanelWidget instances are recycled across home-panel rebuilds, so the gate is
// applied from attach() (via the print-state observer) and from both rebuild
// paths. Registering the observer only in on_size_changed() would leave a reused
// instance correct on first build and stale forever after.
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherGateFixture, "pills grey and un-grey as the print state changes",
                 "[ams][tool-switcher][safety]") {
    lv_obj_t* host = lv_obj_create(lv_screen_active());
    ToolSwitcherWidget widget(printer_state);
    widget.attach(host, lv_screen_active());

    auto& pills = ToolSwitcherTestAccess::pills(widget);
    pills.push_back(lv_obj_create(host));
    pills.push_back(lv_obj_create(host));

    // observe_int_sync() defers through the UpdateQueue, so the gate lands on
    // the next tick — drain before asserting.
    auto tick = [] {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    };

    set_print_state(helix::PrintJobState::PRINTING);
    tick();
    for (lv_obj_t* pill : pills) {
        CHECK(lv_obj_has_state(pill, LV_STATE_DISABLED));
    }

    // PAUSED on a backend that does not self-home is the recovery workflow —
    // greying it there would hide the backend relaxation entirely.
    set_print_state(helix::PrintJobState::PAUSED);
    tick();
    for (lv_obj_t* pill : pills) {
        CHECK_FALSE(lv_obj_has_state(pill, LV_STATE_DISABLED));
    }

    set_print_state(helix::PrintJobState::STANDBY);
    tick();
    for (lv_obj_t* pill : pills) {
        CHECK_FALSE(lv_obj_has_state(pill, LV_STATE_DISABLED));
    }

    widget.detach();
    lv_obj_delete(host);
}

// ============================================================================
// States that were never gated stay ungated
// ============================================================================

TEST_CASE_METHOD(ToolSwitcherGateFixture, "inactive print states never block a tool change",
                 "[ams][tool-switcher][safety][paused]") {
    ToolSwitcherWidget widget(printer_state);

    for (helix::PrintJobState s : {helix::PrintJobState::STANDBY, helix::PrintJobState::COMPLETE,
                                   helix::PrintJobState::CANCELLED, helix::PrintJobState::ERROR}) {
        set_print_state(s);
        CAPTURE(static_cast<int>(s));
        CHECK(ToolSwitcherTestAccess::refusal(widget).success());
    }
}

// ============================================================================
// Confirm tool changes
// ============================================================================

namespace {

/// The confirmation is a modal_dialog, which LVGLUITestFixture registers.
struct ToolSwitcherConfirmFixture : public LVGLUITestFixture {
    ToolSwitcherConfirmFixture() {
        helix::test::set_wire_state(state(), helix::PrintJobState::STANDBY);
    }
    ~ToolSwitcherConfirmFixture() override {
        ModalStack::instance().clear();
    }
    ToolChangerRig rig{3};
};

} // namespace

TEST_CASE_METHOD(ToolSwitcherConfirmFixture, "with Confirm tool changes on, a tool tap asks first",
                 "[ams][tool-switcher][tool-change][confirm]") {
    SafetySettingsManager::instance().set_tool_change_require_confirmation(true);
    ToolSwitcherWidget widget(state());
    ToolSwitcherTestAccess::select_tool(widget, 1);

    CHECK(rig.backend->change_tool_calls == 0);
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);

    SECTION("confirming changes tool") {
        helix::test::answer_top_modal("btn_primary");
        CHECK(rig.backend->change_tool_calls == 1);
        CHECK(rig.backend->last_tool == 1);
    }
    SECTION("cancelling changes nothing") {
        helix::test::answer_top_modal("btn_secondary");
        CHECK(rig.backend->change_tool_calls == 0);
    }
}

TEST_CASE_METHOD(ToolSwitcherConfirmFixture,
                 "a paused print asks once whether Confirm tool changes is on or off",
                 "[ams][tool-switcher][tool-change][confirm][paused]") {
    const bool confirm = GENERATE(false, true);
    CAPTURE(confirm);
    SafetySettingsManager::instance().set_tool_change_require_confirmation(confirm);
    helix::test::set_wire_state(state(), helix::PrintJobState::PAUSED);
    rig.backend->self_homes = false;

    ToolSwitcherWidget widget(state());
    ToolSwitcherTestAccess::select_tool(widget, 1);
    CHECK(rig.backend->change_tool_calls == 0);
    helix::test::answer_top_modal("btn_primary");
    CHECK(rig.backend->change_tool_calls == 1);
    // One dialog, not the paused one stacked over the plain one.
    CHECK(ModalStack::instance().stack_empty());
}
