// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_patterns.h"
#include "print_start_phase.h"

#include <optional>

namespace helix {

/// Short translated step name for a pre-print checklist row, no ellipsis.
/// "" for IDLE, INITIALIZING and COMPLETE.
[[nodiscard]] const char* print_start_phase_step_label(PrintStartPhase phase);

/// The one OperationCategory -> PrintStartPhase mapping. nullopt for
/// categories that are not a pre-print phase of their own (BED_LEVEL,
/// SKEW_CORRECT, START_PRINT, UNKNOWN).
[[nodiscard]] std::optional<PrintStartPhase> phase_of_category(OperationCategory category);

} // namespace helix
