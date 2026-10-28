// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_chip_model.h"

#include "display_numbering.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <utility>

namespace helix {

ChipSeverity chip_severity(const GcodeToolInfo& tool, const AvailableSlot* resolved) {
    if (!resolved || resolved->is_empty) {
        return ChipSeverity::Block;
    }
    const MismatchFlags flags = FilamentMapper::classify_mismatches(tool, *resolved);
    if (flags.material_mismatch) {
        return ChipSeverity::Block;
    }
    return flags.color_mismatch ? ChipSeverity::Warn : ChipSeverity::Ok;
}

ChipSeverity mapping_severity(const GcodeToolInfo& tool, const ToolMapping& mapping,
                              const AvailableSlot* lane) {
    if (!lane && mapping.is_auto) {
        return ChipSeverity::Ok;
    }
    return chip_severity(tool, lane);
}

std::string chip_slot_label(const AvailableSlot& slot,
                            const std::vector<AvailableSlot>& all_slots) {
    std::vector<std::pair<int, int>> units; // (backend_index, unit_index), first-seen order
    for (const auto& s : all_slots) {
        const std::pair<int, int> unit{s.backend_index, s.unit_index};
        if (std::find(units.begin(), units.end(), unit) == units.end()) {
            units.push_back(unit);
        }
    }
    const std::string number = helix::ui::lane_number_text(slot.local_slot_index);
    if (units.size() <= 1) {
        return number;
    }
    const auto it = std::find(units.begin(), units.end(),
                              std::pair<int, int>{slot.backend_index, slot.unit_index});
    const int unit_pos = static_cast<int>(it - units.begin());
    return fmt::format("{}{}{}", helix::ui::lane_number_text(unit_pos), helix::ui::kUnitSeparator,
                       number);
}

namespace {

SlotFit fit_of(const GcodeToolInfo& tool, const AvailableSlot& slot) {
    if (slot.is_empty) {
        return SlotFit::Empty;
    }
    const MismatchFlags flags = FilamentMapper::classify_mismatches(tool, slot);
    if (flags.material_mismatch) {
        return SlotFit::MaterialDiffers;
    }
    return flags.color_mismatch ? SlotFit::ColorDiffers : SlotFit::Exact;
}

} // namespace

std::vector<RankedSlot> rank_slots_for_tool(const GcodeToolInfo& tool,
                                            const std::vector<AvailableSlot>& slots,
                                            const std::vector<GcodeToolInfo>& tools,
                                            const std::vector<ToolMapping>& mappings) {
    std::vector<RankedSlot> ranked;
    ranked.reserve(slots.size());
    for (std::size_t pos = 0; pos < slots.size(); ++pos) {
        RankedSlot r;
        r.slot_pos = pos;
        r.fit = fit_of(tool, slots[pos]);
        const std::size_t n = std::min(tools.size(), mappings.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (tools[i].tool_index == tool.tool_index) {
                continue;
            }
            if (FilamentMapper::resolve_mapped_slot(mappings[i], slots) == &slots[pos]) {
                r.also_tools.push_back(tools[i].tool_index);
            }
        }
        ranked.push_back(std::move(r));
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const RankedSlot& a, const RankedSlot& b) {
        return static_cast<int>(a.fit) < static_cast<int>(b.fit);
    });
    return ranked;
}

std::string slot_fit_text(const RankedSlot& ranked) {
    std::string text;
    switch (ranked.fit) {
    case SlotFit::Exact:
        text = lv_tr("exact match");
        break;
    case SlotFit::ColorDiffers:
        text = lv_tr("color differs");
        break;
    case SlotFit::MaterialDiffers:
        text = lv_tr("material differs");
        break;
    case SlotFit::Empty:
        text = lv_tr("empty");
        break;
    }
    if (!ranked.also_tools.empty()) {
        std::string tools;
        for (const int t : ranked.also_tools) {
            if (!tools.empty()) {
                tools += ", ";
            }
            tools += helix::ui::tool_label(t);
        }
        text += fmt::format(lv_tr(", also {}"), tools);
    }
    return text;
}

std::string used_materials_text(const std::vector<std::string>& materials,
                                const std::optional<std::set<int>>& used_tools) {
    std::vector<std::string> picked;
    const auto add = [&picked](const std::string& m) {
        if (m.empty()) {
            return;
        }
        // Exact spelling: materials_match() folds a family together ("ABS-GF" is an "ABS"),
        // and the details row names each material the file asks for.
        if (std::find(picked.begin(), picked.end(), m) == picked.end()) {
            picked.push_back(m);
        }
    };
    if (used_tools) {
        for (const int t : *used_tools) {
            if (t >= 0 && static_cast<std::size_t>(t) < materials.size()) {
                add(materials[static_cast<std::size_t>(t)]);
            }
        }
    } else {
        for (const auto& m : materials) {
            add(m);
        }
    }
    std::string text;
    for (const auto& m : picked) {
        if (!text.empty()) {
            text += ", ";
        }
        text += m;
    }
    return text;
}

ChipSeverity loaded_severity(const GcodeToolInfo& tool, const LoadedFilament& loaded,
                             LoadedRole role) {
    if (!loaded.known || role == LoadedRole::Swap) {
        return ChipSeverity::Ok;
    }
    AvailableSlot slot{};
    slot.color_rgb = loaded.color_rgb;
    slot.material = loaded.material;
    slot.is_empty = false;
    GcodeToolInfo judged = tool;
    judged.color_known = tool.color_known && loaded.color_known;
    return chip_severity(judged, &slot);
}

} // namespace helix
