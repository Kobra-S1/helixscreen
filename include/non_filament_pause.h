// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <utility>

namespace helix::non_filament_pause {

/// Answers "is the current pause explained by something other than filament?"
/// with the firmware's reason for it, or an empty string when it is not. A
/// subsystem that recognises its own pauses (print-defect detection) installs
/// one; the runout surfaces ask before they read the sensors, so one pause gets
/// one explanation. Header-only so every build that carries a runout surface
/// links, whether or not it carries a subsystem that installs an explainer.
/// Main thread only.
using Explainer = std::function<std::string()>;

struct ExplainerSlot {
    const void* owner = nullptr;
    Explainer explainer;
};

inline ExplainerSlot& explainer_slot() {
    static ExplainerSlot slot;
    return slot;
}

/// Install @p owner's explainer, replacing any other.
inline void set_explainer(const void* owner, Explainer explainer) {
    explainer_slot() = {owner, std::move(explainer)};
}

/// Remove the explainer if @p owner is the one that installed it; another
/// owner's stays.
inline void clear_explainer(const void* owner) {
    ExplainerSlot& slot = explainer_slot();
    if (slot.owner == owner) {
        slot = {};
    }
}

/// The reason the installed explainer gives for the current pause; empty when
/// it does not claim it, or with none installed.
inline std::string explanation() {
    const Explainer& explainer = explainer_slot().explainer;
    return explainer ? explainer() : std::string();
}

/// True when the installed explainer claims the current pause. With none
/// installed every pause is open to the runout surfaces.
inline bool explained() {
    return !explanation().empty();
}

} // namespace helix::non_filament_pause
