// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_discovery_sequence.h"

namespace helix {

/// Reads the MCU bucket parse_objects() fills, which has no public accessor.
class MoonrakerDiscoverySequenceTestAccess {
  public:
    static const std::vector<std::string>& mcus(const MoonrakerDiscoverySequence& seq) {
        return seq.mcus_;
    }
};

} // namespace helix
