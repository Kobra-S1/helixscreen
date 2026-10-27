// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bed_occupancy_state.h"

namespace helix {

/// Sets BedOccupancyState's verdict directly, so a plate test can put a part of a chosen
/// height on the bed without staging a print history that produces it.
class BedOccupancyStateTestAccess {
  public:
    static void set_verdict(BedOccupancyState& state, const BedOccupancy& verdict) {
        state.current_ = verdict;
        if (state.subjects_initialized_) {
            lv_subject_set_int(&state.occupied_, verdict.occupied ? 1 : 0);
        }
        state.bump_version();
    }
};

} // namespace helix
