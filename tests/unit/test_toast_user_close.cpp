// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toast_user_close.cpp
 * @brief show_with_action()'s user-closed callback: runs on the X button's
 * removal path only, never on hide(), timeout or stack eviction.
 */

#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "display_settings_manager.h"

#include <cstdint>
#include <iterator>

#include "../catch_amalgamated.hpp"

namespace {

int g_closed_count = 0;
void* g_closed_data = nullptr;

void counting_closed_cb(void* user_data) {
    ++g_closed_count;
    g_closed_data = user_data;
}

void noop_action_cb(void*) {}

void* data_42() {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(42));
}

void drain() {
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

} // namespace

/// Friend accessor (L065), at global scope where the production class's
/// friend declaration names it: stage one list entry the widget paths resolve.
class ToastManagerTestAccess {
  public:
    static void inject(ToastManager& tm, lv_obj_t* widget) {
        // detach_from_input() requires this child by name; without it the
        // strict-mode UI contract aborts before the path under test runs.
        lv_obj_t* action_btn = lv_obj_create(widget);
        lv_obj_set_name(action_btn, "toast_action_btn");
        tm.active_.push_back(ToastManager::ToastInstance{});
        auto it = std::prev(tm.active_.end());
        it->widget = widget;
        it->action_cb = noop_action_cb;
        it->action_user_data = data_42();
        it->user_closed_cb = counting_closed_cb;
    }
    static void user_close(ToastManager& tm, lv_obj_t* widget) {
        tm.user_close_toast(tm.find_by_widget(widget));
    }
    static void clear(ToastManager& tm) {
        tm.active_.clear();
    }
};

namespace {

struct UserCloseFixture : LVGLUITestFixture {
    UserCloseFixture() {
        animations_were_enabled_ =
            helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
        g_closed_count = 0;
        g_closed_data = nullptr;
    }
    ~UserCloseFixture() override {
        ToastManagerTestAccess::clear(ToastManager::instance());
        helix::DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
        drain();
    }

    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(UserCloseFixture, "Toast user close: the X path runs the closed callback once",
                 "[toast][user-close]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_t* widget = lv_obj_create(host);

    ToastManagerTestAccess::inject(tm, widget);
    ToastManagerTestAccess::user_close(tm, widget);
    ToastManagerTestAccess::user_close(tm, widget); // already exiting: no second fire

    CHECK(g_closed_count == 1);
    CHECK(g_closed_data == data_42());

    lv_obj_delete(host);
    drain();
}

TEST_CASE_METHOD(UserCloseFixture, "Toast user close: hide() does not run the closed callback",
                 "[toast][user-close]") {
    auto& tm = ToastManager::instance();
    ToastManagerTestAccess::clear(tm);
    lv_obj_t* host = lv_obj_create(test_screen());
    lv_obj_t* widget = lv_obj_create(host);

    ToastManagerTestAccess::inject(tm, widget);
    tm.hide(noop_action_cb, data_42());
    drain();

    CHECK(g_closed_count == 0);

    lv_obj_delete(host);
    drain();
}
