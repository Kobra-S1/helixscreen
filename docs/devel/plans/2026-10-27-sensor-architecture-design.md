# Sensor Architecture Cleanup: Design

Status: draft for review. Branch `refactor/sensor-architecture`.

## Why

Sensor support grew one manager at a time, and each new manager was a copy of the last. The result
works, but it is hard to read, hard to change safely, and every new sensor kind costs a tour of six
files. Preston's priorities for this cleanup, in order: **code readability, maintainability, and ease
of development.** Firmware size is a welcome side effect, not the goal.

What the copies cost today, measured on main at `29794e3709`:

- **Name recognition is written out three times.** `PrinterDiscovery::parse_objects`
  (`src/printer/printer_discovery_parse.cpp`), `MoonrakerDiscoverySequence::parse_objects`
  (`src/api/moonraker_discovery_sequence.cpp`) and each manager's private `parse_klipper_name()`
  decide independently what a Klipper object is. Four more files do it again for fans, LEDs, MCUs
  and display names. Only the heater rule is shared. The copies disagree in at least a dozen
  places, several of them user-visible (see "Behavior fixes").
- **Eight managers share a vocabulary but no code.** About 150-200 lines of lifecycle per manager
  are hand copies, and they have drifted: four different locking rules at teardown, deferred
  subject updates that read sensor state without the lock in five managers, three persistence
  shapes, role exclusivity decided four ways, and auto-role assignment in `discover()` for some
  managers and in `load_config_from_file()` for another.
- **Dead API compiled everywhere.** Humidity, temperature and accelerometer carry role setters
  and persistence that nothing in `src/` calls; several subjects have no consumer.
- **Settings > Sensors builds rows three ways.** Only the Inputs section is declarative. Four
  sections build rows with `lv_obj_create` and inline styles; three row XML components are never
  instantiated; role dropdowns map option index to enum value by cast.

## Goals

1. **One table** classifies every Klipper object name and config section: sensors, fans, heaters,
   LEDs, steppers, MCUs. Discovery, the subscription builder, every sensor manager, the hardware
   role registry, fan state, the LED controller, the MCU performance source and device display
   names all ask it. Adding a chip or object name is one row.
2. **One base class**, `SensorManager`, non-template, under all eight Klipper sensor managers and
   `SensorState` (Moonraker `server.sensors`). It owns the lifecycle, threading, sync mode, count
   subject, discovery reconcile, role assignment, persistence and the Settings row model. A
   derived manager contains only what is genuinely its own.
3. **One registry** of managers replaces `for_each_sensor_manager` and every hand-written list of
   managers (discovery wiring, preset reload, telemetry counts, Settings sections).
4. **One Settings row** component for every sensor kind, fully declarative.
5. **Adding a sensor category** becomes: a table row (or a few), one manager class, one registry
   line. The `SENSOR_MANAGEMENT.md` recipe shrinks from eight steps across six files to that.
6. Delete what nothing uses.

## Non-goals

- No new user-facing features. No new role pickers for humidity, temperature or accelerometer
  (Preston: delete that dead code).
- No change to what any printer shows or does, except the listed behavior fixes.
- No settings migration. Saved JSON under `filament_sensors`, `probe_sensors`, `width_sensors`
  and `input_sensors` keeps its exact shape and is read unchanged.
- No changes to the AMS backends' own filament logic beyond how their object names are
  classified.

## Anti-goals (failure even if it works)

- **A mega-manager.** Filament runout, grace periods, lane scoping, toasts and bypass arming must
  read top to bottom in `FilamentSensorManager`, not be scattered across base-class hooks. If a
  hook exists only for one manager, it does not belong in the base.
- **Template sprawl.** A design that re-instantiates the lifecycle per manager (CRTP, a templated
  base) defeats both readability and the firmware size goal.
- **Behavior drift hidden in the refactor.** Every behavior change is either in the "Behavior
  fixes" list with its own test, or a bug.
- **Broken saved settings.** A user's role assignments survive the upgrade.

## Constraints

- Threading invariants (`.claude/rules/threading.md`, `docs/devel/THREADING.md`): subject writes on
  the main thread only; background status parsing deferred through a lifetime token; deinit
  invalidates first.
- Firmware: no exceptions, no RTTI (`scripts/check_esp32_app_srcs.py`). ESP32 image size must
  not grow; measured before and after through helixscreen-ae's thelio build queue.
- Vendor abstraction (`.claude/rules/vendor-abstraction.md`): vendor object names are data rows in
  the one classification module; behavior that differs by vendor stays in its provider module.
- Declarative UI (`.claude/rules/declarative-ui.md`): data in C++, appearance in XML.
- Verification cadence (`CLAUDE.md`): tags while building; one review, scoped mutation, screenshots
  and one land-gate at the finish.
- `src/api/moonraker_discovery_sequence.cpp#start` and its callback plumbing are being changed by
  helixscreen-29 (#1874 port to `helix::SharedCallback`). Do not touch `start()`; keep edits in
  `parse_objects` and `build_subscription_objects` so the merge stays mechanical.

## Design

### 1. The object table

New `include/hardware_object_table.h`, `src/printer/hardware_object_table.cpp`, namespace
`helix::hw`.

```cpp
enum class ObjectKind : uint8_t {
    Unknown,
    Stepper, Mcu,
    Heater,             // extruder*, heater_bed
    GenericHeater,      // heater_generic <name>: a heater and a temperature sensor
    TemperatureSensor,  // temperature_sensor <name>
    TemperatureFan,     // temperature_fan <name>: a sensor and a fan
    DriverTemperature,  // tmc2240 / tmc5160 with a temperature reading
    HumidityChip,       // bme280, htu21d, sht3x, aht10/20/20_f/3x, temperature_oams
    Fan, HeaterFan, ControllerFan, GenericFan, OutputPinFan,
    Led, OutputPinLed, LedEffect, Speaker,
    FanFeedback,
    FilamentSwitch, FilamentMotion, BackendFilamentSensor,
    WidthSensor, LoadCell, Probe, Input,      // Input = gcode_button
    Accelerometer,      // config section only: no get_status
    PowerLossMonitor,
};

struct ObjectClass {
    ObjectKind kind = ObjectKind::Unknown;
    std::string_view instance;  // the part after the prefix ("chamber" in "temperature_sensor chamber")
    uint8_t chip = 0;           // kind-specific type index (humidity chip, probe type, accel chip, ...)
};

ObjectClass classify_object(std::string_view klipper_object);   // objects.list names
// configfile.config sections. Takes the section body because a few rules read
// field contents (Beacon's onboard accelerometer: accel_scale, resonance_tester.accel_chip).
ObjectClass classify_section(std::string_view section, const nlohmann::json& body);
const StatusFields& status_fields(ObjectKind kind);               // what to subscribe
bool is_fan(ObjectKind), is_heater(ObjectKind), is_sensor(ObjectKind), ...;  // family questions
```

The project is C++17 (`-std=c++17`, ESP32 `gnu++17`): no `std::span`. Sequences cross
interfaces as `const std::vector<T>&` or pointer plus count.

The body is one `constexpr` array of rows `{match: Exact|Prefix|PrefixOrBare, pattern, kind,
chip}`, scanned in order, plus one function for the `output_pin` keyword rule. Kind-specific
display strings and chip enums (`humidity_sensor_chips()`, the probe and accel type tables) move
into or next to this table so a new chip is one row.

`PrinterDiscovery` stores each object's `ObjectClass` once and offers
`objects_of(ObjectKind...)`. It also stores the classified config sections from
`parse_config_keys`, which already runs (configfile arm) before
`init_subsystems_from_hardware`, so config-only kinds like accelerometers are visible to the
same discovery pass as objects. Its existing bucket accessors (`sensors()`, `fans()`, `leds()`,
`load_cells()`, `width_sensor_objects()`, `filament_sensor_names()`, ...) become thin views over
that, so their many callers do not change in this branch.

`MoonrakerDiscoverySequence::parse_objects` stops bucketing. It already calls
`hardware_.parse_objects(objects)` first; it reads the buckets from there.
`build_subscription_objects` subscribes each discovered object with `status_fields(kind)`, plus
whatever each sensor manager adds through `SensorManager::extra_subscriptions()` (the probe's
"Cartographer reports on `probe`" rule, today's `ProbeSensorManager::required_status_objects`).

The table classifies object *kinds* only. Whether a filament sensor belongs to an AMS
(`PrinterHardware::is_ams_sensor`, which asks `has_mmu()` and each `AmsBackend`'s
`sensor_name_names_filament_system`) is a backend decision and stays there; its four copies
(Settings overlay, three in the filament wizard) become one function. The filament manager keeps
discovering every `filament_sensor_names()` entry as today.

Other name classifiers switch to the table: `hardware_role_registry.cpp` `is_*_candidate`,
`printer_fan_state.cpp`, `led/led_controller.cpp`, `moonraker_performance_source.cpp`,
`device_display_name.cpp`, `PrinterHardware::guess_*`.

`power_loss_sensor`: `power_loss_check` becomes a `PowerLossMonitor` row. The module's two
functions with no caller in `src/` (`firmware_reports_power_loss`, `power_loss_asserted`) are
deleted; `required_status_objects` becomes a table lookup used by the subscription builder and
the debug bundle collector.

### 2. The base class

New `include/sensor_manager.h`, `src/sensors/sensor_manager.cpp`, namespace `helix::sensors`.

The key decision: **the base owns the sensor list itself, non-templated.** Today each manager has
its own `SensorCollection<Config, State>`, which forces either a template base or duplicated
queries. Instead, everything the base needs is the same for every kind:

```cpp
struct SensorEntry {
    std::string klipper_name;   // "temperature_sensor chamber"
    std::string display_name;   // "chamber"
    uint8_t type = 0;           // kind-specific chip/type index, from the table
    uint8_t role = 0;           // index into the manager's RoleSpec table; 0 = none
    bool enabled = true;
    bool available = false;     // discovered and not yet gone
};

struct RoleSpec {               // one row per role a manager has
    uint8_t id;                 // the manager's role enum value
    const char* config_name;    // persisted name, unchanged from today
    const char* label_key;      // translation key, resolved when a row is built
    bool exclusive;             // one holder at a time
    bool offered;               // shown as a dropdown option (a held, non-offered role still displays)
};

struct TypeSpec {               // one row per sensor type a manager knows
    uint8_t id;
    const char* config_name;    // persisted "type" string, unchanged ("switch", "motion", ...)
    const char* label_key;
};
```

Each manager's typed live reading (temperature, humidity, filament state, ...) stays in the
derived class in a vector parallel to the entries, or keyed by name. Typed role enums stay too:
small inline helpers convert at the boundary (`role_of<TemperatureSensorRole>(entry)`), so
derived code still reads `if (role == TemperatureSensorRole::CHAMBER)`.

```cpp
class SensorManager {
  public:
    virtual ~SensorManager();

    const char* name() const;                         // log tag, registry and deinit name
    void init_subjects();                             // final
    void deinit_subjects();                           // final
    SubjectLifetime subjects_lifetime() const;
    void set_sync_mode(bool);                         // tests

    // Objects and config sections, both from PrinterDiscovery. No-op for a manager whose
    // source is not PrinterDiscovery (SensorState).
    void discover(const PrinterDiscovery& hw);        // final: reconcile, then on_discovered()
    // Default: lock, parse_status(), unlock, publish() when it reports a change.
    // Filament overrides it to keep its pre-lock AmsState read and post-unlock
    // notifications exactly where they are today (TSan ABBA cycle with AmsState).
    virtual void update_from_status(const nlohmann::json& status);
    virtual std::vector<std::string> extra_subscriptions(const PrinterDiscovery&) const;

    void load_settings();                             // final: no-op when config_key() is null
    void save_settings();                             // final

    // Read API shared by every manager
    size_t sensor_count() const;
    std::vector<SensorEntry> entries() const;
    const SensorEntry* find(std::string_view klipper_name) const;

    // Edits (Settings, wizard)
    // Exclusive roles are taken from other holders. Filament overrides it for RUNOUT,
    // which is exclusive per lane (lane_index_for_sensor), a rule only filament has.
    virtual void set_role(std::string_view klipper_name, uint8_t role);
    void set_enabled(std::string_view klipper_name, bool);
    virtual void set_option(std::string_view klipper_name, bool); // Invert today; default no-op

    // Settings row model (section 4); plain data, no LVGL
    virtual SensorRowView row(size_t index) const;
    lv_subject_t* revision_subject();   // bumped on every publish(); live rows observe it

  protected:
    struct Options {
        const char* name;
        std::vector<ObjectKind> kinds;            // objects and config sections it owns
        const char* count_subject;
        const char* config_key = nullptr;         // nullptr = not persisted
        std::vector<RoleSpec> roles;
        std::vector<TypeSpec> types;
        bool keep_missing_state = false;          // filament only, as today
        bool from_printer_discovery = true;       // false for SensorState
    };
    explicit SensorManager(Options);

    // Hooks
    virtual void register_subjects(SubjectManager&) = 0;           // main thread
    virtual void refresh_subjects() = 0;                            // main thread, lock NOT held
    virtual bool parse_status(const nlohmann::json& status) = 0;   // any thread, lock held
    virtual void on_discovered() {}                                 // lock held; auto roles live here
    virtual void read_entry_extra(SensorEntry&, const nlohmann::json&) {}       // lane, inverted
    virtual void write_entry_extra(const SensorEntry&, nlohmann::json&) const {}
    virtual void read_extra_settings(const nlohmann::json&) {}      // master_enabled, door_check
    virtual void write_extra_settings(nlohmann::json&) const {}

    void publish();               // sync mode: refresh now; else defer through the lifetime token
    std::recursive_mutex& mutex() const;
    std::vector<SensorEntry>& entries_locked();
};

const std::vector<SensorManager*>& sensor_managers();   // the one registry
```

What this settles, uniformly, for every manager:

- **Teardown:** `deinit_subjects` locks, invalidates the lifetime guard first, clears subjects.
- **Deferred refresh:** `refresh_subjects` runs on the main thread **without** the mutex. It
  copies what it needs under a short lock, releases it, then sets subjects. Setting a subject runs
  observers synchronously, and some of those observers call back into managers or into
  `AmsState`, which calls the filament manager under its own lock; holding the manager's mutex
  across subject writes would invite an ABBA deadlock. Today five managers read state with no
  lock at all here; the snapshot rule fixes that without the deadlock.
- **Count subject:** set by the base after every reconcile.
- **Reconcile:** against `hw.objects_of(kinds)`, objects and config sections alike, so
  accelerometers (config-only) reconcile in the same pass. Semantics are today's: an entry for a
  sensor that disappeared is dropped and its role with it; filament alone keeps the missing
  sensor's runtime state (`keep_missing_state`). Temperature's per-sensor subjects for a vanished
  sensor are torn down as today.
- **Auto roles:** always in `on_discovered()`, then saved settings override them. Probe's
  "single probe is the Z probe" moves there from `load_config_from_file`; it still applies only
  when no saved role names a probe.
- **Persistence:** the current shape, byte-compatible: `{sensors: [{klipper_name, role, enabled,
  type, <entry extras>}], <manager extras>}`. Role and type strings come from `RoleSpec` and
  `TypeSpec` with today's exact spellings. Entry extras (filament `lane`, written only when set;
  input `inverted`) and manager extras (`master_enabled`, `door_check`) go through the
  `*_extra` hooks. Reads are lenient: a wrong-typed field is skipped, not fatal. Round-trip
  fixtures copied from real saved configs pin it.
- **Role exclusivity:** `RoleSpec::exclusive` replaces three hand-written rules; filament
  overrides `set_role` for its per-lane RUNOUT rule.

The base is deliberately not the place for filament's status pipeline. Filament keeps its own
`update_from_status` (pre-lock `AmsState` read, toasts and the state-change callback after the
lock, on the thread they run on today) and its own `set_role`. Those are the two overrides it
needs; everything else it inherits.

`SensorState` derives from the base for the lifecycle, mutex, `publish()` and dynamic subjects.
It is constructed with `from_printer_discovery = false`, so the registry's `discover(hw)` loop
leaves it alone; it keeps its `set_sensors` entry point fed by `server.sensors` and its
`notify_sensor_update` handler, both now calling the base `publish()`. It has no roles, no
persistence and no Settings rows. Its per-(sensor, key) dynamic subjects and temperature's
per-sensor dynamic subjects are the same structure written twice today; both use one
`DynamicSubjectMap` helper.

`SensorCollection` (`include/sensor_collection.h`) and `for_each_sensor_manager`
(`include/sensor_managers.h`) are deleted once every manager is migrated.

### 3. Discovery wiring

`init_subsystems_from_hardware` (`src/printer/printer_discovery.cpp`) becomes a loop:
`for (auto* m : sensor_managers()) { m->discover(hw); m->load_settings(); }`. The configfile arm
in the discovery sequence keeps feeding `PrinterDiscovery::parse_config_keys` (which now records
classified sections) and stops calling the managers directly: the probe's z_offset seeding and the
input's press/release reactions read their sections in `discover(hw)` like everything else.
`reload_sensor_roles_after_preset` becomes `for (m) m->load_settings()`.
`temperature_sensor_objects()` disappears: temperature's kinds include `GenericHeater`.

### 4. Settings > Sensors

One `ui_xml/sensor_row.xml` component: icon, name, detail text (type or live reading), state
text with alert style, optional role dropdown, optional toggle (Enabled or Invert), all bound to
per-slot subjects `sensor_row_<field>_<slot>`.

The row subjects are owned by the Settings overlay, not by the managers. Slots are allocated on
demand while the overlay is open, one per row across all sections (temperature sensors are
unbounded, so there is no fixed maximum), and released when it closes; each section is a
`<repeat count="sensor_<section>_row_count">` of `sensor_row` (declarative rule 8). Role option
labels are static per section in XML (helix-xml has no options binding), and a test asserts they
match the manager's offered roles. The overlay asks each registered manager for its rows (`SensorManager::row(i)`
returning a plain `SensorRowView`: strings, the offered role options built from `RoleSpec`, the
selected role, toggle kind and value) and fills slots. Rows are live: the overlay observes each
manager's `revision_subject` and refills that section's slots, so live state text (input doors
today) keeps updating. Edits arrive with the slot index; the overlay maps slot to (manager, row)
and calls `set_role` / `set_enabled` / `set_option`, then `save_settings()`. Dropdown option index
maps to role through `RoleSpec`, never by enum cast; a held role that is not offered is shown as
the selected text without becoming a choice.

Section headers, visibility and count badges bind to each manager's count subject. The
`update_*_count()` imperative badge setters, every `populate_*` builder, the clear-children loops
and `probe_sensor_row.xml`, `humidity_sensor_row.xml`, `accel_sensor_row.xml`,
`filament_sensor_row.xml`, `width_sensor_row.xml`, `load_cell_row.xml` and `input_sensor_row.xml`
are deleted. The "no sensors" placeholder binds to a total count subject so it can actually show.
Chamber assignment dropdowns stay as they are (they write `SettingsManager`, not a sensor role).

The thermistor widget's picker rows and carousel pages
(`src/ui/panel_widgets/thermistor_widget.cpp`) and the temp graph widget's settings rows
(`src/ui/panel_widgets/temp_graph_widget.cpp`) build the same name-plus-value row imperatively
today. Home widgets are sized and recycled differently from an overlay, so they share the row
*model* (`SensorRowView` from the manager) but not the Settings component or its slot subjects;
each moves to its own XML component, declaratively bound. The filament sensor wizard keeps its own step UI but reads candidates through
the base API, and the four copies of the "standalone, not AMS" filter become one function.

### 5. Deletions

- Humidity, temperature, accelerometer: `set_sensor_role`, `set_sensor_enabled`, `load_config`,
  `save_config` (Preston: delete). Load cell's no-op stubs.
- Subjects with no consumer: `filament_probe_triggered`, `filament_motion_active`,
  `chamber_pressure`, `dryer_humidity`, `probe_triggered`, `probe_last_z`, and their getters.
- Public methods with no `src/` caller, per manager (full list in the research notes; the plan
  enumerates them).
- `ProbeLoadCellTestAccess` stale friend, `SensorCollection`, `for_each_sensor_manager`, the
  power-loss dead functions, the unused row XML.

## Behavior fixes

The table resolves every disagreement between the copies. Each resolution gets a test.

| # | Today | After |
|---|---|---|
| 1 | Humidity chips (`bme280 x`, `aht20_f box`) are subscribed but missing from temperature sensors | They also appear as temperature sensors |
| 2 | `output_pin` fan/LED: one copy uses "starts with FAN", the other "contains fan" | One rule: name split on `_`; a `fan`/`fans` token makes it a fan, else a `light`/`led`/`lamp` token makes it an LED; speaker tokens independent |
| 3 | Bare `neopixel`/`dotstar` recognized but never subscribed | Recognized and subscribed |
| 4 | `load_cell_probe`, `dockable_probe` not recognized as probes; `cartographer`/`beacon` missing from the XY-offset list | One probe list everywhere |
| 5 | `has_accelerometer` true for a bare `resonance_tester` with no chip | True only when an accelerometer is found |
| 6 | `temperature_sensor extruder_board` hidden by a name filter | Shown; only real heaters are excluded |
| 7 | MCUs bucketed in one copy only | One bucket |
| 8 | Role registry guesses can fail their own candidate check (`bed_fans`, `heat_fan`, `e0`) | Guesses only choose among candidates |
| 9 | Hardware validator reads `"name"`, which filament configs never write, so a missing configured sensor is never flagged | Reads through the manager API |
| 10 | Filament role dropdown selects by enum cast; a saved `Z_PROBE` (enum 10, settable only by hand-editing config) selects a wrong or missing option | Dropdown maps through `RoleSpec`. The filament `Z_PROBE` role is deleted: its only output is `filament_probe_triggered`, which nothing reads; a saved `"z_probe"` reads as None |
| 11 | Deferred subject refreshes read sensor state unlocked in five managers | State copied under the lock, subjects set after releasing it |
| 12 | "No sensors" placeholder can never show | Shows when every count is 0 |

## Testing

- **Golden discovery snapshot, written first, on today's code.** For every mock persona, record
  the classified object buckets, the subscription object/field set, and each manager's discovered
  entries and roles. After each migration step the snapshot must match, except rows the behavior
  fixes table predicts, each changed deliberately with its fix.
- **Table tests:** every row, every disagreement case above, vendor rows, unknown names.
- **Base tests, once, against a small test manager:** sync and deferred publish, drop after
  deinit, reconcile keep-missing, each `RoleScope`, settings round trip including today's saved
  JSON for each persisted manager (fixtures copied from real configs), lenient read.
- **Each manager keeps its domain tests;** `*TestAccess` helpers are updated, not dropped.
- **Settings:** XML binding tests for `sensor_row`; screenshots of every section before and
  after, shown to Preston.
- **ESP32:** image size before and after through helixscreen-ae's queue.
- Finish per the verification cadence: one review, scoped mutation, one land-gate.

## Migration order

Each step lands green on its own and keeps the golden snapshot.

1. Golden snapshot tests on current main.
2. The object table; `PrinterDiscovery` and the subscription builder read it; behavior fixes
   1-7 with their tests.
3. `SensorManager` base and registry; migrate width first as the worked example, then humidity,
   accelerometer, load cell, probe, input, temperature, filament (largest, last), `SensorState`.
   Deletions ride with each manager's migration.
4. Role registry, fan state, LED controller, MCU source and display names onto the table;
   fixes 8-9.
5. Settings row and slots; widgets' rows; fixes 10 and 12.
6. Rewrite `docs/devel/SENSOR_MANAGEMENT.md` for the new shape; delete this plan.

## Design calls made without Preston (check these)

These came out of the builder check; each is reversible and none changes what a user sees beyond
the behavior fixes table.

- Missing sensors keep today's semantics (entry and role dropped; filament keeps runtime state).
  Keeping roles across a disappearance would be a new behavior, so it is out.
- Filament keeps its own `update_from_status` and `set_role` overrides rather than the base
  growing filament-only pre-lock, post-unlock and per-lane hooks.
- `refresh_subjects` snapshots under the lock and writes subjects outside it, never holding the
  mutex across observer callbacks.
- `SensorState` derives the base for lifecycle only and stays out of the `discover(hw)` loop.
- Accelerometers and other config-only kinds are discovered through `PrinterDiscovery`'s
  recorded config sections in the same pass as objects; the discovery sequence stops calling
  managers directly.
- The AMS-sensor filter stays a backend decision, consolidated to one function; the table only
  classifies kinds.
- Settings row subjects are allocated per section on demand (no fixed cap), rows render through
  `<repeat>`, and rows refresh live off a per-manager revision subject.
- Home widgets share the row model, not the Settings component.
- The filament `Z_PROBE` role is deleted.

## Left to the builder

- Exact slot pool size and subject naming for the Settings rows, within the declarative rules.
- Whether `PrinterDiscovery` keeps a per-object `ObjectClass` vector or per-kind index lists.
- Internal file layout of the table rows and the per-kind display string tables.
- Log levels and messages, matching `docs/devel/LOGGING.md`.
