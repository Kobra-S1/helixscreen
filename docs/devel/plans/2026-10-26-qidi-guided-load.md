# QIDI guided filament load

Status: in progress on `feature/qidi-load-wizard`. Deleted in the change that ships it
(`docs/CLAUDE.md`).

- Done: the wizard (flow, modal, executor hook, ctl demos), the sensor restore after
  `M603`, box-slot load/unload through `MULTI_COLOR_LOAD`/`MULTI_COLOR_UNLOAD` on the
  controller dialect. Durable notes: `FILAMENT_MANAGEMENT.md` § "Stock load macros that are
  one step of a wizard", `FILAMENT_BACKEND_QIDI_BOX.md` § Control Surface.
- Open: the external spool as a load target when a box is attached; answering the box
  controller's `operation.is_waiting_user`. Both need a box to verify.

## Problem

On a QIDI with no load macro of its own, Load runs the stock `M604 S<temp>` and
reports done. The stock screen treats `M604` as the first step of a wizard, and
the steps differ by firmware generation (verified on the Q2 at 01.01.02.01,
2026-10-26; 1.1.x read from QIDITECH/QIDI_Q2):

| Step | 1.1.x | 01.01.02.x |
|---|---|---|
| `M604 T0 S<t>` | home, trash chute, heat, extrude 80mm | home, trash, heat, then wait: `print_stats_manager.sub_status = push_filamenting` |
| bite | n/a | `SET_PRINT_SUB_STATUS SUB_STATUS=bit_filamenting` + `M604 T1 S<t>` (80mm), sent by the stock screen when the filament sensor sees the push |
| retry | `M604 T1 S<t>` (trash, heat, 80mm) | `M604 T1 S<t>` |
| confirm | `M104 S0` | `M604 T2 S<t>`: flush 80mm, `main_status = ready`; nozzle left hot |

Both generations run `DISABLE_ALL_SENSOR` in `M603`/`M604`, and nothing turns
the sensor back on until `PRINT_START`/`RESUME_PRINT`.

New firmware after our `T0`: hot nozzle, filament never gripped (Felix's video).

## Design

**Vendor knowledge in one module.** `filament_macro_profiles` already owns the
M604/M603 rows. It grows one capability question:

```cpp
namespace helix::filament_macros {
struct GuidedLoadProtocol {
    std::string waiting_object;    // printer object to query after the load macro
    std::string waiting_field;     // its field
    std::string waiting_value;     // value meaning "the macro stopped for a hand push"
    std::string feed_gcode;        // {temp} template; the bite, sent from the push step
    std::string retry_gcode;       // {temp}; extrude again from the confirm step
    std::string confirm_gcode;     // {temp}; empty when the generation has no flush step
    std::string finish_gcode;      // after confirm: cooldown + sensor restore
    std::string cancel_gcode;      // dismissed or failed mid-flow
};
/// nullopt for every macro with no stock wizard behind it.
std::optional<GuidedLoadProtocol> guided_load_protocol(const std::string& macro_name);
/// Gcode the macro leaves undone that the caller must send afterwards
/// (M603: ENABLE_ALL_SENSOR). Empty when none.
std::string restore_after(const std::string& macro_name);
}
```

The generation is read at runtime (one `printer.objects.query` of
`waiting_object.waiting_field` after the load macro returns), never from macro
text: a missing object or another value means the macro already extruded.
New-firmware-only lines (`SET_PRINT_SUB_STATUS`) live only in the feed gcode,
which only the waiting generation reaches.

**Pure flow** (`include/guided_load_flow.h`): stages `AwaitPush`, `Feeding`,
`ConfirmOutput`, `Flushing`, `Finished`, `Cancelled`. Events return the gcode to
send (empty = nothing): `start(waits_for_push)`, `filament_detected()` (AwaitPush
only), `extrude()`, `command_finished(ok)`, `done()`, `cancel()`. Events while a
command is in flight are ignored. A failed command cancels with `cancel_gcode`.

**Executor hook.** `dispatch_load`'s macro-tier success, for a macro with a
protocol: query the waiting field, then open the modal at `AwaitPush` (waits) or
`ConfirmOutput` (already extruded). The temperature is the `S` the macro was
sent with. The surface's own `finished()` still runs as today; the modal owns
the rest. The unload success path sends `restore_after()` when non-empty.

**Modal** (`GuidedLoadModal`, `ui_xml/guided_load_modal.xml`), one int subject
for the stage:
- AwaitPush: "Push the filament into the extruder until it stops." [Cancel] [Extrude].
  A 0 to 1 edge on `filament_runout_detected` (live even while firmware has the
  sensor off) sends the bite, as stock does.
- Feeding / Flushing: "Extruding..." no buttons.
- ConfirmOutput: "Is new filament coming out of the nozzle?" [Extrude more] [Done].
- Dismissal (backdrop, ESC, rebuild) = cancel.

## With a QIDI Box

01.01.02.x is QIDI's current Q2 firmware (V01.01.02.04 is the newest release, and
what our Q2 runs); 1.1.x is the previous generation. Ship for both, flag the box
paths as unverified in the beta notes, ask box owners for feedback.

- External spool: `M604 T0` handles the box itself (`E_LOAD SLOT=16` when
  `box_extras` exists). `AmsBackendQidi` gains an external-spool load target that
  routes to the macro tier, so the same wizard runs. Today it has none
  (`supports_bypass = false`), so with a box the external spool cannot be loaded.
- Box slots on current firmware (`multi_color_controller` present, the same
  discriminator `box_uses_multi_color_` already reads): load/unload as
  `MULTI_COLOR_LOAD` / `MULTI_COLOR_UNLOAD SLOT=slotN`, matching the stock screen.
  1.1.x keeps `EXTRUDER_LOAD` / `M603`.
- `multi_color_controller.operation.is_waiting_user` true: show the confirm the
  stock screen shows and send `USER_CONFIRM_CONTINUE`.

## Tests

- `[filament_macros]`: protocol present for M604 (any case), absent for others;
  templates substitute temp; M603 restore is `ENABLE_ALL_SENSOR`, M604 none.
- `[guided_load]` flow: waits path start to AwaitPush; detect sends feed once,
  second detect ignored; extrude while Feeding ignored; feed ok to ConfirmOutput;
  retry sends retry; done with confirm_gcode sends confirm+finish then Finished
  on ok; done without confirm sends finish; failure in any command sends cancel;
  cancel after Finished sends nothing; non-waiting start lands in ConfirmOutput.
- Executor: macro tier with M604 opens the flow after the query; another macro
  does not; query error counts as non-waiting.
