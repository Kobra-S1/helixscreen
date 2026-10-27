// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_fault_path_position.cpp
 * @brief The fault-path capability answers for every firmware row it holds.
 *
 * The graphic and the modals ask helix::faultpath, never a firmware module, so
 * these pin that the question reaches AFC's row and that a message no row
 * recognises passes through untouched.
 */

#include "fault_path_position.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::faultpath::fault_position;
using helix::faultpath::strip_position_diagram;

namespace {

const std::string kAfcHubFault = "filament did not trigger hub sensor, CHECK FILAMENT PATH\n"
                                 "||=====||==>--||-----||\n"
                                 "TRG   LOAD   HUB   TOOL.";

const std::string kUnpositionedFault = "Lane 2 reset failed\n||=====||\nTRG   LOAD";

} // namespace

TEST_CASE("an AFC lane fault resolves through the fault-path capability", "[fault-path]") {
    REQUIRE(fault_position(kAfcHubFault).has_value());
    CHECK(*fault_position(kAfcHubFault) == helix::PathSegment::HUB);
    CHECK(strip_position_diagram(kAfcHubFault) ==
          "filament did not trigger hub sensor, CHECK FILAMENT PATH");
}

TEST_CASE("a fault no provider positions passes through untouched", "[fault-path]") {
    CHECK_FALSE(fault_position(kUnpositionedFault).has_value());
    CHECK(strip_position_diagram(kUnpositionedFault) == kUnpositionedFault);
}
