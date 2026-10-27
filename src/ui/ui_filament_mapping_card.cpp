// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_mapping_card.h"

#include "ui_fonts.h"
#include "ui_swatch.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "color_utils.h"
#include "display_numbering.h"
#include "filament_chip_model.h"
#include "filament_mapper.h"
#include "filament_variants.h"
#include "helix/xml/indexed_subject_pool.h"
#include "helix_psram_attr.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observe_language.h"
#include "print_start_checks.h"
#include "settings_manager.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>

namespace helix::ui {

namespace {

using helix::xml::IndexedSubjectPool;

HELIX_PSRAM_BSS lv_subject_t s_chip_count{};
HELIX_PSRAM_BSS lv_subject_t s_more_count{};
HELIX_PSRAM_BSS lv_subject_t s_more_text{};
HELIX_PSRAM_BSS char s_more_text_buf[16];
helix::WidgetSubjects s_chip_subjects{"FilamentChipSubjects"};

// Per chip, in display order: filament_chip_<i>_material and so on.
IndexedSubjectPool s_chip_material{"filament_chip", IndexedSubjectPool::Type::String, 32,
                                   "_material"};
IndexedSubjectPool s_chip_slot{"filament_chip", IndexedSubjectPool::Type::String, 16, "_slot"};
IndexedSubjectPool s_chip_file_color{"filament_chip", IndexedSubjectPool::Type::Color, 0,
                                     "_file_color"};
IndexedSubjectPool s_chip_file_color_known{"filament_chip", IndexedSubjectPool::Type::Int, 0,
                                           "_file_color_known"};
IndexedSubjectPool s_chip_ink{"filament_chip", IndexedSubjectPool::Type::Int, 0, "_ink"};
IndexedSubjectPool s_chip_slot_color{"filament_chip", IndexedSubjectPool::Type::Color, 0,
                                     "_slot_color"};
IndexedSubjectPool s_chip_slot_colors{"filament_chip", IndexedSubjectPool::Type::String, 96,
                                      "_slot_colors"};
IndexedSubjectPool s_chip_slot_finish{"filament_chip", IndexedSubjectPool::Type::String, 32,
                                      "_slot_finish"};
IndexedSubjectPool s_chip_slot_state{"filament_chip", IndexedSubjectPool::Type::Int, 0,
                                     "_slot_state"};
IndexedSubjectPool s_chip_severity{"filament_chip", IndexedSubjectPool::Type::Int, 0, "_severity"};

IndexedSubjectPool* const s_chip_pools[] = {
    &s_chip_material,   &s_chip_slot,       &s_chip_file_color,  &s_chip_file_color_known,
    &s_chip_ink,        &s_chip_slot_color, &s_chip_slot_colors, &s_chip_slot_finish,
    &s_chip_slot_state, &s_chip_severity};

/// filament_chip_<i>_slot_state: what the chip's body draws.
enum ChipSlotState : int { kNoSlot = 0, kLoadedSlot = 1, kEmptySlot = 2 };

/// filament_chip_<i>_ink: the header label's colour.
enum ChipInk : int { kThemeInk = 0, kDarkInk = 1, kLightInk = 2 };

void init_chip_subjects() {
    s_chip_subjects.ensure_init(
        [] {
            UI_MANAGED_SUBJECT_INT(s_chip_count, 0, "filament_chip_count", s_chip_subjects);
            UI_MANAGED_SUBJECT_INT(s_more_count, 0, "filament_chip_more_count", s_chip_subjects);
            UI_MANAGED_SUBJECT_STRING(s_more_text, s_more_text_buf, "", "filament_chip_more_text",
                                      s_chip_subjects);
            for (IndexedSubjectPool* pool : s_chip_pools) {
                pool->ensure_size(FilamentMappingCard::kMaxFilamentChips);
            }
        },
        [] {
            for (IndexedSubjectPool* pool : s_chip_pools) {
                pool->reclaim();
            }
        });
}

void publish_chip(size_t i, const helix::GcodeToolInfo& tool, const helix::AvailableSlot* slot,
                  const std::vector<helix::AvailableSlot>& all_slots) {
    // The file names the tool by its material; the G-code tool is the fallback.
    s_chip_material.set_string(i, tool.material.empty() ? helix::ui::tool_label(tool.tool_index)
                                                        : tool.material);
    s_chip_file_color.set_color(i, tool.color_rgb);
    s_chip_file_color_known.set_int(i, tool.color_known ? 1 : 0);
    int ink = kThemeInk;
    if (tool.color_known) {
        ink = lv_color_eq(theme_manager_get_readable_on(lv_color_hex(tool.color_rgb)),
                          lv_color_black())
                  ? kDarkInk
                  : kLightInk;
    }
    s_chip_ink.set_int(i, ink);

    s_chip_slot.set_string(i, slot ? helix::chip_slot_label(*slot, all_slots)
                                   : std::string(lv_tr("Pick")));
    s_chip_slot_color.set_color(i, slot ? slot->color_rgb : 0);
    s_chip_slot_colors.set_string(i, slot ? slot->multi_color_hexes : std::string{});
    s_chip_slot_finish.set_string(i, slot ? std::string(helix::filament_finish_name(slot->finish))
                                          : std::string{});
    s_chip_slot_state.set_int(i, !slot ? kNoSlot : (slot->is_empty ? kEmptySlot : kLoadedSlot));
    s_chip_severity.set_int(i, static_cast<int>(helix::chip_severity(tool, slot)));
}

} // namespace

// ============================================================================
// Setup
// ============================================================================

void FilamentMappingCard::create(lv_obj_t* card_widget, lv_obj_t* rows_container,
                                 lv_obj_t* warning_container) {
    card_ = card_widget;
    rows_container_ = rows_container;
    warning_container_ = warning_container;

    // Make the entire card tappable. If an on_tap override is set (the print
    // detail view routes the tap to the panel's single open_remap_modal()), fire
    // that; otherwise fall back to the card's own internal mapping modal.
    if (card_) {
        lv_obj_add_flag(card_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            card_,
            [](lv_event_t* e) {
                auto* self = static_cast<FilamentMappingCard*>(lv_event_get_user_data(e));
                if (self->on_tap_) {
                    self->on_tap_();
                } else {
                    self->open_mapping_modal();
                }
            },
            LV_EVENT_CLICKED, this);
    }

    init_chip_subjects();
    language_observer_ =
        observe_language_change(this, [](FilamentMappingCard* self) { self->publish_chips(); });

    spdlog::debug("[FilamentMapping] Card created");
}

// ============================================================================
// Update / visibility
// ============================================================================

bool FilamentMappingCard::recompute_visibility() {
    // The WHOLE visibility rule, in one place, so update() and set_used_tools()
    // cannot reach different answers from the same state. It reads only
    // used_tools_ and the two palette counts update() stored - never tool_info_,
    // whose size apply_used_tools_filter() shrinks, which would make a second
    // set_used_tools() for the same file decide against a number the first call
    // had already trimmed.
    // Visibility is published via the `filament_mapping_visible` subject by the
    // detail view — see PrintSelectDetailView::publish_card_visibility().
    should_show_ = card_ && rows_container_ &&
                   shows_mapping_for(palette_colour_count_, palette_tool_count_, used_tools_);
    return should_show_;
}

bool FilamentMappingCard::shows_mapping_for(size_t palette_colour_count, size_t palette_tool_count,
                                            const std::optional<std::set<int>>& used_tools) {
    auto& ams = AmsState::instance();
    if (!ams.is_available()) {
        return false;
    }

    // NO editable-backend gate here. It once existed to avoid a dead control on
    // Snapmaker U1 / ACE, back when a second surface (the print-detail FILAMENTS
    // card) drew the same chips there. That surface is gone, so hiding here would
    // show the user nothing at all. Whether a TAP does anything is a separate
    // question, answered by PrintSelectDetailView::current_remap_block().

    // A dead-control rule survives here even without the editable-backend gate
    // above: with bypass engaged a single-tool print takes its filament from
    // the external spool, and print_start_checks.cpp compares
    // against that spool instead of the lanes (the `any_bypass_active &&
    // print_lane_requirement(...) <= 1` short-circuit). Offering a lane mapping
    // there claims something the print will not do — a K2 Plus user read the
    // chips as "this maps to lane 2", tapped one to confirm it, and started a
    // print that ran on the bypass spool. A genuinely multi-lane print still
    // uses the mapping with bypass on, so this only hides the <= 1 case.
    //
    // print_lane_requirement() is shared with the gate rather than reimplemented
    // here: it prefers the scan's tools_used and falls back to the palette, and
    // a second copy of that precedence would drift into exactly the mismatch
    // this hides.
    //
    // The fallback is the COLOUR count, not the tool count: the two differ when
    // a slicer reports materials and no colours (every OrcaSlicer file on a
    // K2 Plus), and this gate has always asked "how many lanes does the file
    // want", which is a question about colours.
    if (ams.any_bypass_active() &&
        helix::print_lane_requirement(used_tools ? *used_tools : std::set<int>{},
                                      palette_colour_count) <= 1) {
        return false;
    }

    // Nothing to map. Equivalent to the build_tool_info() result being empty:
    // it emits max(colours, materials) entries and filters none.
    if (palette_tool_count == 0) {
        return false;
    }

    // Moved from PrintSelectDetailView::swatches_card_visible_for(): on a
    // multi-tool printer any referenced tool is worth showing (lane identity
    // matters); on a single extruder it takes 2+ tools to be a manual-swap
    // multi-colour file rather than an ordinary single-colour print.
    //
    // print_lane_requirement() (shared with the bypass gate above) is reused
    // rather than a hand-written used_tools_->size() : tool_info_.size()
    // fallback — every OTHER reader of used_tools_ in this class treats an
    // empty set as "no answer, show all" (the bypass gate above,
    // apply_used_tools_filter()'s documented contract), and a second,
    // divergent copy of "empty means fall back to the palette count" is
    // exactly the drift this merge exists to remove. An empty set is not a
    // not-yet-computed sentinel either — PrintSelectDetailView persists a
    // successful zero-tool scan as a legitimate single-extruder answer.
    const int ams_slots = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    const bool is_multi_tool_printer =
        helix::ToolState::instance().is_multi_tool() || ams_slots > 1;
    const size_t tool_count = helix::print_lane_requirement(
        used_tools ? *used_tools : std::set<int>{}, palette_tool_count);
    if (!(is_multi_tool_printer ? tool_count > 0 : tool_count > 1)) {
        return false;
    }

    return true;
}

FilamentMappingCard::Mapping
FilamentMappingCard::compute_mapping(const std::vector<std::string>& colors,
                                     const std::vector<std::string>& materials,
                                     const std::optional<std::set<int>>& used_tools) {
    Mapping m;
    m.palette_colour_count = colors.size();
    m.palette_tool_count = std::max(colors.size(), materials.size());
    // A pure function of the palette, built whether or not the card shows:
    // the print-start gate reads the tools either way.
    m.tool_info = build_tool_info(colors, materials);
    m.shows = shows_mapping_for(m.palette_colour_count, m.palette_tool_count, used_tools);
    if (m.shows) {
        auto& ams = AmsState::instance();
        // Seeded through the shared rule, which reads the EFFECTIVE auto-match
        // predicate: AmsState::effective_auto_match() overrides the raw setting
        // on backends (Snapmaker U1) where the two differ.
        m.slots = ams.collect_available_slots();
        m.mappings = ams.seed_tool_mappings(m.tool_info, m.slots);
    }
    // Seeded from the whole palette, then narrowed to the tools the file uses;
    // nullopt or empty keeps them all.
    apply_used_tools_filter(m.tool_info, m.mappings, used_tools);
    return m;
}

void FilamentMappingCard::update(const std::vector<std::string>& gcode_colors,
                                 const std::vector<std::string>& gcode_materials) {
    seed_from_palette(gcode_colors, gcode_materials, {});
}

void FilamentMappingCard::refresh_palette(const std::vector<std::string>& gcode_colors,
                                          const std::vector<std::string>& gcode_materials) {
    // A copy: seeding replaces mappings_ before the picks are laid back over it.
    const std::vector<helix::ToolMapping> picks = mappings_;
    seed_from_palette(gcode_colors, gcode_materials, picks);
}

void FilamentMappingCard::seed_from_palette(const std::vector<std::string>& gcode_colors,
                                            const std::vector<std::string>& gcode_materials,
                                            const std::vector<helix::ToolMapping>& keep_picks) {
    Mapping m = compute_mapping(gcode_colors, gcode_materials, used_tools_);
    // Kept, because set_used_tools() re-runs the visibility rule without a
    // palette to hand.
    palette_colour_count_ = m.palette_colour_count;
    palette_tool_count_ = m.palette_tool_count;
    palette_colors_ = gcode_colors;
    palette_materials_ = gcode_materials;
    // Assigned before the visibility early-out: PrintSelectDetailView's
    // get_filament_tool_info() hands it to the print-start gate whether or not
    // the card shows, and must never hand it the previous file's tools.
    tool_info_ = std::move(m.tool_info);

    if (!recompute_visibility_or_clear()) {
        return;
    }

    available_slots_ = std::move(m.slots);
    mappings_ = std::move(m.mappings);
    helix::FilamentMapper::apply_picks(mappings_, keep_picks);

    // Build the compact UI
    publish_chips();

    spdlog::debug("[FilamentMapping] Updated: {} tools, {} slots, {} mappings", tool_info_.size(),
                  available_slots_.size(), mappings_.size());
}

bool FilamentMappingCard::recompute_visibility_or_clear() {
    if (recompute_visibility()) {
        return true;
    }
    // A hidden card has no mappings of its own: whatever it held (the previous
    // file's, or picks made before bypass engaged) must not reach print start.
    available_slots_.clear();
    mappings_.clear();
    return false;
}

void FilamentMappingCard::refresh_slot_data() {
    if (!card_ || !rows_container_) {
        return;
    }
    // The slots can take away what made the card worth showing (the backend
    // going away, bypass engaging), so the rule runs again first.
    if (!recompute_visibility_or_clear()) {
        publish_chips();
        return;
    }
    if (!AmsState::instance().is_available()) {
        return;
    }
    available_slots_ = AmsState::instance().collect_available_slots();
    const Mapping fresh = compute_mapping(palette_colors_, palette_materials_, used_tools_);
    const bool followed =
        fresh.shows && helix::FilamentMapper::follow_seed(mappings_, fresh.mappings);
    publish_chips();
    if (followed && on_mappings_changed_) {
        on_mappings_changed_();
    }
}

void FilamentMappingCard::set_used_tools(std::optional<std::set<int>> used) {
    used_tools_ = std::move(used);

    // Re-decide visibility FIRST. Both lane-count gates read used_tools_, so the
    // precise set arriving here can flip an answer update() could only reach
    // from the palette: a bypassed print of a 3-colour file that really uses one
    // tool wants one lane, not three, and the bypass gate exists to hide exactly
    // that. Without this the card stayed visible on the palette's say-so and
    // then trimmed itself to a single chip - the K2 Plus state where a user read
    // a chip as a lane claim and printed off the bypass spool anyway.
    //
    // Callers must re-publish afterwards; PrintSelectDetailView does that from
    // publish_card_visibility(), which is why this does not reach for a subject
    // itself - the card has never owned one.
    if (recompute_visibility_or_clear() && mappings_.empty()) {
        // Shown again after a narrower set hid it (and cleared it): seed afresh.
        Mapping m = compute_mapping(palette_colors_, palette_materials_, used_tools_);
        tool_info_ = std::move(m.tool_info);
        available_slots_ = std::move(m.slots);
        mappings_ = std::move(m.mappings);
    }

    // Then compact the card's current tool_info_/mappings_ in lockstep. This
    // only filters; it never re-seeds. nullopt/empty is a no-op (show all). Does NOT touch the
    // detail view's full-palette copies (current_filament_colors_/materials) — only the card's own
    // vectors.
    apply_used_tools_filter(tool_info_, mappings_, used_tools_);
    publish_chips();
}

bool FilamentMappingCard::has_mismatch() const {
    return has_any_mismatch();
}

void FilamentMappingCard::on_ui_destroyed() {
    card_ = nullptr;
    rows_container_ = nullptr;
    warning_container_ = nullptr;
    language_observer_.reset();
}

// ============================================================================
// Chips
// ============================================================================

void FilamentMappingCard::init_subjects() {
    init_chip_subjects();
}

void FilamentMappingCard::publish_chips() {
    if (!rows_container_) {
        return;
    }
    init_chip_subjects();

    // A card that is not going to be shown publishes no chips: they would carry
    // a lane claim the visibility rule has just decided not to make.
    if (!should_show_) {
        lv_subject_set_int(&s_more_count, 0);
        lv_subject_set_int(&s_chip_count, 0);
        return;
    }

    // ONE row, and what runs past its right edge is clipped, not wrapped: cap
    // the chips at what fits and say so with a "+N" pill, which takes a chip's
    // place. Dropping the rest silently would hide lanes the print uses.
    const int32_t gap = theme_manager_get_spacing("space_xs");
    int32_t chip_width = theme_manager_get_spacing("filament_chip_width");
    if (chip_width <= 0) {
        chip_width = kChipWidth;
    }
    const size_t capacity =
        std::min(chips_that_fit(lv_obj_get_content_width(rows_container_), gap, chip_width),
                 kMaxFilamentChips);
    const size_t tool_count = tool_info_.size();
    const bool overflow = tool_count > capacity;
    const size_t visible = overflow ? capacity - 1 : tool_count;

    for (size_t i = 0; i < visible; ++i) {
        // mappings_ is built parallel to tool_info_ and compacted in lockstep by
        // apply_used_tools_filter, so index i is this tool's mapping.
        const ToolMapping mapping = (i < mappings_.size()) ? mappings_[i] : ToolMapping{};
        // Judged against the slots as they are now, never a cached verdict:
        // refresh_slot_data() replaces available_slots_ while a pick stays put.
        publish_chip(i, tool_info_[i],
                     helix::FilamentMapper::resolve_mapped_slot(mapping, available_slots_),
                     available_slots_);
    }

    const size_t hidden = tool_count - visible;
    lv_subject_copy_string(&s_more_text, ("+" + std::to_string(hidden)).c_str());
    lv_subject_set_int(&s_more_count, static_cast<int>(hidden));
    // Last: the count rebuilds the chips, which then bind the values above.
    lv_subject_set_int(&s_chip_count, static_cast<int>(visible));
    spdlog::debug("[FilamentMapping] {} tool(s): {} chip(s) of {} that fit{}", tool_count, visible,
                  capacity, overflow ? " + overflow pill" : "");
}

bool FilamentMappingCard::has_any_mismatch() const {
    for (const auto& m : mappings_) {
        if (m.material_mismatch) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Color queries
// ============================================================================

std::vector<uint32_t> FilamentMappingCard::get_mapped_colors() const {
    return helix::FilamentMapper::resolve_display_colors(tool_info_, mappings_, available_slots_);
}

// ============================================================================
// Modal interaction
// ============================================================================

void FilamentMappingCard::open_mapping_modal() {
    spdlog::debug("[FilamentMapping] Opening mapping modal");

    mapping_modal_.set_tool_info(tool_info_);
    mapping_modal_.set_available_slots(available_slots_);
    mapping_modal_.set_mappings(mappings_);
    mapping_modal_.set_on_mappings_updated(
        [this](auto mappings) { set_mappings(std::move(mappings)); });
    mapping_modal_.show(lv_screen_active());
}

// ============================================================================
// Data collection
// ============================================================================

std::vector<helix::GcodeToolInfo>
FilamentMappingCard::build_used_tool_info(const std::vector<std::string>& colors,
                                          const std::vector<std::string>& materials,
                                          const std::set<int>& used) {
    if (used.empty()) {
        return {};
    }
    const auto all_tool_info = build_tool_info(colors, materials);

    std::vector<helix::GcodeToolInfo> tools;
    tools.reserve(used.size());
    for (int tool : used) {
        if (tool >= 0 && static_cast<size_t>(tool) < all_tool_info.size()) {
            auto info = all_tool_info[static_cast<size_t>(tool)];
            info.tool_index = tool; // real gcode tool number, not palette ordinal
            tools.push_back(info);
        }
    }
    return tools;
}

std::vector<helix::GcodeToolInfo>
FilamentMappingCard::build_tool_info(const std::vector<std::string>& colors,
                                     const std::vector<std::string>& materials) {
    std::vector<helix::GcodeToolInfo> tools;

    // Use the larger of colors or materials to determine tool count.
    // If both are empty, return empty — the card will be hidden.
    size_t count = std::max(colors.size(), materials.size());
    if (count == 0) {
        return tools;
    }

    for (size_t i = 0; i < count; ++i) {
        helix::GcodeToolInfo tool;
        tool.tool_index = static_cast<int>(i);

        // Parse color. A missing or unparsable entry leaves color_rgb as a
        // NEUTRAL STAND-IN, not an answer: Moonraker omits filament_colors
        // entirely for some slicers (every OrcaSlicer file on a K2 Plus reports
        // filament_type and nothing else), and the palette is backfilled later
        // from the G-code footer or the viewer. Flagging it keeps the stand-in
        // from being read as the file's choice — see GcodeToolInfo::color_known.
        if (i < colors.size() && !colors[i].empty()) {
            auto parsed = helix::parse_hex_color(colors[i]);
            tool.color_rgb = parsed.value_or(0x808080);
            tool.color_known = parsed.has_value();
        } else {
            tool.color_rgb = 0x808080;
            tool.color_known = false;
        }

        // Material
        if (i < materials.size()) {
            tool.material = materials[i];
        }

        tools.push_back(std::move(tool));
    }

    return tools;
}

size_t FilamentMappingCard::chips_that_fit(int32_t content_width, int32_t gap, int32_t chip_width) {
    // Layout has not settled on the very first render, so the width reads 0 (or
    // negative once padding is subtracted). Fall back to the floor rather than
    // to "everything": drawing past the right edge is what silently loses lanes.
    if (content_width <= 0) {
        return kMinVisibleChips;
    }
    // n chips occupy n*chip_width + (n-1)*gap, hence the (+ gap) on both sides.
    const int32_t n = (content_width + gap) / (std::max<int32_t>(1, chip_width) + gap);
    // Always offer one slot: a row too narrow for a single chip still has to show
    // the "+N" pill rather than nothing at all.
    return static_cast<size_t>(std::max<int32_t>(1, n));
}

void FilamentMappingCard::apply_used_tools_filter(std::vector<helix::GcodeToolInfo>& tool_info,
                                                  std::vector<helix::ToolMapping>& mappings,
                                                  const std::optional<std::set<int>>& used) {
    // nullopt OR empty set ⇒ no filter (show all). Safety rule: never blank the
    // card pre-parse, and never hide everything on the headless single-extruder
    // path (where the used set is empty forever).
    if (!used || used->empty()) {
        return;
    }
    const std::set<int>& keep = *used;

    // Filter BOTH vectors independently by their own .tool_index — they are
    // built parallel (mappings_[i].tool_index == tool_info_[i].tool_index), so
    // the same predicate compacts them in lockstep. std::remove_if preserves
    // order; .tool_index is retained (used for the "T%d" label + modal rows).
    tool_info.erase(std::remove_if(tool_info.begin(), tool_info.end(),
                                   [&keep](const helix::GcodeToolInfo& t) {
                                       return keep.count(t.tool_index) == 0;
                                   }),
                    tool_info.end());
    mappings.erase(std::remove_if(mappings.begin(), mappings.end(),
                                  [&keep](const helix::ToolMapping& m) {
                                      return keep.count(m.tool_index) == 0;
                                  }),
                   mappings.end());
}

const helix::GcodeToolInfo*
FilamentMappingCard::find_by_tool_index(const std::vector<helix::GcodeToolInfo>& tool_info,
                                        int tool_index) {
    for (const auto& t : tool_info) {
        if (t.tool_index == tool_index) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace helix::ui
