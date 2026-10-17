// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

namespace helix::test {

/// Press @p button_name ("btn_primary", "btn_secondary") on the top dialog.
inline void answer_top_modal(const char* button_name) {
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* button = lv_obj_find_by_name(dialog, button_name);
    REQUIRE(button != nullptr);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    ui::UpdateQueue::instance().drain();
}

/// Tap the top dialog's backdrop: the dismissal no button answers.
inline void dismiss_top_modal() {
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
    ui::UpdateQueue::instance().drain();
}

} // namespace helix::test
