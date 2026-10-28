# Filament chips with an inline slot dropdown: design

Status: design, from a conversation with Preston on 2026-10-27, not yet planned.

## Why

The FILAMENTS card on the Print File detail screen draws one 40px two-band chip per G-code
tool. The top band is the file's filament and the bottom band is the mapped slot. Preston's
reaction on a real file (`ssr_heat_sink_orca`, which uses only T4, ABS-GF) was that the chips
"feel backwards", and that a red X in the card header beside an outline-less chip "looks a bit
odd". Three things are actually wrong:

- **The details row contradicts the chip.** The file's details read "ABS", while the chip reads
  "ABS-GF". The details row takes the first entry of the slicer's filament list
  (`src/ui/ui_panel_print_select.cpp`, `filament_types.front()`), which is T0, a tool this print
  never uses. The chip filters to the tools the print uses and is right.
- **The state is hard to read.** An unmapped chip draws no bottom band and no outline. The
  mismatch and empty-slot lamps sit in the card header (`filament_mismatch_icon`,
  `empty_tools_warning_icon`), away from the chip they are about.
- **Changing one mapping takes a full screen.** Bambu's printer UI shows a compact chip (the
  material header in the filament colour, a spool, "A2" and a caret) that opens a dropdown in
  place. Preston wants that.

Success: a user reads each chip on its own, and knows what the file asks for, what the printer
will feed, and whether that is a problem. They can change one tool's slot with two taps, without
leaving the screen.

## Goals

1. A new chip, matching the approved mockup (scratchpad `chip/m.png`, "Proposed"):
   - a header band filled with the file's colour for the tool and labelled with the file's
     material (or the G-code tool name when the file names no material);
   - a body showing a spool glyph tinted with the mapped slot's actual colour, the slot label,
     and a caret;
   - an outline that always shows.
2. One signal per chip, in its outline:
   - grey when matched;
   - amber when the colour differs;
   - red when the material differs, or the slot is empty or missing. In that case the slot text
     is red too, and an unmapped tool reads "Pick".
   The header lamps are removed.
3. A tap on a chip opens a dropdown anchored under it, listing every slot best first with the
   reason. The current pick is highlighted. "More options…" at the bottom opens today's full
   mapping modal.
4. Slot labels come from the backend's lane abstraction, never a bare number (below).
5. On a printer with one extruder and no filament system, the card shows the file's filament
   against what is loaded, with no dropdown.
6. The details-row material comes from the same used-tools list the chips use.

## Non-goals

- Changing the full mapping modal (`filament_mapping_modal.xml`). It stays as the "More
  options…" destination.
- Changing how mappings are applied at print start (the remap path, RemapBlock rules).
- The preflight check modal's per-tool rows. They can adopt the chip later.

## Anti-goals

- A bare number on a chip where it could be read as T-numbering or as the wrong unit's slot.
- A dropdown that offers a remap the backend cannot perform. It must respect
  `PrintSelectDetailView::current_remap_block()` exactly as the card does today.
- A second implementation of "how well does slot X fit tool Y". Ranking reuses
  `FilamentMapper` (`classify_mismatches`, `materials_match`, `color_distance`).
- Re-introducing the bypass dead control. The card stays hidden when bypass is on and the print
  needs at most one lane (`shows_mapping_for`).

## Decisions (Preston's)

### Labels

- Each backend reports its word through `AmsBackend::lane_noun()`. The word is turned into text
  only through `include/display_numbering.h`.
- **One unit:** the chip shows the bare 1-based number ("3"). The chip is 64px wide, four to a row
  at 800x480 (Preston chose this over a 72px "Lane 3" chip, three to a row). The noun appears in
  the dropdown, whose rows and heading use `lane_label()` ("Lane 3").
- **More than one unit:** the chip shows the unit index and the number joined by the middle dot
  (`kUnitSeparator`): "2·1". The dropdown spells the full unit-scoped label
  (`lane_label(noun, unit_display_name, index)`, e.g. "Turtle 2 · Lane 1").
- The G-code side is always `tool_label()` ("T4"), 0-based and never renumbered. It titles the
  dropdown ("T4 · ABS-GF").

### Mismatch signal

- The outline only, with three levels (grey, amber, red). The severities are the existing ones:
  an empty slot or no slot blocks the print (danger), a material mismatch is a warning, and a
  colour mismatch is the amber surround the chip draws today.
- The header icons go away. The card already tells the print-start path about a blocking state;
  that stays.

### Dropdown

- Every slot, best first. For each: spool colour, label, material and a short reason ("exact
  match", "color differs", "material differs", "empty").
- The current pick is highlighted, and "More options…" opens the full modal.
- Choosing a slot applies that one tool's mapping the same way the modal does (one shared apply
  path), and the chips redraw.

### One extruder, no filament system

- Every file with a known material shows the card. That is a new visibility path:
  `shows_mapping_for` hides the card today when no filament system is available.
- There is one chip per used tool. Its header is that tool's filament from the file. Its body is
  compared with what is loaded now: the active spool if Spoolman or HelixPrint knows it,
  otherwise the extruder's known material.
- The first used tool's chip reads "Loaded". Later tools read "Swap", because a manual-swap
  (M600) file changes filament mid-print.
- When the loaded filament is unknown, the outline is grey and the body reads "?".
- The outline follows the same three levels. There is no caret and no dropdown.

## Constraints

- Card size at 800x480: 296x85 outer, 274x69 content. Chips are about 64px wide, 4 to a row,
  with the existing "+N" pill (`filament_mapping_more_pill.xml`) past that. Measure the smallest
  breakpoint with `ctl geom` before fixing the width.
- `.claude/rules/declarative-ui.md`: chips are a C++-generated collection, so per-item paint is
  allowed. Static styles and states belong in XML (`filament_swatch.xml` or its successor).
- Colours are painted through the swatch painter (`helix::ui::apply_swatch_color`,
  `docs/devel/COLOR_SWATCHES.md`): one colour-swatch language app-wide.
- The dropdown is `FilamentSlotPicker`, the one the mapping modal already uses per row.
- No vendor names in the card. The noun comes from the backend.
- The ESP32 firmware image is over its size budget (2026-10-27). This card ships in the firmware,
  so the plan reports the size change and keeps new code small.
- US English UI strings, all locales, glossary terms.

## Reuse map (searched)

| Need | Exists |
|---|---|
| Lane word and labels | `include/display_numbering.h` (`LaneNoun`, `lane_label`, `lane_number`, `tool_label`, `kUnitSeparator`, `pick_tool_label_form`) |
| Backend noun | `AmsBackend::lane_noun()` |
| Slot data | `helix::AvailableSlot` (`unit_index`, `local_slot_index`, `unit_display_name`, colour, material, empty) |
| Match logic | `FilamentMapper::classify_mismatches`, `materials_match`, `color_distance`, `resolve_mapped_slot` |
| Anchored dropdown | `FilamentSlotPicker` (`include/ui_filament_slot_picker.h`): the mapping modal's own per-row slot dropdown, with `Options` for a heading, trigger highlight (swatch triggers), an extra no-slot row and an Auto row. The chip opens THIS picker; extend it for ranking and reasons, never fork it |
| Applying one pick | `FilamentMappingModal::on_slot_selected` (sets slot, backend, is_auto, user_picked, re-classifies). Extract it into one shared function the modal and the card both call |
| Swatch paint | `helix::ui::apply_swatch_color`, `swatch_finish` |
| Remap gating | `PrintSelectDetailView::current_remap_block()`, `color_card_remappable_` |
| Used tools | `FilamentMappingCard::set_used_tools`, `apply_used_tools_filter`, `print_lane_requirement` |

### Declarative (Preston, 2026-10-27: "refactor for pure declarative approach as well")

- The chips are `<repeat count="filament_chip_count">` in a dedicated container, each
  iteration binding its own indexed subjects (`filament_chip_${i}_material`, `_slot`,
  `_file_color`, `_file_color_known`, `_slot_color`, `_slot_colors`, `_slot_finish`,
  `_slot_set`, `_severity`, `_tappable`). Colours bind through `<swatch_paint bind_color>` and
  `<color_dot color_subject>` (`docs/devel/COLOR_SWATCHES.md`). Severity drives
  `bind_style_if` outlines, and the caret and the "Pick" text bind flags. C++ only computes
  the model and publishes subjects: no `lv_xml_create` loop, no per-item
  `lv_obj_set_style_*`, no `lv_label_set_text`.
- `FilamentSlotPicker` becomes an XML component the same way: a heading subject, then
  `<repeat count="filament_picker_row_count">` rows (`filament_picker_row_${i}_label`,
  `_material`, `_reason`, `_color`, `_colors`, `_finish`, `_selected`, `_empty`), an Auto row
  and the extra and cancel rows bound by flags. Taps go through one `event_cb` carrying the
  row index. The mapping modal's use of it moves to the same component.
- Indexed subjects are registered up to a fixed cap (chips `kMaxFilamentChips`, picker rows
  `kMaxPickerRows`). Past the cap the "+N" pill stands in on the card, and the picker
  scrolls with the cap sized above any backend's lane count.
- The card's `DECLARATIVE_OK` sites and the picker's C++ widget construction go away. The
  `check_imperative_ui.py` count must fall.

## Settled after the builder check (Claude's calls, flagged for Preston)

- **Blocked remap.** The chip's caret binds to `color_card_remappable` and hides when remap is
  blocked. A chip tap goes through the same `current_remap_block()` branch the card tap uses
  today: `NeedsPlugin` opens the existing help modal, any other block does nothing, and `None`
  opens the picker. The header chevron keeps opening the full modal. Chips get their own click
  handler; the card-level tap remains for the header.
- **Picker rows.**
  - Auto first, as in the modal.
  - Then every slot, best first, ranked on `FilamentMapper`: exact (material and colour) >
    material match, colour differs > material differs > empty. Inside a tier, rows keep their
    physical order.
  - Reasons are "exact match", "color differs", "material differs" and "empty". A slot already
    mapped to another tool adds "also T2".
  - Last comes the extra row, "More options…", which opens the full modal.
  - There is no external-spool row and no "family match" tier (the mockup's were illustrative).
  - Two tools may share a slot, as the modal allows today.
- **Severity order.**
  - Red: no slot, an empty slot, or a material mismatch.
  - Amber: colour mismatch only, and only when the file's colour is known.
  - Grey: everything else, including Auto once it resolves to a slot.
  - One pure function decides it from `classify_mismatches` and the resolved slot.
- **Details row.**
  - Shows the materials of the used tools, deduplicated in tool order and joined by ", " ("PLA,
    PETG").
  - Until the used-tools set arrives it shows every material in the file, deduplicated.
  - Only the detail view changes; the list view's per-file material stays as it is.
- **Units.** Units are counted across all of `available_slots_` by (backend, unit). The chip's
  unit number is the unit's 1-based position in that combined display order, which is also the
  order the dropdown lists them in, so "2·1" always names the same lane as "Turtle 2 · Lane 1".

## Left to the builder

- Whether the chip stays the `filament_swatch` component (reworked) or becomes a new component.
- The ranking function's signature and the reason strings' exact words. The ranking must sit on
  `FilamentMapper`.
- How a dropdown pick applies (sharing the modal's apply path, not copying it).
- The single-extruder source of "loaded" when Spoolman is absent.
