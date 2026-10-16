// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_pause_reason.cpp
 * @brief The paused overlay's reason line on a pause something other than
 *        filament explains.
 *
 * Run with: ./build/bin/helix-tests "[print_status][pause_reason]"
 *
 * A Snapmaker U1 bed-check pause (print_stats.exception 532 code 1) carries
 * its reason in the exception, not in print_stats.message. With a runout
 * sensor reading empty at the same time, the reason line must give the
 * firmware's reason, never "Filament Runout".
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "ams_state.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "u1_stock_detection_source.h"

#include <lvgl.h>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::FilamentSensorManager;
using helix::FilamentSensorRole;
using helix::ui::UpdateQueue;

namespace {

constexpr const char* SENSOR = "filament_switch_sensor runout_sensor";

struct PauseReasonFixture : public LVGLUITestFixture {
    PauseReasonFixture() {
        AmsState::instance().clear_backends();
        auto& fsm = FilamentSensorManager::instance();
        PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
        fsm.discover_sensors({SENSOR});
        fsm.set_sensor_role(SENSOR, FilamentSensorRole::RUNOUT);
        fsm.update_from_status(
            nlohmann::json{{SENSOR, {{"filament_detected", false}, {"enabled", true}}}});
        settle();
        REQUIRE(fsm.has_real_runout());

        panel_ = std::make_unique<PrintStatusPanel>(state(), nullptr);
        panel_->init_subjects();
        root_ = panel_->create(test_screen());
    }

    ~PauseReasonFixture() override {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
        PostUnloadGraceTestAccess::reset(FilamentSensorManager::instance());
    }

    void settle() {
        UpdateQueue::instance().drain();
        process_lvgl(10);
    }

    void status(const nlohmann::json& print_stats) {
        state().update_from_status(nlohmann::json{{"print_stats", print_stats}});
        settle();
    }

    static std::string reason() {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "print_pause_reason");
        REQUIRE(subject != nullptr);
        return lv_subject_get_string(subject);
    }

    std::unique_ptr<PrintStatusPanel> panel_;
    lv_obj_t* root_ = nullptr;
};

const nlohmann::json kDirtyBed = {
    {"id", 532}, {"index", 0}, {"code", 1}, {"message", "detected dirty bed"}};

} // namespace

TEST_CASE_METHOD(PauseReasonFixture,
                 "PrintStatusPanel: a dirty-bed pause shows the firmware's reason, not a runout",
                 "[print_status][pause_reason]") {
    helix::detection::U1StockSource source(&state());
    source.set_capable(true);
    source.start();

    status({{"state", "printing"}});
    SECTION("exception in the pause's own frame") {
        status({{"state", "paused"}, {"exception", kDirtyBed}});
        CHECK(reason() == "detected dirty bed");
    }
    SECTION("exception a frame after the pause") {
        status({{"state", "paused"}});
        REQUIRE(reason() == "Filament Runout");
        status({{"exception", kDirtyBed}});
        CHECK(reason() == "detected dirty bed");
    }

    // Leaving the pause ends its explanation: a later plain pause with the
    // sensor still empty reads as a runout.
    status({{"state", "printing"}});
    status({{"state", "paused"}});
    CHECK(reason() == "Filament Runout");
}

TEST_CASE_METHOD(PauseReasonFixture,
                 "PrintStatusPanel: a pause with no explainer still reads the runout sensor",
                 "[print_status][pause_reason]") {
    status({{"state", "printing"}});
    status({{"state", "paused"}, {"exception", kDirtyBed}});
    CHECK(reason() == "Filament Runout");
}
