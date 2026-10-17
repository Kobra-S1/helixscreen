// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The preview card's corner buttons: the view-mode toggle never covers the
// objects button, whichever order the objects and the widget tree arrive in.

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "printer_state.h"
#include "test_helpers/print_status_panel_test_access.h"

#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

/// A real PrintStatusPanel built from production XML, with two objects defined
/// before the widget tree exists: the order the app sees when the panel is
/// built lazily mid-print.
class ObjectsBeforeCreateFixture : public LVGLUITestFixture {
  public:
    ObjectsBeforeCreateFixture() {
        heal_global_print_status_panel_subjects();
        state().get_excluded_objects_state()->set_defined_objects({"A", "B"});
        UpdateQueue::instance().drain();
        panel_ = std::make_unique<PrintStatusPanel>(state(), nullptr);
        panel_->init_subjects();
        UpdateQueue::instance().drain();
        root_ = panel_->create(test_screen());
        REQUIRE(root_ != nullptr);
    }

    ~ObjectsBeforeCreateFixture() override {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
        state().get_excluded_objects_state()->set_defined_objects({});
        UpdateQueue::instance().drain();
        heal_global_print_status_panel_subjects();
    }

    PrintStatusPanel& panel() {
        return *panel_;
    }

  protected:
    lv_obj_t* root_ = nullptr;

  private:
    void heal_global_print_status_panel_subjects() {
        auto& global = get_global_print_status_panel();
        if (!global.are_subjects_initialized()) {
            global.init_subjects();
        }
    }

    std::unique_ptr<PrintStatusPanel> panel_;
};

lv_area_t transformed_area(lv_obj_t* obj) {
    // A style translate moves what the user sees and taps.
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_obj_get_transformed_area(obj, &a, LV_OBJ_POINT_TRANSFORM_FLAG_NONE);
    return a;
}

} // namespace

TEST_CASE_METHOD(ObjectsBeforeCreateFixture,
                 "The view toggle sits beside the objects button, not over it",
                 "[print_status][view_toggle]") {
    lv_subject_set_int(PrintStatusPanelTestAccess::viewer_mode(panel()), 1);
    UpdateQueue::instance().drain();
    process_lvgl(50);
    lv_obj_update_layout(root_);

    lv_obj_t* objects_btn = lv_obj_find_by_name(root_, "btn_objects");
    lv_obj_t* toggle = lv_obj_find_by_name(root_, "btn_view_toggle");
    REQUIRE(objects_btn != nullptr);
    REQUIRE(toggle != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(objects_btn, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(toggle, LV_OBJ_FLAG_HIDDEN));

    const lv_area_t a = transformed_area(objects_btn);
    const lv_area_t b = transformed_area(toggle);
    INFO("objects x1=" << a.x1 << " x2=" << a.x2 << " toggle x1=" << b.x1 << " x2=" << b.x2);
    CHECK(lv_area_get_width(&a) > 0);
    CHECK(lv_area_get_width(&b) > 0);
    const bool overlap = a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
    CHECK_FALSE(overlap);

    // With the objects button hidden, the toggle takes the corner.
    state().get_excluded_objects_state()->set_defined_objects({"Solo"});
    UpdateQueue::instance().drain();
    lv_obj_update_layout(root_);
    REQUIRE(lv_obj_has_flag(objects_btn, LV_OBJ_FLAG_HIDDEN));
    const lv_area_t solo = transformed_area(toggle);
    CHECK(solo.x1 == a.x1);
}
