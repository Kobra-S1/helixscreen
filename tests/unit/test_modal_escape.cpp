// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_modal.h"
#include "ui_runout_guidance_modal.h"
#include "ui_spaghetti_detection_modal.h"

#include "../lvgl_ui_test_fixture.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

void press_escape(lv_obj_t* dialog) {
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    uint32_t key = LV_KEY_ESC;
    lv_obj_send_event(backdrop, LV_EVENT_KEY, &key);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ESC closes the spaghetti modal without aborting the print",
                 "[modal][escape]") {
    int aborted = 0, resumed = 0;
    auto owned = std::make_unique<SpaghettiDetectionModal>();
    auto* modal = owned.get();
    modal->set_on_abort([&] { ++aborted; });
    modal->set_on_resume([&] { ++resumed; });
    modal->set_detection("detected noodle", nullptr);
    REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);

    press_escape(dialog);
    process_lvgl(50);

    CHECK(aborted == 0);
    CHECK(resumed == 0);
    CHECK(ModalStack::instance().top_dialog() != dialog);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ESC closes the runout modal without resuming the print",
                 "[modal][escape]") {
    int resumed = 0, cancelled = 0;
    auto owned = std::make_unique<RunoutGuidanceModal>();
    owned->set_on_resume([&] { ++resumed; });
    owned->set_on_cancel_print([&] { ++cancelled; });
    REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);

    press_escape(dialog);
    process_lvgl(50);

    CHECK(resumed == 0);
    CHECK(cancelled == 0);
    CHECK(ModalStack::instance().top_dialog() != dialog);
}
