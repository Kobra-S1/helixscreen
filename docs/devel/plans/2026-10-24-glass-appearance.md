# Glass Appearance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A hardware-gated "Glass Effects" appearance modifier that gives every piece of UI chrome depth and lighting, derived from whatever theme is active.

**Architecture:** Glass is drawn by rewriting LVGL draw tasks in place (`LV_EVENT_DRAW_TASK_ADDED`), not by styles: each fill's resolved color, local overrides included, becomes a gradient derived from that color, and each border becomes a rim derived from it. A widget's glass *family* (raised / surface / recessed / none) is read from which shared theme style it carries, or from an explicit marker. A gate (fingerprinted, probed once per board + version) decides whether the setting exists at all; only while glass is effectively on are the draw-task hooks installed.

**Tech Stack:** C++17, LVGL 9.5 (`LV_USE_DRAW_SW_COMPLEX_GRADIENTS 1`, `LV_GRADIENT_MAX_STOPS 8`), helix-xml, Catch2.

**Spec:** `docs/devel/plans/2026-10-24-glass-appearance-design.md` (mockups in `docs/devel/plans/glass/`).

## Global Constraints

- Glass only lightens or darkens a color toward white/black. Never a hue shift.
- Effective glass = gate passed && setting on. No `beta_features` gate, no BETA tag.
- Fails the gate without probing: `LV_COLOR_DEPTH < 24`, `!full_style_effects_allowed(tier)`, software-rotated display.
- Glass off (or gate failed) must render pixel-identical to today: no hook installed, styles configured exactly as now.
- Setting row: Settings → Appearance, directly under "Theme Colors", label "Glass Effects", description "Depth and lighting on buttons and panels", hidden via a `glass_available` subject.
- Filament color swatches and content (camera, thumbnails, gcode/3D previews, bed-mesh and graph plots, images) stay flat.
- Cards glow from their own top-left; glow radius is fixed in pixels (start: 420px at 800x480), never a percentage of the card.
- Button corner radius stays the theme's `button_radius` (Claude's call: it is already user-tunable).
- Live: setting toggle, theme change, dark/light, hot reload all re-derive glass without restart.
- Env override: `HELIX_GLASS=1` forces the gate to pass, `HELIX_GLASS=0` forces fail.
- LVGL work on the main thread only (`.claude/rules/threading.md`).
- Constants `kCamelCase`, spdlog only, SPDX headers, no exceptions/RTTI in shared code.

## Review Focus

1. A button whose bg color is set locally (LED color chips, color pickers): glass must shade *its* color, or leave it flat if marked exempt. Task 3 tests a locally colored button.
2. Toggling glass while an overlay/modal is open: existing widgets on every screen and layer gain/lose the hook. Task 5 tests a widget on `lv_layer_top()`.
3. Theme or dark/light change while glass is on: the hook survives and shades from the new palette, and styles keep glass shadows. Task 4 tests `set_dark_mode` with glass on.
4. A board whose stored probe result came from an older app version or a different board: it must re-probe, not trust the stale pass. Task 2 tests both.
5. Translucent fills (bg_opa < cover, e.g. `ButtonTransparent` pre-blend, overlays at 50%): leave them alone, do not turn them opaque. Task 3 tests opa 128.

## Test plan (one plan for every agent; orchestrator owns it)

While building, each task runs only `make t F='[glass]'` (plus `[theme]` for Task 4) once after its change, and proves each new test red once by breaking the line it pins. No `unit-sweep`, `full-test-run`, `land-gate`, mutation, ASAN or screenshots inside a task. The finish (Task 9) runs one review, one scoped `make mutate-diff`, screenshots, and one `make land-gate`.

---

### Task 1: Pure shading derivation

**Files:**
- Create: `include/glass_shading.h`, `src/ui/glass_shading.cpp`
- Test: `tests/unit/test_glass_shading.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace helix::ui {
  enum class GlassFamily : uint8_t { None, Raised, Surface, Recessed };
  struct GlassStops { int count; lv_color_t color[4]; uint8_t frac[4]; };  // frac 0..255
  constexpr int32_t kGlassGlowRadiusPx = 420;
  GlassStops glass_stops(GlassFamily family, lv_color_t base, bool dark, bool pressed);
  lv_color_t glass_rim_color(GlassFamily family, lv_color_t base, bool dark);
  }
  ```
  `glass_stops(None, ...)` returns `count == 0`.

Values (percent = share of white/black mixed into `base`; frac = position):

| Family | Mode | Stops |
|---|---|---|
| Raised | dark | white 45% @0, white 22% @23, white 6% @102, black 22% @255 |
| Raised | light | white 50% @0, white 22% @23, white 6% @102, black 18% @255 |
| Raised pressed | both | black 25% @0, black 5% @255 |
| Surface (radial from top-left) | dark | white 24% @0, base @160, black 15% @255 |
| Surface | light | base @0, black 3% @140, black 9% @255 |
| Recessed (vertical) | dark | black 30% @0, base @255 |
| Recessed | light | black 12% @0, base @255 |

Rim: Raised dark = white 40%, Raised light = black 22%, Surface dark = white 12%, Surface light = black 14%, Recessed = black 40% (dark) / black 20% (light).

- [ ] **Step 1: Write the failing tests** (`[glass]`)
  - `glass_stops: raised dark matches the spec table`: for base `0x3A7CC8`, each stop equals `lv_color_mix(white/black, base, pct*255/100)` and fracs are `{0,23,102,255}`.
  - `glass_stops: pressed raised darkens top to bottom`: 2 stops, stop[0] luminance < stop[1] luminance.
  - `glass_stops: no hue shift`: for each of `0x3A7CC8, 0xD94848, 0x5CB85C, 0xE8A83A, 0x7C6CC8` × every family × dark/light, every stop's HSV hue (`lv_color_to_hsv`) is within ±4° of base's (skip when base saturation < 10).
  - `glass_stops: none has no stops`: `count == 0`.
  - `glass_rim_color: lighter in dark, darker in light`: luminance comparisons for Raised.
- [ ] **Step 2:** `make t F='[glass]'` → FAIL (undefined symbols).
- [ ] **Step 3:** Implement in `src/ui/glass_shading.cpp` with `lv_color_mix`; no LVGL objects, no globals.
- [ ] **Step 4:** `make t F='[glass]'` → PASS. Break one table value, see red, restore.
- [ ] **Step 5:** Commit `feat(glass): palette-derived glass shading stops`.

### Task 2: The gate (decision, store, probe)

**Files:**
- Create: `include/glass_gate.h`, `src/ui/glass_gate.cpp`
- Modify: `include/screensaver_level_store.h`, `src/ui/screensaver_level_store.cpp` (or wherever `board_fingerprint` is defined), `src/ui/screensaver_manager.cpp#ScreensaverManager::board_facts`
- Test: `tests/unit/test_glass_gate.cpp`

**Interfaces:**
- Consumes: `helix::ui::BoardFacts`, `board_fingerprint()` (`include/screensaver_level_store.h`), `PlatformCapabilities`, `full_style_effects_allowed()`.
- Produces:
  ```cpp
  namespace helix::ui {
  // Moved out of ScreensaverManager so glass and savers fingerprint the same way.
  BoardFacts current_board_facts(const helix::PlatformCapabilities& caps,
                                 const std::string& display_backend);
  struct GlassProbeRecord { bool passed = false; double frame_ms = 0; std::string version, board; };
  struct GlassGateFacts {
      int color_depth = 0; bool style_effects_allowed = false; bool software_rotated = false;
      std::optional<bool> env_force;              // HELIX_GLASS
      std::optional<GlassProbeRecord> stored;
      std::string version, board;
  };
  enum class GlassGateVerdict { Pass, Fail, NeedsProbe };
  GlassGateVerdict decide_glass_gate(const GlassGateFacts& f);
  constexpr const char* kGlassProbeStorePath = "/display/glass_probe";
  std::optional<GlassProbeRecord> load_glass_probe(const helix::Config& c);
  void save_glass_probe(helix::Config& c, const GlassProbeRecord& r);
  constexpr double kGlassFrameBudgetMs = 6.0;
  double measure_glass_frame_ms(int32_t width, int32_t height);  // main thread, offscreen
  }
  ```
- `decide_glass_gate` order: env_force wins; then `color_depth < 24 || !style_effects_allowed || software_rotated` → Fail; then a stored record matching `version` and `board` → its `passed`; else NeedsProbe.
- `measure_glass_frame_ms`: draw into an offscreen `lv_canvas` layer one full-size Surface radial fill plus twelve 140x46 Raised vertical fills, 5 repetitions, return the median ms. Pass = `frame_ms <= kGlassFrameBudgetMs` (a third of a 60 Hz frame; tune from device numbers, keep the constant).

- [ ] **Step 1: Failing tests** (`[glass]`): env force on beats 16bpp; env force off beats a stored pass; 16bpp fails; disallowed effects fails; software-rotated fails; stored pass for same version+board → Pass; stored pass for other version → NeedsProbe; stored pass for other board → NeedsProbe; stored fail same version+board → Fail; `save_glass_probe` then `load_glass_probe` round-trips; malformed store node → nullopt. `ScreensaverManager` fingerprint unchanged for the same facts (existing screensaver tests stay green).
- [ ] **Step 2:** `make t F='[glass]'` → FAIL.
- [ ] **Step 3:** Implement; extract `current_board_facts` and make `ScreensaverManager::board_facts` call it. Probe uses `lv_canvas_init_layer`/`lv_canvas_finish_layer` and `lv_draw_rect` with gradients built from Task 1's `glass_stops`.
- [ ] **Step 4:** `make t F='[glass]'` and `make t F='[screensaver]'` → PASS. Break one decision branch, see red, restore.
- [ ] **Step 5:** Commit `feat(glass): hardware gate with per-board probe record`.

### Task 3: Draw-task painter for buttons, cards, dialogs

**Files:**
- Create: `include/glass_painter.h`, `src/ui/glass_painter.cpp`
- Test: `tests/unit/test_glass_painter.cpp`

**Interfaces:**
- Consumes: Task 1 (`glass_stops`, `glass_rim_color`, `GlassFamily`, `kGlassGlowRadiusPx`); `ThemeManager::get_style(StyleRole)`, `ThemeManager::is_dark_mode()`.
- Produces:
  ```cpp
  namespace helix::ui {
  GlassFamily glass_family_of(const lv_obj_t* obj, lv_part_t part);
  void glass_mark(lv_obj_t* obj, GlassFamily family);   // explicit family; None = exempt
  void glass_attach(lv_obj_t* obj);                     // flag + DRAW_TASK_ADDED handler
  void glass_detach(lv_obj_t* obj);
  void glass_rewrite_fill(lv_draw_fill_dsc_t* dsc, GlassFamily f, bool dark, bool pressed);
  void glass_rewrite_border(lv_draw_border_dsc_t* dsc, GlassFamily f, lv_color_t base, bool dark);
  }
  ```
- Classification (MAIN part): a `glass_mark` marker wins. Else Raised if the object carries `StyleRole::Button` or any solid `Button*` role (Primary, Secondary, Tertiary, Danger, Success, Warning); None if it carries Ghost, Transparent or Outline; Surface for `Card` or `Dialog`; otherwise None. Markers are four static empty `lv_style_t`s identified by pointer. Style presence is read from the object's style list (`lv_obj_private.h`); one helper, `obj_has_style(obj, style, part)`.
- Rewrite: only fill tasks with `opa == LV_OPA_COVER` and `grad.dir == LV_GRAD_DIR_NONE`. Raised/Recessed → vertical gradient; Surface → radial centred at the task area's top-left, end circle at `kGlassGlowRadiusPx`. Pressed = `lv_obj_has_state(obj, LV_STATE_PRESSED)`. Border tasks: color = `glass_rim_color(family, lv_obj_get_style_bg_color(obj, part), dark)`.
- Precedent for the handler shape: `src/ui/ui_text.cpp` (`LV_EVENT_DRAW_TASK_ADDED`) and `src/plugin/plugin_table.cpp`.

- [ ] **Step 1: Failing tests** (`[glass]`, `LVGLTestFixture`):
  - `glass_family_of`: buttons with Primary → Raised; Ghost/Outline/Transparent → None; obj with Card → Surface; Dialog → Surface; plain obj → None; `glass_mark(obj, None)` on a Primary button → None; `glass_mark(obj, Surface)` on a plain obj → Surface.
  - `glass_rewrite_fill: uses the fill's own color`: dsc color `0xFF00FF` (a local override) → stop 0 equals `glass_stops(Raised, 0xFF00FF, ...)`.color[0].
  - `glass_rewrite_fill: leaves translucent fills alone`: opa 128 → dsc unchanged (memcmp).
  - `glass_rewrite_fill: leaves an existing gradient alone`.
  - `glass painter renders a card differently, and detach restores it exactly`: `lv_snapshot_take` of a 200x120 Card obj flat; attach → snapshot differs; detach → byte-identical to the flat snapshot.
- [ ] **Step 2:** `make t F='[glass]'` → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make t F='[glass]'` → PASS; break the opa guard, see red, restore.
- [ ] **Step 5:** Commit `feat(glass): draw-task painter for buttons, cards and dialogs`.

### Task 4: Theme styles in glass mode

**Files:**
- Modify: `include/theme_manager.h` (`ThemePalette`, `ThemeManager`), `src/ui/theme_manager_new.cpp#ThemeManager::apply_palette`, `src/ui/style_configs.cpp` (`configure_button`, `configure_card`, `configure_dialog`)
- Test: `tests/unit/test_glass_painter.cpp` (theme section), existing `[theme]` tests

**Interfaces:**
- Produces: `bool ThemePalette::glass = false;`, `void ThemeManager::set_glass(bool on)` (re-applies the current palette with `glass` set, then `lv_obj_report_style_change(nullptr)`), `bool ThemeManager::is_glass() const`. `apply_palette` and `set_dark_mode` keep the flag.
- With `p.glass`: buttons get shadow width 12, offset_y 5, opa 102; cards and dialogs width 10, offset_y 4, opa 90; shadow color black in dark, `0x1E2846` at opa 56 in light; border width at least 1. With `!p.glass`, every configure function is byte-for-byte today's.

- [ ] **Step 1: Failing tests:** `set_glass(true)` → Button style shadow width 12; `set_glass(false)` → identical property values to a never-glassed init; glass on then `set_dark_mode(false)` → `is_glass()` still true and Card shadow color is the light value.
- [ ] **Step 2:** `make t F='[glass]'` → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make t F='[glass],[theme]'` → PASS; break one, red, restore.
- [ ] **Step 5:** Commit `feat(glass): glass shadows and rims in the shared theme styles`.

### Task 5: Effective state, setting, Appearance row

**Files:**
- Modify: `include/display_settings_manager.h`, `src/system/display_settings_manager.cpp`, `src/ui/theme_lvgl_apply.cpp` (apply callback), `ui_xml/settings_appearance_overlay.xml`, `src/ui/ui_settings_appearance.cpp`, app startup (the place that runs after the first panel is shown; find it from `Application`), `docs/devel/ENVIRONMENT_VARIABLES.md`, translation sources (procedure: memory `reference_adding_translated_strings`, i.e. sync → fill → generate → CJK font regen)
- Modify: `include/glass_gate.h`, `src/ui/glass_gate.cpp` (controller part)
- Test: `tests/unit/test_glass_gate.cpp`, `tests/unit/test_glass_painter.cpp`

**Interfaces:**
- Produces:
  - `DisplaySettingsManager::get_glass_effects()/set_glass_effects(bool)`, subject `settings_glass_effects` (int 0/1, config `/display/glass_effects`, default 0), subject `glass_available` (int 0/1, default 0, set only by the gate).
  - `void helix::ui::glass_gate_start();` called once after boot is idle: decides; NeedsProbe → `lv_timer` one-shot 10 s later runs `measure_glass_frame_ms` at the display resolution, saves the record, sets `glass_available`. Logs verdict + ms at INFO.
  - `void helix::ui::glass_set_effective(bool on);` / `bool glass_effective();` driven by an `observe<int>` on both subjects (`observer_factory.h`): on → `ThemeManager::set_glass(true)` then `glass_attach` on every object of every screen of the default display plus `lv_layer_top()` and `lv_layer_sys()`; off → the reverse. The theme apply callback calls `glass_attach(obj)` for new objects while effective.
- XML row (between `row_theme_settings` and `row_animations`):
  `setting_toggle_row name="row_glass_effects" label="Glass Effects" icon=<pick an existing icon> description="Depth and lighting on buttons and panels" subject="settings_glass_effects" callback="on_glass_effects_changed"` with `<bind_flag_if_eq subject="glass_available" flag="hidden" ref_value="0"/>`.

- [ ] **Step 1: Failing tests:** saved on + unavailable → not effective; available + saved on → effective, a Card on `lv_layer_top()` gets `LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS`; flip off → flag cleared and `ThemeManager::is_glass()` false; `HELIX_GLASS=0` with a stored pass → `glass_available` 0; NeedsProbe path sets available from a stub measurement (inject the measure function: `glass_gate_start(std::function<double(int32_t,int32_t)> measure)` overload for tests).
- [ ] **Step 2:** `make t F='[glass]'` → FAIL.
- [ ] **Step 3:** Implement. XML is hot-reloaded; no rebuild needed for the row.
- [ ] **Step 4:** `make t F='[glass]'` → PASS; break the availability check, red, restore. `make test-xml`-level lint is covered at the finish.
- [ ] **Step 5:** Commit `feat(glass): Glass Effects setting, gate wiring and live switching`.

### Task 6: The rest of the chrome

**Files:**
- Modify: `src/ui/glass_painter.cpp` (`glass_family_of` for more classes and parts), the owning C++ of surfaces that set their bg locally (start with the navbar: `ui_xml/navigation_bar.xml` uses `style_bg_color="#card_bg"`, marked from its C++ owner with `glass_mark(..., Surface)`), `src/ui/ui_keycap_style.cpp` if keycaps need it
- Test: `tests/unit/test_glass_painter.cpp`

**Interfaces:**
- Consumes: Task 3.
- Classification additions: `lv_switch`, `lv_slider`, `lv_arc` → KNOB Raised, MAIN Recessed, INDICATOR Raised; `StyleRole::InputBg` (textarea, dropdown) → Recessed; `lv_bar` MAIN → Recessed, INDICATOR → Raised; `lv_checkbox` INDICATOR → Raised; keyboard (`lv_buttonmatrix`) ITEMS → Raised; `lv_dropdownlist` → Surface. Content classes (`lv_image`, `lv_canvas`, charts, camera, gcode/3D views, bed mesh, swatches) → None, enforced by test.
- Color-identity buttons (LED color chips `src/ui/ui_led_chip_factory.cpp`, any button whose bg is set from user/filament data: grep `lv_obj_set_style_bg_color` on buttons) → `glass_mark(obj, None)`.

- [ ] **Step 1: Failing tests:** one per added class/part above; `lv_image`/`lv_canvas`/`lv_chart` → None; an LED chip → None.
- [ ] **Step 2:** `make t F='[glass]'` → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make t F='[glass]'` → PASS; break one, red, restore.
- [ ] **Step 5:** Commit `feat(glass): glass on controls, inputs, bars, keys and the navbar`.

### Task 7: Look tuning from real pixels (orchestrator, with Preston)

Not an agent task. With `HELIX_GLASS=1 HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`, capture home, controls, print status (`HELIX_MOCK_AUTO_PRINT=1 --sim-speed 6`), filament/AMS, a modal, Appearance, in dark and light, at 800x480 and the smallest breakpoint. Tune Task 1's table and `kGlassGlowRadiusPx` (it may scale per breakpoint; never per card) until Preston signs off. Long lists: if settings screens read as stripes, give rows inside a Surface group a quieter Surface variant (new `GlassFamily` value only if needed).

### Task 8: Docs

- `docs/devel/ENVIRONMENT_VARIABLES.md`: `HELIX_GLASS` (done in Task 5).
- A "Glass appearance" section in `docs/devel/THEME_SYSTEM.md` (families, `glass_mark`, the exempt rule for color-identity widgets and content, the gate), a pointer from `docs/devel/COLOR_SWATCHES.md` that swatches stay flat, and the shared `current_board_facts` in `docs/devel/SCREENSAVERS.md`.
- Delete both plan files and `docs/devel/plans/glass/` in the change that ships it.

### Task 9: Finish (orchestrator, once)

1. One review of the whole branch (Opus, `docs/devel/REVIEW_RUBRIC.md`, DRY sweep included).
2. `make mutate-diff MUTATE_ARGS="--tests '[glass],[theme],[screensaver]'"`.
3. Screenshots of every changed screen, flat vs glass.
4. One `make land-gate`, then land on main, push, `scripts/teardown-worktree.sh glass-appearance`.
