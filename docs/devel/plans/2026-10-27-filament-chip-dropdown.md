# Filament Chip Dropdown Implementation Plan

> **For agentic workers:** execute task by task in `.worktrees/filament-chip-dropdown`
> (branch `feature/filament-chip-dropdown`). The repo's Verification Cadence overrides the
> superpowers execution skills:
> - per task, run only `gmake t F='<the task's tags>'` (gmake on the Mac);
> - prove each new test red once by breaking the line it pins;
> - commit each task.
>
> Do not run sweeps, gates, mutation or reviews; the orchestrator runs the finish once. Run
> the app headless only (`HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`, `--test`, pinned socket).

**Goal:** Replace the FILAMENTS card's 40px two-band chips and header lamps with Bambu-style
chips that open a ranked slot dropdown in place. Build the chips and the dropdown purely
declaratively. Give single-extruder printers a loaded-filament chip, and make the details row
show the materials the print actually uses.

**Architecture:**
- A pure model (`include/filament_chip_model.h`) answers severity, labels, ranking, reasons and
  the details text on top of `FilamentMapper`.
- `FilamentMapper::pick_slot()` is the one way a pick is recorded.
- The card and `FilamentSlotPicker` only publish indexed subjects. XML `<repeat>` components
  bind them, with colours through `swatch_paint` / `color_dot`.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml (`<repeat count=subject>`, `${i}` subjects), Catch2.

**Spec:** `docs/devel/plans/2026-10-27-filament-chip-dropdown-design.md`. Read it first,
including "Declarative" and "Settled after the builder check".

**Contracts in the tree (syntax-checked):**
- `include/filament_chip_model.h`;
- `FilamentMapper::pick_slot` in `include/filament_mapper.h`;
- `FilamentSlotPicker::Options::{order, reasons, offer_auto}` in `include/ui_filament_slot_picker.h`.

The declarative task may replace those `Options` fields with subject-driven ones. If it does,
say so in its commit body.

## Global Constraints

- **Declarative rules:** `.claude/rules/declarative-ui.md`. This branch must LOWER
  `scripts/check_imperative_ui.py --summary`; record the before and after counts in the last
  task's commit body.
- **Swatch colours** go through the swatch painter only (`docs/devel/COLOR_SWATCHES.md`). There
  is one colour-swatch language app-wide.
- **Labels** go through `include/display_numbering.h` only. G-code tools are always
  `tool_label()`. A chip never prints a bare number on a multi-unit printer.
- **No vendor names.** The noun comes from `AmsBackend::lane_noun()`.
- **Firmware rules:** these files are in the ESP32 firmware
  (`firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`), so they must be
  exception-free; add any new `src/*.cpp` there; large statics use `HELIX_PSRAM_BSS`.
- **Size:** the firmware is over its size budget. Keep the new code small, and report the
  `.text` delta of the touched objects (`size build/obj/...`) in the final commit.
- **Strings:** US English, translated in every locale (`scripts/translation_sync.py sync`,
  then fill each locale following `translations/GLOSSARY.md`, then
  `scripts/generate_translations.py`), and no em dashes.
- **Values:**
  - chips are 64px wide, four to a row at 800x480 (card content 274x69);
  - `kMaxFilamentChips = 16`, `kMaxPickerRows = 32`;
  - the reasons read "exact match", "colour differs", "material differs", "empty" and ", also T2";
  - the single-extruder body reads "Loaded", "Swap" or "?";
  - an unmapped chip reads "Pick".

## Review Focus

1. **A blocked remap backend, tapped.** The chip must not open the picker. `NeedsPlugin` opens
   the help modal, as the card does today. Pinned in Task 4.
2. **A pick that becomes stale.** A slot is picked, then `refresh_slot_data()` replaces the slots
   (a spool swap). The chip's severity and colours must follow the new slot data, not a
   cached verdict. Pinned in Task 3.
3. **More tools than the subject cap, or more slots than the row cap.** No out-of-range subject
   may be written. The "+N" pill and a scrolling picker take over. Pinned in Tasks 3 and 2.
4. **Two units in two backends**, both with `unit_index` 0. The chip labels must still differ
   ("1·1" and "2·1"). Pinned in Task 1.
5. **The language changes while the card is open.** The reasons and "Pick" must re-translate
   (`observe_language_change`). Pinned in Task 3.

---

### Task 1: Pure chip model and the shared pick

**Files:**
- Create `src/printer/filament_chip_model.cpp` (add it to `app_srcs.txt`).
- Modify `src/printer/filament_mapper.cpp` (`pick_slot`) and
  `src/ui/ui_filament_mapping_modal.cpp`: `on_slot_selected` calls `pick_slot`, and its own
  copy of the logic goes.
- Tests: `tests/unit/test_filament_chip_model.cpp` (new), plus a case in the mapper tests.

**Tests `[filament_chip]`:**

`chip_severity`:
- a null slot gives Block;
- an empty slot gives Block;
- PLA vs PETG gives Block;
- same material, colour distance above the tolerance, colour known: Warn;
- the same with colour not known: Ok;
- exact: Ok;
- Auto resolved to a matching slot: Ok.

`chip_slot_label`:
- one unit: "3";
- two units in one backend: "2·1";
- two backends each with `unit_index` 0: "1·1" and "2·1".

`rank_slots_for_tool`:
- tiers come in order and keep their input order;
- an unknown file colour never demotes a slot;
- `also_tools` lists other tools mapped to the slot and leaves out self.

`slot_fit_text`: "material differs, also T2".

`used_materials_text`:
- `{"ABS","ABS","ABS","ABS","ABS-GF"}` used `{4}` gives "ABS-GF";
- used `{0,4}` gives "ABS, ABS-GF";
- nullopt gives "ABS, ABS-GF";
- an empty list gives "".

`loaded_severity`:
- unknown gives Ok;
- a Swap gives Ok;
- a Loaded tool with the material differing gives Block.

`pick_slot` (`[filament_mapper]`):
- marks `user_picked`;
- Auto clears the warnings;
- a lane no longer present clears the warnings;
- a material mismatch sets `material_mismatch`.

**Commit:** `feat(filament): chip severity, labels and slot ranking as one pure model`.

### Task 2: Declarative slot picker

**Files:**
- `src/ui/ui_filament_slot_picker.cpp` and its header;
- a new `ui_xml/components/filament_slot_picker.xml` and a row component;
- the subjects for the heading, `filament_picker_row_count` and the per-row indexed subjects
  (registered up to `kMaxPickerRows`).

**Behaviour:**
- Rows bind `<color_dot>` (with colours and finish), the label, the material and the reason,
  and are highlighted by `bind_style_if` on `_selected`.
- An Auto row shows when `offer_auto` is set. The extra and cancel rows are bound by flags.
- Taps go through one `event_cb` carrying the row index.
- The positioning and anchoring logic stays (measured layout is allowed in C++), and the
  outline on the trigger stays.
- The mapping modal keeps working through the same component.

**Tests `[slot_picker]`:**
- the picker shows N rows for N slots;
- the rows follow the `order` given, each with its reason;
- a tap on row k returns Selection k;
- Auto returns `is_auto`;
- the extra row returns `is_extra`;
- with more rows than the cap, no subject past the cap is written and the list scrolls;
- the modal's row picker still maps a tool (the existing modal tests stay green).

**Commit:** `refactor(filament): the slot picker is an XML component bound to subjects`.

### Task 3: Declarative chips and the outline signal

**Files:**
- `ui_xml/components/filament_swatch.xml`, reworked into the 64px chip;
- `ui_xml/print_file_detail.xml`: the chips go in a dedicated container with
  `<repeat count="filament_chip_count">`, and the header lamps `filament_mismatch_icon` and
  `empty_tools_warning_icon` are removed;
- `src/ui/ui_filament_mapping_card.cpp` and its header: `rebuild_compact_view` publishes the
  indexed subjects from `filament_chip_model` and creates no widgets;
- the "+N" pill (`filament_mapping_more_pill.xml`) binds a count subject.

**Behaviour:**
- Severity comes from `chip_severity` at publish time, against the current slots.
- The chip's spool takes the slot's colour, colours and finish; the header takes the file's
  colour, or no fill when it is unknown.
- The slot label comes from `chip_slot_label`, and "Pick" shows when no slot is resolved.
- The caret binds `color_card_remappable`.
- Keep the subjects the print-start path reads (`filament_mismatch`, `empty_tools_warning`);
  only their icons go.

**Tests `[filament_mapping]`:**
- the subjects for a three-tool file;
- severity 2 for an empty slot;
- after `refresh_slot_data()` changes the slot's colour, severity and colour follow (Review
  Focus 2);
- 20 tools shows the "+N" pill and writes no subject past the cap;
- a language change re-translates "Pick";
- the header icons are gone;
- `check_imperative_ui` falls.

**Commit:** `feat(filament): Bambu-style chips bound to subjects, outline carries the warning`.

### Task 4: Chip tap opens the picker

**Files:** `ui_filament_mapping_card.cpp`, `src/ui/ui_print_select_detail_view.cpp` and the chip
XML (`event_cb` with `user_data="$i"`).

**Behaviour:**
- A chip tap runs the same `current_remap_block()` branch as the card:
  - `None` opens the picker anchored under that chip;
  - `NeedsPlugin` opens the help modal;
  - any other block does nothing.
- The picker's heading is "T4 · ABS-GF" (`tool_label` + material). Its rows are ranked by
  `rank_slots_for_tool` with `slot_fit_text`, it offers Auto, and its extra row is "More
  options…".
- A pick goes through `FilamentMapper::pick_slot`, then `set_mappings`, then the existing
  `on_mappings_changed` callback, so the detail view hears about it exactly as it does from
  the modal.
- "More options…" opens the full modal, as the header chevron does.

**Tests `[filament_mapping]`:**
- a tap with `RemapBlock::None` opens the picker (`is_visible`);
- with `NeedsPlugin` the help modal is on the stack and no picker opens;
- with `Probing` nothing happens;
- picking row 0 changes `get_mappings()` and fires the callback once;
- "More options…" opens the modal.

**Commit:** `feat(filament): change one tool's slot from its chip`.

### Task 5: No filament system: loaded and swap chips

**Files:** the card's visibility rule (`shows_mapping_for` / `recompute_visibility`), the card,
and the detail view.

**Behaviour:**
- With `!AmsState::is_available()` and a file that has a known material, the card shows.
- There is one chip per used tool, with no caret and no tap.
- `LoadedFilament` comes from `AmsState::get_external_spool_info()`.
- The bodies read "Loaded" or "Swap" (`LoadedRole`), or "?" when unknown, with severity from
  `loaded_severity`.
- The bypass dead-control rule is unchanged when a filament system exists.

**Tests `[filament_mapping]`:**
- a no-AMS printer with a one-tool file shows one Loaded chip;
- loaded PLA against file PETG gives severity 2;
- unknown loaded filament gives "?" and severity 0;
- a two-tool file gives Loaded and Swap;
- an AMS printer with bypass on and a one-lane print stays hidden.

**Commit:** `feat(filament): show the loaded filament against the file on a printer with no filament system`.

### Task 6: Details row uses the used tools

**Files:** `src/ui/ui_panel_print_select.cpp` (the `filament_types.front()` site) and the
detail view, where `selected_filament_type` re-publishes once the used tools arrive.

**Behaviour:** `used_materials_text` decides the text, the same source the chips use. The
list view stays as it is.

**Tests:**
- `ssr_heat_sink_orca`-shaped metadata (five materials, used `{4}`) publishes "ABS-GF";
- before the used tools arrive it publishes "ABS, ABS-GF".

**Commit:** `fix(print-select): the file's material is what its used tools print with`.

### Task 7: Docs

**User docs:** `docs/user/guide/printing.md`, the section on the print file screen and the
FILAMENTS card. Cover the chip anatomy, what the outline colours mean, a tap to change a slot,
"More options…", and single-extruder Loaded and Swap.

**Developer docs:**
- `docs/devel/FILAMENT_MANAGEMENT.md`, the mapping card section;
- `docs/devel/COLOR_SWATCHES.md`, where the chip is a consumer;
- `docs/devel/LVGL9_XML_GUIDE.md`, only if a new XML pattern was needed;
- a changelog draft entry.

Document what shipped, not the plan.

**Commit:** `docs: filament chips with a slot dropdown`.

### Finish (orchestrator only)

1. One review.
2. Scoped `mutate-diff`:
   `[filament_chip],[filament_mapper],[slot_picker],[filament_mapping],[print_select]`.
3. Screenshots of the chips in every severity, the dropdown, the single-extruder card and the
   modal picker.
4. ESP32 size number from helixscreen-ae.
5. One land-gate.
6. Delete both plan files in the landing change.
