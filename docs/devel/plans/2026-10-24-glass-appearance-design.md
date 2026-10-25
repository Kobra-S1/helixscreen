# Glass appearance: design

## Intent

The flat UI is getting stale. We want a fresher, depth-y look with our own character:
buttons that read as physical glass you press, and cards lit by a corner light. It is
for capable hardware only. Weak boards keep today's flat UI and never see the option.

The direction came from looking at Aqua, Aero, the iOS 6 bevel and iOS 26/27 Liquid
Glass. We kept Liquid Glass's bright top edge and light rim and B/C's light-to-dark
body, dropped real blur, the pill shape and gloss-line seams, and added a corner light
on cards, which nobody else does and which is the part that makes it ours.

## Shape: a modifier, not a theme

Glass is a display setting layered over whichever theme is active: **Appearance: Flat /
Glass**, default Flat. It is not a theme file.

- Every shading color is **derived from the active palette** by lightening or darkening
  it. Glass never shifts hue, so it is safe on any theme (Nord, Gruvbox, Catppuccin...).
  This is a hard rule.
- On a board that fails the capability gate the setting is **hidden**, not greyed out,
  and rendering is flat regardless of what the saved value says. The user's theme
  choice is never touched.
- **Placement (Preston approved):** Settings → Appearance, top group, a
  `setting_toggle_row` named "Glass Effects" directly under "Theme Colors", description
  "Depth and lighting on buttons and panels", no BETA tag
  (`ui_xml/settings_appearance_overlay.xml`). Hidden through a `glass_available`
  subject binding, the way `settings_dark_mode_available` gates Dark Mode. The Theme
  Colors preview renders with the current glass state; no second switch anywhere.
  Mockup: `docs/devel/plans/glass/appearance-mock.png`.
- **Not beta-gated, no BETA tag** (Preston): the hardware gate alone decides. Whether
  to tag it beta is decided at 1.1 ship time, not now.

## The look

Reference mockups (browser approximations, 24-bit): `docs/devel/plans/glass/`
`hybrid.html` option 1 (buttons), `one-lamp.html` option A (cards),
`light-mode.html` option 1 (light mode). Pixel values below are starting points to
tune against real LVGL screenshots, not commitments.

### Buttons (solid variants only)

Applies to primary, secondary, tertiary, danger, success, warning and the default
(elevated_bg) button. Ghost, transparent and outline stay exactly as today.

Dark mode, with `c` = the variant color:
- Vertical gradient: `c` +45% toward white at 0%, +22% at ~9% (the bright top edge),
  +6% at ~40%, then a smooth fade to `c` -22% toward black at 100%. No hard step.
- Border: 1px, `c` +40% toward white (the light rim).
- Outer shadow: soft, ~12px, ~0.4 opacity, offset down ~5px.
- Pressed: the gradient flips (dark top, `c` -25% → `c` -5%) and the shadow
  tightens. The existing scale-down press (`configure_pressed_for_tier`) can stay.
- Corners: ~10px for now (the mockups' value). The theme's existing `button_radius`
  should still win if the builder finds that cleaner; Preston said Apple's pill was
  "too round", so anything up to ~14px is in range.

Light mode ("Daylight"): same gradient, rim **darker** instead of lighter (`c` ~-22%
toward black; softened from the mockup's -35% at Preston's request), shadows cooler
and softer (a navy-ish shadow color at ~0.2 opacity plus a tight 1-2px contact shadow).

### Cards

Each card glows from its **own top-left corner** (mockup "A"). Not a screen-wide lamp,
and buttons do not tilt toward it.

- Dark: radial gradient centred at the card's top-left, card_bg +14% toward white at
  the centre, card_bg at ~55%, card_bg -15% toward black at the far corner.
- Light ("Daylight"): the card base is pulled slightly below card_bg (a few % darker)
  so the corner can brighten up to card_bg/white. Same geometry.
- **Glow radius is fixed in pixels**, not a percentage of the card, so a full-width
  print-status card and a 1x1 home widget catch the same amount of light. Builder
  picks the value from screenshots at 800x480.
- Border 1px slightly lighter than card_bg (dark) / a light grey (light); soft shadow.
- `LV_USE_DRAW_SW_COMPLEX_GRADIENTS` is already 1 in `lv_conf.h`, so radial is
  available. `LV_GRADIENT_MAX_STOPS` is 8.

### Everything else: comprehensive

Preston's ruling: the look is comprehensive, not buttons-and-cards. Every piece of UI
chrome gets glass when it is on: modals and dialogs, home-panel widgets, list and
settings rows, sidebar cards, the navbar, toggles, sliders, chips, dropdowns, inputs,
progress bars, keyboard keys. Two families cover it, so it stays one material:

- **Raised controls** (anything you press or drag: buttons, chips, navbar buttons,
  toggle and slider knobs, keycaps) take the button treatment.
- **Surfaces** (cards, modals, widgets, rows, sidebar cards) take the corner-light card
  treatment. Rows in a long list need a quieter version so a settings screen doesn't
  turn into a stack of shaded strips; builder tunes it from screenshots.
- Tracks, inputs and progress-bar troughs are the builder's call within the same
  vocabulary (a recessed light-to-dark is the natural fit).

**Exempt, stays flat:**
- Filament color swatches (Preston's ruling): the color identifies a spool, so shading
  must not shift it. Needs a per-widget opt-out the swatch code sets.
- Content, as opposed to chrome (Claude's call): camera feed, print thumbnails, gcode
  and 3D previews, bed-mesh and temperature-graph plots, images. Same reason as the
  swatches: their pixels are data.

Builder's call: whether to land it in phases (e.g. buttons + cards + gate first, then
the rest), as long as the setting is not exposed until it is comprehensive.

## Hardware gate

Model it on the screensaver gate: measure on the real board, remember the result per
board and app version.

- Existing pieces: `PlatformCapabilities::detect()` / `PlatformTier`,
  `full_style_effects_allowed()` (`include/platform_capabilities.h`), and the
  screensaver level store keyed on `board_fingerprint(BoardFacts)`
  (`include/screensaver_level_store.h`, used from `src/ui/screensaver_manager.cpp`).
  Reuse the fingerprint and the store's shape; do not fork a twin.
- Automatic fail without measuring: `LV_COLOR_DEPTH` below 24 (banding on radial
  gradients shows as rings), and the tiers `full_style_effects_allowed()` rejects.
- Otherwise measure: render a representative frame of glass cards and buttons and
  compare against a budget. Builder designs the probe and the budget; it must not be
  visible to the user or delay boot noticeably.
- Env override to force pass/fail for testing, matching the `HELIX_SCREENSAVER_*`
  pattern, documented in `docs/devel/ENVIRONMENT_VARIABLES.md`.

## Constraints

- **Per-widget colors must survive.** About 100 call sites in `src/ui/` set a bg color
  locally (`lv_obj_set_style_bg_color`), filament swatches among them. A theme-level
  `bg_grad` descriptor replaces the fill colors entirely and would paint over those.
  The builder must choose a mechanism where a locally set bg color still shows with
  glass shading on top (e.g. derive the gradient from the object's resolved bg color
  at draw time, or draw the shading as a translucent overlay in a draw event), and
  prove it on a swatch.
- Theme switches, live recolor (`src/ui/theme_live_recolor.cpp`), dark/light toggle
  and hot reload must all re-derive glass correctly.
- Declarative UI rules apply (`.claude/rules/declarative-ui.md`): no per-panel C++ to
  opt widgets in. It hangs off the shared button/card styles
  (`src/ui/style_configs.cpp`, `src/ui/theme_lvgl_apply.cpp`).
- No measurable frame-rate loss on boards that pass the gate (Preston's rule: keep the
  highest sustainable frame rate).
- Flat mode must be byte-for-byte today's styling.

## Goals / non-goals / anti-goals

Goals: the look above on buttons and cards, dark and light, on any theme, on capable
hardware, switchable at runtime.

Non-goals (later, maybe): real blur. Two shapes were discussed and parked: frosted cards
over a pre-blurred wallpaper (Windows Mica style), and live glass over moving content
(camera, 3D preview) on GPU boards using the existing `src/ui/backdrop_blur.cpp`. Also
not in scope: per-theme glass tuning knobs.

Anti-goals: anything that changes how flat mode looks; any glass on a board that fails
the gate, including a "lite" fallback; hue shifts; a gloss-line seam; pill buttons.

## Calls made in review (Claude's, for Preston to check)

- **Mechanism may be a global hook.** The declarative rule forbids per-panel opt-in
  code, not a hook installed centrally (theme apply / widget creation) on every
  matching object. That is allowed. Nothing is wired per XML file.
- **"Flat unchanged" means identical pixels.** In Flat mode no glass hook is installed
  at all, so it costs nothing; the test compares flat screenshots before and after.
- **Runtime switch, no restart.** Flipping the setting, a theme change, dark/light and
  hot reload all re-derive glass live.
- **The probe runs in the background after boot is idle**, once per board fingerprint +
  app version, off the live display (an offscreen draw buffer). The setting stays
  hidden until a pass is recorded; after that it shows immediately on every boot.
  Pass criterion is the builder's (absolute ms per frame against the display's refresh
  budget is the expected shape). Its result is stored beside the screensaver levels,
  not mixed into them.
- **Effective glass = gate passed && setting on.** A saved Glass value on
  a board that fails is kept but ignored. `ctl`/settings reads report the saved value;
  the effective state is logged at INFO.
- **Corner-glow radius** is fixed in pixels per breakpoint (it may scale with the
  breakpoint, never with the card).

## Left to the builder

The rendering mechanism (see the per-widget color constraint), the probe and budget,
setting name and placement in Display settings, exact pixel values after screenshots,
and whether the pressed scale-down stays.

## Proving it

Screenshots of home, controls, print status (active print via `--sim-speed`), a
filament/AMS screen with swatches, and a modal, in dark and light, at 800x480 and the
smallest breakpoint, flat vs glass. Tests for: the palette-derivation function (pure:
color in, stops out, no hue shift), the gate decision (facts in, pass/fail out), the
setting being hidden on fail, flat mode unchanged.
