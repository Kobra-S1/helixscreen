# Print Status redesign follow-ups (#1879) — implementation plan

Issue: prestonbrown/helixscreen#1879. Point-in-time scaffolding per
`docs/devel/CLAUDE.md`; delete this file in the change that ships the work.
This plan covers the issue body's three sections: (a) mode fixed per phase,
(b) the DRY consolidations, (c) the two bounded perf items, plus (d) the
mutation-survivor checklist in the issue's comment (§7), so the issue closes
with this branch.

Branch `feature/print-status-followups-1879`, worktree
`.worktrees/print-status-followups-1879`, based on main `1209ec86af`. Every
symbol named below was re-verified by grep against that base (the live
speed/flow chip and per-chip heights, `CalloutChipIn::h`, landed after the first
draft; the sections they touch are corrected below). Signatures are the
deliverable; bodies are left to the implementer. Each new header declaration
gets a `scripts/syntax_check.py` pass (borrowing a sibling's flags) before
execution starts — plans are interfaces, not code.

## What exists today (the map)

The print status printer tile and the home printer-image widget share one
layout engine:

- `include/callout_layout.h` — pure layout. `CalloutLayoutInput` carries a
  `budget` ("Worst-case chips this printer can ever show. Decides the MODE") and
  an `active` list ("Chips showing now. Only these get positions"). The mode is
  chosen by fitting the BUDGET (`try_line_modes`), then only ACTIVE chips are
  placed. `CalloutChipIn::expendable` controls the shed order when chips do not
  fit (`compute_callout_layout`), the pinned overflow (`slide_apart`,
  `dock_rank`), and `CalloutLayout::dropped`.
- `src/ui/callout_presenter.cpp#apply` — maps caller-measured parts into that
  input: `in.budget.push_back({c.kind, c.w_budget, ..., c.expendable, ...})`
  where `CalloutPresenter::Chip::expendable` is documented as
  "ChipContent::expendable" — the LIVE flag.
- `src/ui/ui_print_status_tiles.cpp#update_now` — the tile's wiring.
  `part.expendable = contents[i].expendable` copies the live flag into the
  budget part. The home widget (`src/ui/panel_widgets/printer_image_widget.cpp`
  `apply_callout_layout`) never sets `expendable` — only the tile is affected.
- `include/print_status_chips.h` — the pure per-chip decisions; `FanRole`
  (Part/Hotend/Aux) is local to it.

Label/mapping duplicates (all verified):

| Concept | Copy 1 | Copy 2 | Copy 3 |
|---|---|---|---|
| Phase short name | `src/print/print_start_phase_label.cpp#print_start_phase_step_label` ("Z tilt") | `src/print/print_start_collector.cpp#phase_signal_message` ("Z Tilt Adjust...") | `src/ui/ui_pre_print_options_renderer.cpp#label_key_for` ("Z-Tilt Adjust") |
| Option id → phase | — | `src/ui/ui_print_preparation_manager.cpp#preprint_phase_overrides` (`kTogglePhases`) | `label_key_for`'s id ladder |
| Operation → phase | `src/print/print_start_collector.cpp#phase_of_operation` | `src/print/start_block_steps.cpp#step_kind` + `#start_block_step_phase` | — |
| Fan spelling | `src/ui/print_status_chips.cpp#fan_chip` ("Part"/"HE"/"Aux") | `src/ui/ui_settings_fans.cpp#fan_type_label` ("Part"/"Heater"/...) | `src/printer/printer_fan_state.cpp` role names ("Hotend Fan") + `src/ui/panel_widgets/fan_stack_widget.cpp` name_map ("Hotend") |

---

## 1. Behavior: the layout mode is fixed per phase

### Today's bug

`src/ui/ui_print_status_tiles.cpp#update_now` feeds each budget chip the LIVE
`ChipContent::expendable` (`part.expendable = contents[i].expendable`). Those
flags flip with state: a fan at 0% is expendable, spinning it is not
(`print_status_chips.cpp#fan_chip`); speed/flow at 100% is expendable, a tune is
not (`#speed_flow_chip`); the light off is expendable, on is not
(`#light_chip`). So a fan spinning up, a tune, or the light can change which
chips the budget must fit, and with it the mode `compute_callout_layout` picks —
mid-print, the picture moves. This defeats the documented invariant on
`CalloutLayoutInput::budget`.

### Design options

**Option A — phase-static budget policy (recommended).**
The budget's `expendable` becomes a per-(chip, phase) constant: "may this chip's
room be shed in this phase". Live flags still drive the ACTIVE chips (a stopped
fan still dims and may drop when pinned), but never the mode. The mode is then a
pure function of (area, capability set, fan-name set, phase) and cannot change
inside a phase.

**Option B — freeze the budget vector at phase entry.**
Cache the built budget parts when `host_.printing()` flips (the panel already
fires `tiles_.request_update()` on that edge,
`src/ui/ui_panel_print_status.cpp` `on_print_start_phase_changed`) and reuse it
until the next flip. Same stability, but the frozen flags are whatever happened
to be live at entry, so behavior depends on entry state (a fan spinning at entry
pins its chip for the whole print even after it stops mattering). Rejected: an
accidental snapshot is a worse contract than a stated policy.

**Option C — split mode decision from placement in `callout_layout.h` and cache
the mode on (area, budget widths).**
The issue's perf bullet offers this as an alternative. It refactors a pure,
well-tested header and duplicates what Option A achieves at the input. Rejected
for this batch; Option A makes the mode deterministic, which is the same win.

**Where a phase-dropped chip goes when it becomes unusual (the decision the
issue asks for):** the docked row. The engine already has the fallback —
`callout_detail::place_docked` — used by `dock_free` when a loose chip fits
nowhere. Rule: a chip in `out.dropped` whose live content is shown and NOT live
expendable comes back, docked along the bottom edge, without relayout. An
unusual reading stays visible; the picture never moves. If the docked row would
overflow the area width, the existing dock_rank order sheds again (already the
engine's rule for pinned overflow).

### Interface

In `include/print_status_chips.h` (it already includes `callout_layout.h`, so
`CalloutKind` is visible; `PrintStatusChip` lives one layer up and must not
appear here):

```cpp
/// Whether the layout may shed this chip's room in this phase. This is the
/// BUDGET's expendable: a constant per phase, so the callout mode cannot
/// change mid-print. The active chip's expendable stays live.
/// Printing: the part fan is expected to run, so its room is never shed; the
/// hotend fan (automatic, redundant with the nozzle chip), the aux fan, the
/// light and speed/flow at 100% stay sheddable.
/// Preparing: fans are normally off, so all fan chips stay sheddable.
[[nodiscard]] bool phase_budget_expendable(CalloutKind kind, bool printing);
```

Table (the deliverable; `false` = never shed):

| CalloutKind | printing | preparing |
|---|---|---|
| Nozzle, Bed, Chamber | false | false |
| Fan (part) | false | true |
| HotendFan | true | true |
| AuxFan | true | true |
| Light | true | true |
| SpeedFlow | true | true |
| Toolhead | false | false |

In `include/callout_presenter.h`, `CalloutPresenter::Chip` gains a second flag
beside the live one:

```cpp
bool expendable = false;        ///< ChipContent::expendable: the live reading is an expected value
/// May the layout shed this chip's room in this phase. Decides the MODE, so it
/// must not follow live state; the tile sets it from phase_budget_expendable().
/// A surface whose budget is static anyway (home) leaves both flags false.
bool budget_expendable = false;
```

**Why two fields, and which pass reads which** (the first draft of this plan
fed the phase flag into the one `expendable` field; `apply` pushes that field
into BOTH `in.budget` and `in.active`, so the live flag would have been lost):

- First `compute_callout_layout` (decides the mode): `budget_expendable` for
  BOTH `in.budget` and `in.active`. The engine's shed loop
  (`compute_callout_layout`, the `gone` lambda) erases by one rule from both
  vectors; feeding them different flags would leave an active chip placed with
  no budgeted room, which can overlap.
- Pinned second pass (mode already fixed, budget cleared): the live
  `expendable`, so a stopped fan still yields first when pinned chips collide.
- Undock pass, last in `src/ui/callout_presenter.cpp#apply`, only when the
  final mode is NOT Pinned (Pinned's second pass already placed every active
  chip by live rules): for each kind in `out.dropped` whose part is `active &&
  !expendable`, erase it from `out.dropped` and place it with
  `callout_detail::dock_free(in, out.image, {chip_in}, out.chips)` — dock_free,
  not place_docked: it searches the strips the line mode left free and checks
  clearance against every placed chip, and itself falls back to place_docked
  when nothing is free. Comment states the constraint: the mode stays fixed per
  phase; an unusual reading docks into free space rather than relayouting.

### Callers to convert

- `src/ui/ui_print_status_tiles.cpp#update_now`: keeps `part.expendable =
  contents[i].expendable` (live) and adds
  `part.budget_expendable = phase_budget_expendable(kChips[i].kind, printing)`
  where `printing` is the same `host_.printing()` read `heater()` already uses.
- No change in `src/ui/panel_widgets/printer_image_widget.cpp` (it never sets
  `expendable`; the home budget is already phase-static).
- No change in `include/callout_layout.h` — the engine is correct; the input
  was not.

### Tests

Extend `tests/unit/test_print_status_redesign.cpp` `[print_status]` (fixtures
`Portrait320`/`Portrait272` and the `fans_part_spinning_aux_stopped` helper
already exist) and `tests/unit/test_print_status_chips.cpp`
`[print_status_chips]`:

1. `[print_status_chips]` "phase budget expendable table" — pin every row of the
   table above. Hand-break: flip one row (e.g. `Fan` under printing to `true`),
   rerun, see red; restore.
2. `[print_status]` "a fan spinning up mid-print does not change the mode" —
   Portrait320, part fan 0% → settle → record `ps_callout_mode` and the image
   rect (`lv_obj_get_x/y/width/height` of `ps_printer_image`); spin part fan to
   60% → settle; assert mode and rect unchanged. Hand-break: revert
   `part.expendable` to `contents[i].expendable` and watch this case go red on
   a width where the flags flip the fit (try 320 and 272; keep the one that
   flips).
3. `[print_status]` "a tuned speed does not change the mode" — same shape with
   `speed_pct` 100 → 104 (the `host_.speed_pct` seam the fixture already uses).
4. `[print_status]` "a phase-dropped chip that turns unusual docks" — smallest
   tile where AuxFan is in `out.dropped` in a line mode (assert
   `ps_chip_aux_fan_shown == 0` and the mode is not Pinned first, so the setup
   provably reached the branch), then spin the aux fan; assert
   `ps_chip_aux_fan_shown == 1`, mode and image rect unchanged, and its rect
   overlaps no other shown chip. Hand-break: delete the undock pass in
   `callout_presenter.cpp#apply`.
4b. `[callout_layout]`-level or presenter-level case: a chip that is
   `budget_expendable` but live-unusual is never placed without room — run a
   tight layout where the phase flag sheds it, assert it is in `dropped` before
   the undock pass (or docked after), never overlapping. Hand-break: feed
   `in.active` the live flag in the first pass.
5. Re-check the existing `Portrait272` "smallest tile keeps the unusual chips"
   and `[callout_layout]` "hotend fan drops first" cases: the shed ORDER is
   unchanged (dock_rank), but which chips are sheddable narrows during
   Printing, so a tight tile may now reach Pinned with more chips kept. Update
   expectations only after reading the new layout (`ctl geom` or the test's
   own rect dump, never a guess); the `if
   (ps_chip_aux_fan_shown == 1)` guards in the existing cases exist precisely
   because this edge is the engine's call.

---

## 2. DRY consolidations

### 2.1 One phase-label function, short and long

`include/print_start_phase_label.h` already owns the short name
(`print_start_phase_step_label`). It gains:

```cpp
/// Long banner text for a running phase, with its ellipsis
/// ("Z Tilt Adjust..."). "" for IDLE.
[[nodiscard]] const char* print_start_phase_signal_message(PrintStartPhase phase);

/// The phase a pre-print option id toggles, when it toggles one:
/// bed_mesh, qgl, z_tilt, nozzle_clean, purge_line. nullopt for every other
/// id (timelapse, ai_detect, ...).
[[nodiscard]] std::optional<PrintStartPhase> print_start_phase_of_option(
    std::string_view option_id);
```

Both implemented in `src/print/print_start_phase_label.cpp` (existing TU;
gains `#include <optional>`, `<string_view>` and `operation_patterns.h` is NOT
needed here). The long texts move verbatim from
`src/print/print_start_collector.cpp#phase_signal_message`, including the three
sub-step overrides in `apply_start_block_step` ("Homing axes...", "Probing
Z...", "Bed mesh...") — those REFINE a phase per step kind and stay in the
collector, with the constraint comment that they refine, not restate.

Callers to convert:

- `src/print/print_start_collector.cpp`: delete the anonymous-namespace
  `phase_signal_message` and its four call sites (`phase_signal_message(x)` →
  `print_start_phase_signal_message(x)`; the three `apply_start_block_step`
  sub-step overrides stay, as above).
- `src/ui/ui_pre_print_options_renderer.cpp#label_key_for`: the id ladder's
  five phase-backed ids route through `print_start_phase_of_option` +
  `print_start_phase_step_label` (qgl row now reads "Quad gantry level", z_tilt
  "Z tilt", nozzle_clean "Cleaning nozzle", purge_line "Purge"). The bed_mesh
  relabel stays: "Adaptive Bed Mesh" / "Auto Bed Mesh" are option variants, not
  the phase name — keep those two literals and the `adaptive_active` branch.
  Everything else (`label_key` from the DB, timelapse, humanize fallback) is
  untouched. This is the "nine new translation keys folded back": the option
  spellings ("Quad Gantry Level", "Z-Tilt Adjust", "Clean Nozzle", "Nozzle
  Priming") disappear from `ui_xml/translations/*.xml` when the reference scan
  stops naming them (`translation_sync obsolete` picks them up; do NOT hand-
  delete locale files).
- `src/ui/ui_print_preparation_manager.cpp#preprint_phase_overrides`: the
  `kTogglePhases` id→phase table becomes an id list iterated through
  `print_start_phase_of_option`.

Visible wording changes (intended, per the issue): the pre-print option rows
adopt the phase spellings. The purge toggle reads "Purge" instead of "Nozzle
Priming".

Tests:

- New `tests/unit/test_print_start_phase_label.cpp`, tag
  `[print_start_phase_label]`: short and long for every phase (short is
  prefix-of / consistent-with long: "Z tilt" vs "Z Tilt Adjust...");
  `print_start_phase_of_option` for the five ids plus timelapse and an unknown
  id (nullopt). Hand-break: swap the Z_TILT pair, rerun, red.
- `tests/unit/test_pre_print_options_renderer.cpp` (existing cases, tags as
  filed): update the qgl/z_tilt/nozzle_clean/purge_line expected keys to the
  phase spellings; keep a bed_mesh adaptive case. Hand-break: point
  `print_start_phase_of_option("qgl")` at the wrong phase, see the label case
  red.
- `tests/unit/test_print_start_collector.cpp`: the phase-message cases keep
  passing unchanged (texts moved verbatim); add one case asserting the
  collector's banner for each phase equals the literal English strings (so a
  fold-back typo cannot pass silently).

### 2.2 One fan-label helper, FanRole deleted

The spelling table moves to `FanType`'s home. In
`include/printer_fan_state.h`:

```cpp
/// One spelling of a fan everywhere it is printed.
enum class FanLabelLength {
    Chip, ///< fits a chip pill: "Part", "HE", "Aux", "Controller", "Temp", "Generic", "Pin"
    Word, ///< a row label word:    "Part", "Hotend", "Chamber", "Controller", "Temp", "Generic", "Output Pin"
    Full, ///< role/detail name:    "Part Fan", "Hotend Fan", "Chamber Fan", "Exhaust Fan", ...
};

/// The type's spelling at the given length. Role overrides (below) win over
/// these for a fan the wizard assigned a role.
[[nodiscard]] const char* fan_type_label(FanType type, FanLabelLength length);

// member of PrinterFanState:
/// The name this fan goes by on this printer: its configured role
/// (FanRoleConfig), else its type. `object_name` is the Moonraker object.
[[nodiscard]] std::string display_label(const std::string& object_name,
                                        FanLabelLength length) const;
```

Implementation lives in `src/printer/printer_fan_state.cpp`, beside
`role_display_names_`: the role table gains the Chip/Word spellings it already
lacks (it stores only Full today). `display_label` is the public widening of
the existing `PrinterFanState::resolve_display_name(object_name, type)` —
extend that helper (add the length parameter) rather than adding a second
resolver beside it, and keep the role-first resolution it already has.

`FanRole` is deleted from `include/print_status_chips.h`; the decision function
keeps its rules keyed by the chip kind it already receives everywhere:

```cpp
/// A fan chip. `kind` picks the rules (Part glows while spinning; HotendFan
/// at its automatic speed is expendable); `label` ("Part", "HE", "Aux")
/// prefixes the text when the chip has room.
[[nodiscard]] ChipContent fan_chip(CalloutKind kind, std::string_view label,
                                   bool present, int pct, bool automatic,
                                   bool labeled);
```

Callers to convert:

- `src/ui/print_status_chips.cpp#fan_chip`: the three `lv_tr` labels deleted.
- `src/ui/ui_print_status_tiles.cpp`: `fan()` and the `worst()` lambda resolve
  the label once per chip via `printer_state_.fan_state().display_label(name,
  FanLabelLength::Chip)` and pass it through; `FanRole` references gone.
- `src/ui/ui_settings_fans.cpp`: the local `fan_type_label` helper deleted;
  the type pill prints `fan_type_label(type, FanLabelLength::Chip)`. Visible
  change: the pill reads "HE" where it read "Heater" — that unification is the
  issue's point.
- `src/ui/panel_widgets/fan_stack_widget.cpp`: the `name_map` literals
  ("Part"/"Hotend"/"Chamber") and the carousel's `find(" Fan")`-erase hack are
  replaced by `display_label(object_name, FanLabelLength::Word)`; the single
  letters ("P"/"H"/"C") stay as the stack's own narrow form.
- `src/printer/printer_fan_state.cpp#init_fans`: the role name strings come
  from the shared table (no duplicate literals).

Tests:

- `tests/unit/test_print_status_chips.cpp` `[print_status_chips]`: the four
  `fan_chip` cases move to the new signature (labels passed in, e.g.
  `fan_chip(CalloutKind::HotendFan, "HE", ...)`). Add a case that a fan chip
  with an empty label omits the prefix.
- New cases in `tests/unit/test_printer_fan_char.cpp` (FanRoleConfig tests
  already live there), tag as filed: the full `fan_type_label` table at all
  three lengths, and `display_label` role-vs-type precedence (a configured
  aux fan named by a generic object reads "Aux"/"Aux Fan"). Hand-break: change
  `Chip` of HEATER_FAN to "H", see the table case red.
- `tests/unit/test_fan_settings_xml.cpp` / `tests/unit/test_widget_size_fan_stack.cpp`:
  update the literal expectations ("Heater" → "HE", row words via the helper).

### 2.3 One operation-to-phase mapping, and the BED_MESH / CHAMBER_SOAK verdicts

`include/print_start_phase_label.h` gains (it can include
`operation_patterns.h` without a cycle):

```cpp
/// The one OperationCategory -> PrintStartPhase mapping. nullopt for
/// categories that are not a pre-print phase of their own (BED_LEVEL,
/// SKEW_CORRECT, START_PRINT, UNKNOWN).
[[nodiscard]] std::optional<PrintStartPhase> phase_of_category(OperationCategory c);
```

Callers to convert:

- `src/print/print_start_collector.cpp`: the anonymous-namespace
  `phase_of_operation` is deleted; `set_macro_analysis` calls
  `phase_of_category`.
- `src/print/start_block_steps.cpp#step_kind`: the category switch collapses to
  the shared mapping for the 1:1 kinds; two local predicates remain and are
  commented as step-scan predicates, not phase decisions:
  - `command_is` / the G28 axis parse (`homing_kind`) — decides WHICH homing
    step kind (HomeAxes vs ProbeZ), finer than a phase;
  - `calibrates_mesh` — see the resolution below.

**BED_MESH resolution — not a disagreement; both behaviors are correct and
stay.** The two functions answer different questions. `phase_of_operation`
feeds `macro_expected_phases_` (the checklist's expected order): a start macro
that only LOADS a mesh still enters the BED_MESH phase — pinned by
`tests/unit/test_print_start_collector.cpp` "bed mesh phase detection"
(`default_phase("BED_MESH_PROFILE LOAD=default") == BED_MESH`,
`default_phase("BED_MESH_CLEAR") == IDLE`) — so expecting the row is right.
`step_kind` feeds the file-position follower, where a load/clear line runs no
work on its line — pinned by `tests/unit/test_start_block_steps.cpp` ("start
block: what a line runs decides its step": `kinds("BED_MESH_PROFILE
LOAD=default\nBED_MESH_CLEAR\n")` is empty). After consolidation the
category→phase row is written once; `calibrates_mesh` stays in
`start_block_steps.cpp` with the constraint comment: "a mesh line that only
loads, clears or offsets runs no work on its line, so it is not a step; the
phase mapping is shared, this filter is the step-scan's own".

**CHAMBER_SOAK resolution — keep both current behaviors, shared table maps it
to SOAKING, step scan keeps excluding it.** Evidence, not preference: the
category's keywords (`include/operation_patterns.h` `kOperationKeywords`)
include `SET_HEATER_TEMPERATURE HEATER=chamber` — a line that returns at once,
so the follower cannot treat the category's lines as runs-on-its-line work;
and SOAKING is already owned by the collector's temperature/wait detection
(`src/print/print_start_collector.cpp` soak entry points), which would race a
position-driven soak step. `step_kind` keeps no CHAMBER_SOAK case (with the
shared table returning SOAKING, the exclusion becomes an explicit early
return with that comment). Known accepted gap, stated in the code comment and
here: with a start block present, `checklist_input`'s `expected_order` omits
an upcoming soak row until the temperature detection enters SOAKING. If
Preston wants that row earlier, that is a separate change (a
`StartBlockStepKind::Soak` + `runs_on_its_line` semantics) — do not fold it
into this consolidation.

Tests:

- New `[print_start_phase_label]` case: `phase_of_category` for all 11
  categories (the four nullopts included). Hand-break: flip BED_LEVEL to a
  phase, red.
- `tests/unit/test_start_block_steps.cpp` `[start_block]`: unchanged
  expectations (the two predicates keep their pins); add one case that a
  HEAT_SOAK line in a start block yields no step, naming the exclusion.
- `tests/unit/test_print_start_collector.cpp`: `set_macro_analysis` cases
  unchanged (mapping moved verbatim).

### 2.4 Smaller copies

**Toolhead text.** One join, two builders
(`src/ui/panel_widgets/printer_image_widget.cpp#update_callouts`,
`src/ui/ui_print_status_tiles.cpp#update_now`). In
`src/ui/panel_widgets/callout_chip.h` (both call sites already include it):

```cpp
/// The merged toolhead chip's text: the nozzle reading and the part-fan
/// percentage, two spaces apart.
[[nodiscard]] std::string toolhead_chip_text(std::string_view nozzle_text,
                                             std::string_view fan_pct);
```

Test: one `[print_status_chips]`-adjacent case asserting the exact join
("220°  45%"); hand-break: single space. (Barely above the inline bar; it is
on the issue's list, so it is extracted.)

**Tool-badge read.** `PrintStatusTiles` reads the badge pair twice (`heater()`
Nozzle case and `update_now`'s `worst()`). Extract a private
`std::string PrintStatusTiles::tool_badge() const;` used by both. The render
difference (home draws a glyph beside the nozzle chip,
`printer_image_widget.cpp` badge-width measurement in `apply_callout_layout`;
the tile prefixes the text, `heater_budget_text(badge)`) is a design choice on
two different surfaces and stays — unifying it is not in this issue.

**8 + 8 XML blocks.** Two new components, following `activity_chip.xml`'s
prop pattern (component instances pass `name`, which lands on the component
root, so every `lv_obj_find_by_name` lookup in `ui_print_status_tiles.cpp`
keeps working unchanged):

- `ui_xml/components/region_glow.xml` — props `tone_subject`, `shown_subject`,
  `color` (string, used as `style_bg_color="$color"` — the interpolation is
  established in `divider_horizontal.xml`); view is the radius-32767 lv_obj
  with the local `glow_soft`/`glow_strong` styles and the two binds.
- `ui_xml/components/chip_leader_line.xml` — props `line_subject`,
  `shown_subject`; view is the `leader_line` with `styles.leader_dotted` and
  the one `bind_flag_if` (the `ps_callout_mode eq 0 or ... eq 2` literals live
  in the component now).

`ui_xml/components/print_status_printer_tile.xml` replaces the 16 blocks with
16 one-line instances (glow color per group: `#danger` heaters, `#info` fans,
`#warning` light + speed_flow). No C++ change. Verified feasible: color props
interpolate into style attrs (`ui_xml/components/exclude_object_row.xml`), and
`activity_chip` instances already carry `name` + subject props.

Tests: existing `[print_status]` glow/line cases ("phase switch relayouts and
clears pre-print glows") plus land-gate's `lint-xml`/schema checks. Hand-break:
point one instance's `shown_subject` at the wrong subject, see the glow case
red. One screenshot of the tile at 800x480 and the smallest breakpoint at the
finish (changed screen).

**`strip_shows_up_next` restating the strip XML.** The rule moves wholly into
the XML as a derived subject, and the C++ reads it:
in `ui_xml/components/print_status_strip.xml` (top level, beside the existing
subjects):

```xml
<subject_expr name="ps_up_next_visible"
              expr="job_queue_count gt 0 and (ps_strip_stacked eq 1 or ui_breakpoint gt 0)"/>
```

`up_next_row`'s two `bind_flag_if` binds collapse to one
(`subject="ps_up_next_visible" flag="hidden" ref_value="0"` inverted as the
engine requires — implementer checks the polarity against the existing
`bind_flag_if` forms). `src/ui/print_status_layout_fitter.cpp`'s
`strip_shows_up_next` becomes a subject read
(`lv_xml_get_subject(nullptr, "ps_up_next_visible")`, the same lookup pattern
it already uses for `job_queue_count`); no local re-computation remains. The
strip XML's header comment ("change both together") is deleted — there is now
one rule. `subject_expr` is established (`ui_xml/motion_panel.xml`,
`temp_graph_overlay.xml`).

**`kProgressBarHeight` restating `progress_bar.xml`.** Add a token where
`ps_strip_button` is defined (`ui_xml/globals.xml`, regenerated by
`scripts/gen_theme_tokens.py` into `src/generated/theme_token_table.cpp`):
`progress_bar_height` = 12 (non-responsive, like the XML literal it replaces).
`ui_xml/components/progress_bar.xml` reads `height="#progress_bar_height"`;
`src/ui/print_status_layout_fitter.cpp` reads
`theme_manager_get_spacing("progress_bar_height")` and the constexpr is
deleted. `make check-theme-tokens` covers the pair.

Tests for both: `tests/unit/test_print_status_redesign.cpp` already exercises
`PrintStatusLayoutFitter::strip_row_height` (the "strip buttons are square
beside the progress" case) — add a case that flips `job_queue_count`/`ui_breakpoint` subjects and
sees the estimated row count change through the subject (so a future XML edit
that breaks the expr reddens the height). Hand-break: remove the
`ui_breakpoint` clause from the expr, see the new case red.

---

## 3. Performance

### 3.1 Exact-size picture in the print status tile

Today the tile's `ps_printer_image` is `bind_src` + `inner_align="contain"`
(`ui_xml/components/print_status_printer_tile.xml`), so every redraw scales the
full-resolution decode, while the home widget shows a 1:1 cached copy
(`src/ui/panel_widgets/printer_image_widget.cpp#try_set_exact_size_source`,
`#check_or_generate_cache`; cache plumbing in
`src/system/prerendered_images.cpp` + `include/prerendered_images.h`). The
issue records the CC1 A/B: no RAM cost. Extract the machinery — do not fork it:

New `src/ui/panel_widgets/exact_size_image.h` (+ `.cpp`):

```cpp
namespace helix::ui {

/// An exact-size cached copy of the printer picture, so no redraw scales the
/// full-resolution decode. One instance per image surface (the home widget,
/// the print status printer tile). Cache entries are keyed (source, w, h) by
/// prerendered_images and pruned there.
class ExactSizeImage {
  public:
    /// Point `img` at an exact-size copy of `source` for the size the layout
    /// just gave it, scheduling an off-thread cache generation on a miss (the
    /// scaled source stays until it lands — nothing blanks). True when the
    /// exact copy is on screen.
    bool apply(lv_obj_t* img, const std::string& source);
    /// Forget the in-flight generation (owner teardown / tree destroyed).
    void cancel();
};

} // namespace helix::ui
```

`apply()` folds in today's `declared_image_size`, `show_exact_copy` (align
CENTER, scale NONE, then src — the order the comment there pins), the
`get_cached_printer_image_path` hit check, and the miss path's
`HttpExecutor::fast()` generation with the in-flight guard. The completion
keeps routing back to the main thread exactly as the home widget does today
(threading rule 1: no LVGL off the main thread; the class is main-thread only
and documented so).

Callers to convert:

- `src/ui/panel_widgets/printer_image_widget.cpp`: `try_set_exact_size_source`
  and `check_or_generate_cache` become thin calls on a member
  `ExactSizeImage`; `current_displayed_path_`/`cache_job_inflight_` move in.
- `src/ui/ui_print_status_tiles.cpp#update_now`: after `presenter_.apply`,
  when `parts.image` is valid, call `exact_image_.apply(image_obj,
  image.path)` and publish the path it chose through the existing
  `ps_printer_image_src` subject (`set_string(&image_src_, ...)`) so the XML
  binding stays the single src route; on a miss the subject keeps the source
  path and CONTAIN does the work until the generation lands. Annotate the
  imperative swap `// DECLARATIVE_OK: cached asset choice from measured size`.
  The tile calls it only when `presenter_.image_changed()` OR the source path
  changed, so a reading tick does not re-stat.

Constraint worth stating: after item 1 the image rect is fixed per phase, so
the cache converges to one entry per (phase layout, image) instead of one per
transient size — that is why 3.1 lands after 1.

Tests: `tests/unit/test_printer_image_widget.cpp` (existing cache cases) keep
passing against the extracted class. New `[print_status]` case "the printer
tile shows the exact-size copy when the cache holds one": seed the cache the
way `tests/unit/test_prerendered_images.cpp` does, run one `update_now`,
assert `lv_image_get_src(ps_printer_image)` is the cache path; assert a second
`update_now` does not change it. Hand-break: skip the `apply` call, red.

### 3.2 `dock_free` steps by gap

`include/callout_layout.h` `dock_free` scans candidate y (then x) one pixel at
a time, and `try_line_modes_at` runs it up to twice per attempt across the
shrink ladder. Change the two loops to step by `in.gap`:

```cpp
const int h = chip_h_of(in, c);   // chips carry their own height now
for (int y = in.gap; !spot && y + h <= in.area_h - in.gap; y += in.gap)
...
for (int x = in.area_w - in.gap - c.w; !spot && x >= in.gap; x -= in.gap)
```

The bottom/top strips keep their exact edge candidates (`in.area_h - in.gap -
h`, `in.gap`), so a tall chip still hugs the bottom edge even when `h` is not a
multiple of the gap; only the free-axis scan is coarsened.

with the constraint comment: "docked chips sit gap-aligned — every other
placement (bands, stacks, the docked row) puts chip edges on the gap grid, and
a chip must clear its neighbors by a full gap, so a free spot off the grid
cannot host one that a grid spot does not." This drops the scan ~gap× (gap is
`space_xs`); the alternative the issue offers — caching the mode decision on
(area, budget widths) — is delivered by item 1's phase-static budget, so it is
not also done here.

Tests: `tests/unit/test_callout_layout.cpp` `[callout_layout]` new case
"loose chips dock on the gap grid": an area with a tagged image and an
obstacle chip occupying `y in [gap, 2*gap)`, so the first free y for the loose
chip is `2*gap`; assert every docked side-strip rect has `y % gap == 0` and no
two overlap. Add one tall chip (`h = 2 * chip_h + 3`) to the same case so the
non-multiple height is covered. Hand-break: revert the step to `++y`/`--x` and watch the modulo
assertion go red on that construction; restore.

---

## 4. Product call — NEEDS-PRESTON: chamber visibility capability

The home printer-image callout gates its chamber chip on
`Capability::HasChamberHeater`
(`src/ui/panel_widgets/printer_image_widget.cpp#apply_callout_layout` and
`#update_callouts`); the print status tile gates on `Capability::HasChamber`
(sensor OR heater) — both the budget (`in_budget`) and the chip's `present`
(`src/ui/ui_print_status_tiles.cpp`). One capability must own chamber
visibility.

**NEEDS-PRESTON, one line:** should a chamber with a SENSOR but no heater show
a chamber chip on the home printer image (adopt `HasChamber` everywhere — a
passive reading the temp graph can still plot, at the cost of a wider budget
and possibly another callout mode on sensor-only printers), or should Print
Status hide its chamber chip without a heater (adopt `HasChamberHeater`
everywhere — the tile loses a live reading on those printers)?

Everything else in this plan proceeds regardless; whichever way this lands it
is a one-line constant change in each of the two files above plus test
updates. Do NOT unify it as a drive-by in the DRY commits.

## 5. Implementation order

1. **2.3 `phase_of_category`** — pure, small, no UI; unblocks nothing but is
   the lowest-risk start and the BED_MESH/CHAMBER_SOAK verdicts land with it.
2. **2.1 phase labels + option mapping** — pure plus renderer wording; the
   translation-key fold-back rides the same commit so the reference scan never
   sees both spellings.
3. **2.2 fan labels + FanRole deletion** — touches four UI files; its own
   commit so the visible "Heater"→"HE" pill change is bisectable.
4. **Item 1: phase-static budget + dock-the-unusual** — the behavior change,
   isolated after the label work so its diff is only the mode logic.
5. **2.4 smaller copies** — toolhead text, badge read, the two XML components,
   the strip subject_expr, the progress-bar token. Splittable across two
   commits (C++ pair, XML pair) if review wants it.
6. **3.1 ExactSizeImage** — after item 1, so the image rect is stable and the
   cache converges.
7. **3.2 dock_free gap step** — any time; done with item 1's branch to share
   the `[callout_layout]` test pass.

Each step: `make t F='[its tags]'` once after the change, and every new test
is hand-broken once as written (the break named above per case). At the batch
finish: one review, one
`make mutate-diff MUTATE_ARGS="--tests '[print_status],[print_status_chips],[callout_layout],[print_start_phase_label],[start_block]'"`,
one `make land-gate`, one screenshot per changed screen (print status tile at
800x480 and the smallest breakpoint; fan settings pill; pre-print options
rows). Translated-string removal goes through the sync step of the adding-
translated-strings procedure (obsolete keys drop from the locale files via the
reference scan; CJK fonts are unaffected — no new glyphs).

## 6. Files touched (summary)

| File | Change |
|---|---|
| `include/print_status_chips.h/.cpp` | `phase_budget_expendable`; `fan_chip` re-keyed on CalloutKind + label; FanRole deleted |
| `include/callout_presenter.h`, `src/ui/callout_presenter.cpp` | undock pass at end of `apply` |
| `src/ui/ui_print_status_tiles.cpp`, `include/ui_print_status_tiles.h` | budget policy call, label resolution, `tool_badge()`, `ExactSizeImage` member |
| `src/ui/panel_widgets/printer_image_widget.cpp/.h` | exact-size machinery extracted |
| `src/ui/panel_widgets/exact_size_image.h/.cpp` | NEW — shared exact-size copy |
| `src/ui/panel_widgets/callout_chip.h` | `toolhead_chip_text` |
| `include/printer_fan_state.h`, `src/printer/printer_fan_state.cpp` | `FanLabelLength`, `fan_type_label`, `display_label` |
| `src/ui/ui_settings_fans.cpp`, `src/ui/panel_widgets/fan_stack_widget.cpp` | shared labels |
| `include/print_start_phase_label.h`, `src/print/print_start_phase_label.cpp` | `print_start_phase_signal_message`, `print_start_phase_of_option`, `phase_of_category` |
| `src/print/print_start_collector.cpp` | local label/mapping twins deleted |
| `src/print/start_block_steps.cpp` | shared mapping; two predicates kept + commented |
| `src/ui/ui_pre_print_options_renderer.cpp`, `src/ui/ui_print_preparation_manager.cpp` | phase-backed labels; id list |
| `ui_xml/components/print_status_printer_tile.xml` | 16 blocks → 16 component instances |
| `ui_xml/components/region_glow.xml`, `chip_leader_line.xml` | NEW |
| `ui_xml/components/print_status_strip.xml`, `ui_xml/components/progress_bar.xml`, `ui_xml/globals.xml` | `ps_up_next_visible` expr; `#progress_bar_height` token |
| `src/ui/print_status_layout_fitter.cpp` | subject read; token read; kProgressBarHeight deleted |
| `include/callout_layout.h` | dock_free gap step |
| `ui_xml/translations/*.xml` | option spellings dropped by the reference scan |
| tests: `test_print_status_redesign.cpp`, `test_print_status_chips.cpp`, `test_callout_layout.cpp`, new `test_print_start_phase_label.cpp`, `test_pre_print_options_renderer.cpp`, `test_start_block_steps.cpp`, `test_printer_fan_char.cpp`, `test_fan_settings_xml.cpp`, `test_widget_size_fan_stack.cpp`, `test_printer_image_widget.cpp` | per section above |

## 7. Mutation-survivor checklist (issue comment)

The post-landing mutation run named four code places whose survivors are real
gaps (the translation, XML and deleted-code survivors are explained there and
need nothing). Line numbers in the comment are stale; find each by content.

| Place | What a test must pin | Tag |
|---|---|---|
| `src/ui/ui_print_preparation_manager.cpp`, the macro-analysis caching that feeds the pre-print checklist (three hunks) | a second checklist build for the same file reuses the cached analysis (no second scan) and a different file invalidates it | the file's existing tag |
| `src/print/print_start_collector.cpp`, clearing the start-echo lines | lines from print A are gone when print B arms | `[print_start_collector]` or as filed |
| `src/ui/ui_panel_print_status.cpp`, the early returns and the full-refresh call (four hunks) | each early return's guard: the refresh does NOT run in its excluded state; the full refresh DOES repaint every tile | `[print_status]` |
| `src/ui/ui_leader_line.cpp`, the dashed-draw hook | the hook is installed on a created leader line (draw-event callback count or a dashed pixel in a rendered snapshot) | `[leader_line]` |

Each new case is hand-broken once (delete the pinned line, see red, restore).
If a hunk turns out to be dead code rather than an untested branch, delete it
instead of testing it, and say so in the commit body.

## 8. Execution: three streams, one test plan

Streams run in parallel on sub-branches of this branch, each in its own
worktree, merged back here in the order A, B, C:

| Stream | Sections | Owns (exclusive) | Touches shared |
|---|---|---|---|
| A — labels | 2.3, 2.1, 2.2 | `print_start_phase_label.*`, `print_start_collector.cpp`, `start_block_steps.cpp`, `ui_pre_print_options_renderer.cpp`, `ui_print_preparation_manager.cpp#preprint_phase_overrides`, `printer_fan_state.*`, `ui_settings_fans.cpp`, `fan_stack_widget.cpp`, translations | `print_status_chips.h/.cpp` (`fan_chip`, FanRole), `ui_print_status_tiles.cpp` `fan()`/`worst()` only |
| B — layout | 1, 3.2, 2.4, 3.1, §4 | `callout_layout.h`, `callout_presenter.*`, `callout_chip.h`, `exact_size_image.*`, `printer_image_widget.*`, `print_status_printer_tile.xml`, new XML components, `print_status_strip.xml`, `progress_bar.xml`, `globals.xml`, `print_status_layout_fitter.cpp` | `print_status_chips.h/.cpp` (`phase_budget_expendable` only), `ui_print_status_tiles.cpp` `update_now`/`heater()` |
| C — survivors | §7 | the four test files and, only if a hunk is dead, its source line | — |

A and B both edit `print_status_chips.h/.cpp` and `ui_print_status_tiles.cpp`
in disjoint functions; the orchestrator resolves the merge.

**Test plan (orchestrator-owned; agents do not deviate):** while building, each
agent runs only `make t F='[its tags]'` once per change and hand-breaks each new
test once. Agents never run `mutate-diff`, `unit-sweep`, `full-test-run`,
`land-gate`, screenshots or their own review. At the finish the orchestrator
runs, once over the merged branch: one review; `make mutate-diff
MUTATE_ARGS="--tests '[print_status],[print_status_chips],[callout_layout],[print_start_phase_label],[start_block],[print_start_collector],[leader_line],[fan]'"`
(the touched tags, adjusted to what the streams actually tagged); one
screenshot per changed screen; ONE `make land-gate` covering this branch and
`fix/helixprint-1885` together.
