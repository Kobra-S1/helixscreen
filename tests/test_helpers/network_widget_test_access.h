// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/ui/panel_widgets/network_widget.h"

namespace helix {

// Friend access to NetworkWidget's signal-poll timer, which is only observable
// on the privates. Read-only.
//
// Defined in ONE place, in namespace helix to match the friend declaration.
// Follows the tests/test_helpers/ TestAccess pattern ([L088]).
class NetworkWidgetTestAccess {
  public:
    static lv_timer_t* signal_poll_timer(const NetworkWidget& widget) {
        return widget.signal_poll_timer_;
    }
};

} // namespace helix
