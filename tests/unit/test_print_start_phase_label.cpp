// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_phase_label.h"

#include "../catch_amalgamated.hpp"

using helix::OperationCategory;
using helix::phase_of_category;
using helix::PrintStartPhase;

TEST_CASE("phase of category: every operation category, the non-phases included",
          "[print_start_phase_label]") {
    CHECK(phase_of_category(OperationCategory::BED_MESH) == PrintStartPhase::BED_MESH);
    CHECK(phase_of_category(OperationCategory::QGL) == PrintStartPhase::QGL);
    CHECK(phase_of_category(OperationCategory::Z_TILT) == PrintStartPhase::Z_TILT);
    CHECK(phase_of_category(OperationCategory::NOZZLE_CLEAN) == PrintStartPhase::CLEANING);
    CHECK(phase_of_category(OperationCategory::PURGE_LINE) == PrintStartPhase::PURGING);
    CHECK(phase_of_category(OperationCategory::HOMING) == PrintStartPhase::HOMING);
    CHECK(phase_of_category(OperationCategory::CHAMBER_SOAK) == PrintStartPhase::SOAKING);

    // A parent category, a correction and the start macro itself are no
    // pre-print phase of their own.
    CHECK_FALSE(phase_of_category(OperationCategory::BED_LEVEL).has_value());
    CHECK_FALSE(phase_of_category(OperationCategory::SKEW_CORRECT).has_value());
    CHECK_FALSE(phase_of_category(OperationCategory::START_PRINT).has_value());
    CHECK_FALSE(phase_of_category(OperationCategory::UNKNOWN).has_value());
}
