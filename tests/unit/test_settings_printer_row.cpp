// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_printer_row.cpp
 * @brief The Settings "Printer" row opens the Printer Manager overlay.
 *
 * The Printer Manager is otherwise reachable only from the home printer image
 * widget, which a user can remove; this row keeps it reachable from the touch UI.
 */

#include "ui_nav_manager.h"
#include "ui_panel_settings.h"
#include "ui_printer_manager_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "settings_manager.h"

#include <array>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {
struct PrinterRowFixture : LVGLUITestFixture {
    ScopedRuntimeConfig scoped_config_;
    lv_obj_t* root_ = nullptr;

    PrinterRowFixture() {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        get_runtime_config()->use_real_ethernet = false;
        SettingsManager::instance().init_subjects();
        get_global_settings_panel().init_subjects();
        root_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "settings_panel", nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }
    ~PrinterRowFixture() override {
        if (root_ && lv_obj_is_valid(root_))
            lv_obj_delete(root_);
        UpdateQueue::instance().drain();
        get_global_settings_panel().deinit_subjects();
        UpdateQueue::instance().drain();
    }
};
} // namespace

TEST_CASE_METHOD(PrinterRowFixture, "settings root: tapping Printer opens the Printer Manager",
                 "[settings][settings_root][printer_manager]") {
    DisplaySettingsManager::instance().set_animations_enabled(false);
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());
    get_global_settings_panel().setup(root_, test_screen());

    lv_obj_t* row = lv_obj_find_by_name(root_, "row_printer");
    REQUIRE(row);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    process_lvgl(50);
    REQUIRE(NavigationManager::instance().overlay_stack_names().size() == 1);
    CHECK(NavigationManager::instance().overlay_stack_names()[0] == "printer_manager_overlay");

    NavigationManager::instance().go_back();
    UpdateQueue::instance().drain();
    process_lvgl(100);
    destroy_printer_manager_overlay();
    UpdateQueue::instance().drain();
    process_lvgl(100);
    DisplaySettingsManager::instance().set_animations_enabled(true);
    CHECK(NavigationManager::instance().overlay_stack_names().empty());
}
