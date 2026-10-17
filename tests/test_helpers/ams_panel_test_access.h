// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_ams.h"

namespace helix {

/// Friend access to AmsPanel's path-canvas tap routing.
class AmsPanelTestAccess {
  public:
    static lv_obj_t* path_canvas(const AmsPanel& p) {
        return p.path_canvas_;
    }
    /// A tap on the path canvas's head for @p slot, as the canvas hit-test reports it.
    static void tap_toolhead(AmsPanel& p, int slot) {
        AmsPanel::on_path_toolhead_clicked(slot, &p);
    }
    /// A tap on the path canvas's tube or spool for @p slot.
    static void tap_lane(AmsPanel& p, int slot) {
        AmsPanel::on_path_slot_clicked(slot, &p);
    }
    static bool context_menu_open(const AmsPanel& p) {
        return p.context_menu_ && p.context_menu_->is_visible();
    }
};

} // namespace helix
