// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "printer_discovery.h"
#include "safety_settings_manager.h"
#include "tool_state.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix {

/// Backend double: the tool map is what makes ToolState delegate to the backend
/// instead of the Tn gcode fallback, and change_tool() answers straight from a
/// flag so the refusal has somewhere to travel FROM. self_homes is settable
/// because it is the input that decides whether PAUSED is allowed.
///
/// change_tool() deliberately does NOT chain to AmsBackendMock: the mock runs
/// the change on its own thread and emits slot events, whose deferred AmsState
/// writes can outlive this fixture's teardown and land in a LATER test's
/// async-lifetime skip window (test_filament_dispatch_surfaces.cpp counts those
/// exactly). Nothing here asserts simulated motion, only the dispatch result.
class GateBackend : public AmsBackendMock {
  public:
    explicit GateBackend(int slots) : AmsBackendMock(slots) {}

    bool self_homes = false;
    bool tool_change_should_fail = false;
    int change_tool_calls = 0;
    int last_tool = -1;

    /// The action get_system_info() reports in place of the mock's own; IDLE
    /// leaves the mock's.
    AmsAction report_action = AmsAction::IDLE;

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        if (report_action != AmsAction::IDLE) {
            info.action = report_action;
        }
        return info;
    }

    bool filament_ops_self_home() const override {
        return self_homes;
    }

    AmsError change_tool(int tool_number) override {
        ++change_tool_calls;
        last_tool = tool_number;
        if (tool_change_should_fail) {
            return AmsError(AmsResult::TOOL_CHANGE_FAILED, "forced failure",
                            "Tool change did not complete", "Check the filament path and retry");
        }
        return AmsErrorHelper::success();
    }
};

/// A klipper-toolchanger printer with @p heads heads, each its own extruder,
/// driven through a GateBackend, with the warning and error toasts recorded.
/// Head 0 is mounted. "Confirm tool changes" is off, so a tap dispatches at
/// once; a test of the confirmation turns it on. Tears it all down on the way
/// out, so later test files inherit no tools, backend or setting.
struct ToolChangerRig {
    GateBackend* backend = nullptr;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool saved_confirm = SafetySettingsManager::instance().get_tool_change_require_confirmation();

    explicit ToolChangerRig(int heads) {
        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(true);
        PrinterDiscovery hw;
        nlohmann::json objects = nlohmann::json::array({"toolchanger", "heater_bed", "gcode_move"});
        for (int i = 0; i < heads; ++i) {
            objects.push_back("tool T" + std::to_string(i));
            objects.push_back(i == 0 ? std::string("extruder") : "extruder" + std::to_string(i));
        }
        hw.parse_objects(objects);
        ToolState::instance().init_tools(hw);
        REQUIRE(ToolState::instance().tool_count() == heads);

        auto owned = std::make_unique<GateBackend>(heads);
        owned->set_tool_changer_mode(true);
        owned->set_operation_delay(0);
        REQUIRE(owned->start());
        backend = owned.get();
        AmsState::instance().deinit_subjects();
        AmsState::instance().init_subjects(true);
        AmsState::instance().set_backend(std::move(owned));

        ui::set_test_notification_warning_hook(
            [this](const std::string& msg) { warnings.push_back(msg); });
        ui::set_test_notification_error_hook(
            [this](const std::string& msg) { errors.push_back(msg); });
        SafetySettingsManager::instance().set_tool_change_require_confirmation(false);
    }

    ~ToolChangerRig() {
        SafetySettingsManager::instance().set_tool_change_require_confirmation(saved_confirm);
        ui::set_test_notification_warning_hook(nullptr);
        ui::set_test_notification_error_hook(nullptr);
        if (backend) {
            backend->wait_for_operation_thread();
            backend->stop();
        }
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
        ui::UpdateQueueTestAccess::drain_all(ui::UpdateQueue::instance());
    }

    ToolChangerRig(const ToolChangerRig&) = delete;
    ToolChangerRig& operator=(const ToolChangerRig&) = delete;
};

} // namespace helix
