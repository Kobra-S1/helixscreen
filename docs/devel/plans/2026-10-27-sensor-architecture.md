# Sensor Architecture Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **This repo's Verification Cadence overrides both skills:** no per-task review, no mutation, sweep or gate inside a task (CLAUDE.md § Verification Cadence).

**Goal:** One object classification table, one non-template `SensorManager` base under every sensor manager, one manager registry and one declarative Settings row, with today's behavior preserved except the design's listed fixes.

**Architecture:** `helix::hw` classifies every Klipper object and config section; `PrinterDiscovery` stores the classes and every other classifier asks it. `helix::sensors::SensorManager` owns the sensor list, lifecycle, threading, persistence and row model; each manager keeps only its readings, subjects and domain rules. Settings > Sensors renders every manager through one `sensor_row` component bound to overlay-owned slot subjects.

**Tech Stack:** C++17 (`-std=c++17`, ESP32 `gnu++17`), LVGL 9.5 + helix-xml, Catch2, gmake.

**Spec:** `docs/devel/plans/2026-10-27-sensor-architecture-design.md` (approved by Preston 2026-10-27). Read it first; this plan argues from it.

## Global Constraints

- Worktree `.worktrees/sensor-architecture`, branch `refactor/sensor-architecture`. Build with `gmake` on a Mac. Never `git add -A`; commit with explicit paths.
- No `std::span` (C++17). Sequences cross interfaces as `const std::vector<T>&`.
- Firmware-compiled code: no exceptions, no RTTI. JSON through `json_util::as_*` / `find()` plus type checks, never `.value()`, `.at()`, one-arg `parse`.
- Every new `.cpp` under `src/` goes into `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` or `app_srcs_excluded.txt` in the same commit (`scripts/check_esp32_app_srcs.py` gates it). A deleted `.cpp` leaves both lists.
- Saved settings keep their exact JSON shape and strings under `filament_sensors`, `probe_sensors`, `width_sensors`, `input_sensors`. No config migration.
- Subject writes on the main thread only. `refresh_subjects()` never runs with the manager mutex held while setting a subject.
- Vendor object names are data rows in `src/printer/hardware_object_table.cpp`, nowhere else in generic code. The AMS-sensor decision stays in `AmsBackend` / `PrinterHardware`.
- Declarative UI rules (`.claude/rules/declarative-ui.md`): no `lv_obj_create` rows, no `lv_obj_add_event_cb`, no imperative hide/show or text in the new Settings code.
- Do not edit `MoonrakerDiscoverySequence::start()` or its callback plumbing (helixscreen-29's #1874 port).
- No comment archaeology; no em dashes in comments or docs; `constexpr` names `kLikeThis`.
- While building: `gmake t F='<task tags>'` once per change; a new test is hand-broken once and seen red. Nothing heavier.

## Review Focus

1. **A user's saved roles after the upgrade:** filament RUNOUT/TOOLHEAD/ENTRY with `lane`, probe Z_PROBE, width FLOW_COMPENSATION, input DOOR with `inverted` and `door_check` must read back identically. Pinned by Task 5's round-trip fixtures and each migration task's "saved config reads back" test.
2. **A reconnect to a printer whose sensors changed** (one removed, one added) keeps the survivors' roles, drops the removed one, and the count subject matches. Pinned in Task 5 (`reconcile drops a vanished sensor and keeps survivors`).
3. **A status frame arriving after shutdown began** must not touch freed subjects. Pinned in Task 5 (`publish after deinit is dropped`).
4. **Observer re-entry:** a subject observer that calls back into the manager (or into `AmsState`, then filament) during `refresh_subjects()` must not deadlock. Pinned in Task 5 (`an observer may call back into the manager during refresh`) and Task 13 (filament runout observer calling `set_role`).
5. **A Settings dropdown on a sensor holding a role that is not offered** shows the held role as text and does not change it until the user picks. Pinned in Task 17.

## File Map

| File | Responsibility | Task |
|---|---|---|
| `tests/unit/test_sensor_discovery_golden.cpp`, `tests/fixtures/sensor_discovery_golden/*.json` | Today's classification, subscriptions and manager entries per persona and for a kitchen-sink object list | 1 |
| `include/hardware_object_table.h`, `src/printer/hardware_object_table.cpp`, `tests/unit/test_hardware_object_table.cpp` | The one table | 2 |
| `include/printer_discovery.h`, `src/printer/printer_discovery_parse.cpp` | Store classes; `objects_of`; classified sections | 3 |
| `src/api/moonraker_discovery_sequence.cpp` (`parse_objects`, `build_subscription_objects`, configfile arm) | Read PD; subscribe by `status_fields` | 4 |
| `include/sensor_manager.h`, `src/sensors/sensor_manager.cpp`, `include/dynamic_subject_map.h`, `src/sensors/dynamic_subject_map.cpp`, `tests/unit/test_sensor_manager_base.cpp` | Base, registry, dynamic subjects | 5 |
| each manager's `.h/.cpp` and test | Migration | 6-14 |
| `include/sensor_collection.h`, `include/sensor_managers.h`, `tests/unit/test_sensor_collection.cpp` | Deleted | 14 |
| `src/printer/hardware_role_registry.cpp`, `printer_fan_state.cpp`, `led/led_controller.cpp`, `moonraker_performance_source.cpp`, `device_display_name.cpp`, `printer_hardware.cpp`, `hardware_validator.cpp`, `power_loss_sensor.*` | Other classifiers onto the table | 15 |
| `ui_xml/sensor_row.xml`, `ui_xml/sensors_overlay.xml`, `src/ui/ui_settings_sensors.cpp`, `include/ui_settings_sensors.h`, `src/ui/ui_wizard_filament_sensor_select.cpp` | One row, slots, overlay | 16-17 |
| thermistor and temp graph widgets + new XML components | Widget rows | 18 |
| `docs/devel/SENSOR_MANAGEMENT.md` | Rewritten; both plan files deleted | 19 |

---

### Task 1: Golden snapshot of today's discovery

**Files:**
- Create: `tests/unit/test_sensor_discovery_golden.cpp`, `tests/fixtures/sensor_discovery_golden/<persona>.json` (15), `tests/fixtures/sensor_discovery_golden/kitchen_sink.json`, `tests/fixtures/sensor_discovery_golden/kitchen_sink_objects.json`

**Interfaces:**
- Produces: `[sensor_golden]` tag; `HELIX_WRITE_GOLDENS=1` rewrites fixtures (same convention as `tests/unit/test_subscription_parity.cpp#check_against_golden`; reuse that helper by moving it to `tests/test_helpers/golden_json.h` rather than copying it).

- [ ] **Step 1: Write the snapshot test.** For each `helix::mock::kPersonas` entry (`helix::test::PersonaEnv` + `MoonrakerClientMock(type).hardware()`), and for a `PrinterDiscovery` fed `kitchen_sink_objects.json`, build one JSON document:
  - `buckets`: every `PrinterDiscovery` list accessor (`heaters`, `sensors`, `fans`, `leds`, `load_cells`, `width_sensor_objects`, `filament_sensor_names`, `printer_objects`) and every `has_*` sensor flag (`has_probe`, `has_accelerometer`, `has_filament_sensors`, `has_width_sensors`, `has_led`, `has_led_effects`, `has_fan_feedback`, `has_speaker`).
  - `discovery_sequence_buckets`: the lists `MoonrakerDiscoverySequence::parse_objects` builds (add a `MoonrakerDiscoverySequenceTestAccess` in `tests/test_helpers/` if they are not reachable).
  - `subscriptions`: `MoonrakerDiscoverySequence::build_subscription_objects` with those lists.
  - `managers`: for each sensor manager, after the same discovery calls `init_subsystems_from_hardware` makes (plus the configfile-arm calls for probe, accel, input when the mock provides `configfile`), each entry's `klipper_name`, `sensor_name`, `role` and `type` strings, `enabled`, and the manager's count.
  - `kitchen_sink_objects.json` contains one name per row of the design's classification table and every disagreement case: `output_pin exhaust_fan`, `output_pin fan_light`, `output_pin led_fan`, `output_pin beeper`, bare `neopixel`, bare `dotstar`, `neopixel strip`, `aht20_f heater_box1`, `bme280 enclosure`, `temperature_sensor extruder_board`, `heater_generic chamber_heater`, `temperature_fan exhaust`, `tmc2240 stepper_x`, `mcu`, `mcu toolhead`, `zmod_ifs_switch_sensor head`, `filament_switch_sensor runout`, `filament_motion_sensor encoder`, `hall_filament_width_sensor`, `load_cell scale`, `probe`, `bltouch`, `beacon`, `cartographer`, `probe_eddy_current btt`, `gcode_button door`, `power_loss_check`, `led_effect rainbow`, `fan_feedback`, `heater_fan hotend_fan`, `controller_fan board`, `fan_generic aux`, `fan`.
- [ ] **Step 2:** `HELIX_WRITE_GOLDENS=1 gmake t F='[sensor_golden]'`, then `gmake t F='[sensor_golden]'`. Expected: PASS. Hand-break: edit one value in `kitchen_sink.json`, see FAIL naming it, restore.
- [ ] **Step 3: Commit** `test(sensors): golden snapshot of today's discovery and subscriptions`.

Every later task keeps `[sensor_golden]` green. A task that changes a golden value regenerates it only for the rows its behavior fix predicts, and says which in its commit body.

---

### Task 2: The object table

**Files:**
- Create: `include/hardware_object_table.h` (exact content: Appendix A), `src/printer/hardware_object_table.cpp`, `tests/unit/test_hardware_object_table.cpp`; add the `.cpp` to `app_srcs.txt`.

**Interfaces:**
- Produces: everything in Appendix A. Tag `[hw_object_table]`.

- [ ] **Step 1: Failing tests**, one `TEST_CASE` per family, every row of the kitchen-sink list asserted, e.g.:
  - `classify_object("temperature_sensor chamber")` == `{TemperatureSensor, "chamber"}`; `classify_object("heater_generic chamber_heater").kind == GenericHeater`; `classify_object("extruder1").kind == Heater`; `classify_object("extruder1234").kind == Unknown` (≤3 digits rule from `include/klipper_extruder_naming.h`).
  - `output_pin` rule (design fix 2): `exhaust_fan` → `OutputPinFan`; `fan_light` → `OutputPinFan`; `led_fan` → `OutputPinFan`; `case_light` → `OutputPinLed`; `beeper` → `Speaker`; `relay` → `Unknown`. Tokens split on `_`, case-insensitive.
  - Bare `neopixel` and `dotstar` → `Led` (fix 3).
  - `classify_object("aht20_f heater_box1")` → `HumidityChip`, `chip_name(HumidityChip, chip) == "AHT20_F"` (copy display names from `humidity_sensor_chips()`).
  - Probe list (fix 4): `probe`, `bltouch`, `smart_effector`, `cartographer`, `beacon`, `probe_eddy_current x` → `Probe` from objects; `load_cell_probe`, `dockable_probe` → `Probe` from `classify_section`.
  - `classify_section("adxl345 bed", {})` → `Accelerometer`; `classify_section("resonance_tester", {{"accel_chip","beacon"}})` → `Accelerometer`; `classify_section("resonance_tester", {})` → `Unknown` (fix 5).
  - `zmod_ifs_switch_sensor head` → `BackendFilamentSensor`.
  - `status_fields(LoadCell) == {"force_g"}`; `status_fields(Input) == {"state"}`; `status_fields(PowerLossMonitor) == {"*"}`; `status_fields(Accelerometer).empty()`. Copy every list from today's `build_subscription_objects` so Task 4's golden matches.
- [ ] **Step 2:** `gmake t F='[hw_object_table]'` → FAIL (no symbols).
- [ ] **Step 3: Implement** as one ordered `constexpr` row array `{Match::Exact|Prefix|BareOrPrefix, pattern, kind, chip}` plus `output_pin_kind(name)`. Move the humidity chip table's data here; `include/humidity_sensor_types.h#humidity_chip_for_object` becomes a call into this module.
- [ ] **Step 4:** `gmake t F='[hw_object_table],[humidity],[sensor_golden]'` → PASS.
- [ ] **Step 5: Commit** `feat(hw): one table classifies every Klipper object and config section`.

---

### Task 3: PrinterDiscovery stores classes

**Files:** Modify `include/printer_discovery.h`, `src/printer/printer_discovery_parse.cpp` (`parse_objects`, `parse_config_keys`, `parse_probe_xy_offset`); test `tests/unit/test_printer_discovery.cpp`.

**Interfaces:**
- Produces on `helix::PrinterDiscovery`:
  - `std::vector<std::string> objects_of(std::initializer_list<hw::ObjectKind> kinds) const;` objects in discovery order.
  - `std::vector<std::string> sections_of(std::initializer_list<hw::ObjectKind> kinds) const;` classified config sections.
  - `const nlohmann::json* section(std::string_view name) const;` body of a recorded config section, or nullptr.
  - `std::vector<std::string> mcus() const;` (fix 7).
- Existing accessors keep their names and return types and become views over the stored classes.

- [ ] **Step 1: Failing tests** in `[printer_discovery]`: `objects_of({TemperatureSensor})`; `sections_of({Accelerometer})` after `parse_config_keys`; `has_accelerometer()` false for a bare `resonance_tester` (fix 5); `mcus()` lists `mcu` and `mcu toolhead`; `sensors()` now includes `aht20_f heater_box1` (fix 1); the probe XY offset reads `load_cell_probe`, `dockable_probe`, `cartographer`, `beacon` sections (fix 4).
- [ ] **Step 2:** run, FAIL. **Step 3:** implement: one `classify_object` call per name in `parse_objects`; the IFS and AMS detection branches stay where they are (they detect systems, not kinds). **Step 4:** `gmake t F='[printer_discovery],[hw_object_table],[sensor_golden]'`; regenerate only the golden rows fixes 1, 3, 4, 5, 7 predict. **Step 5: Commit** `refactor(discovery): PrinterDiscovery classifies through the object table`.

---

### Task 4: The discovery sequence reads PrinterDiscovery

**Files:** Modify `src/api/moonraker_discovery_sequence.cpp` (`parse_objects`, `build_subscription_objects`, `continue_discovery_objects` configfile arm only), `include/moonraker_discovery_sequence.h`; tests `tests/unit/test_subscription_parity.cpp`, `test_moonraker_subscription_fields.cpp`.

**Interfaces:**
- `build_subscription_objects(const PrinterDiscovery& hw)`: one argument. Each discovered object gets `hw::status_fields(kind)`; then each `SensorManager::extra_subscriptions(hw)`, and until Task 10 lands, `ProbeSensorManager::required_status_objects(hw)` as today.
- `parse_objects` keeps only `all_objects` bookkeeping and calls `hardware_.parse_objects`. The member bucket vectors go.

- [ ] **Step 1:** update the parity test's `Printer::build()` to the one-argument form; it must still match every existing golden except bare `neopixel`/`dotstar` (fix 3) and `output_pin` rows (fix 2). **Step 2:** FAIL to compile. **Step 3:** implement. **Step 4:** `gmake t F='[subscription_parity],[moonraker],[sensor_golden],[printer_discovery]'`; regenerate only fix-predicted rows. **Step 5: Commit** `refactor(discovery): subscriptions come from the object table`.

---

### Task 5: The SensorManager base and registry

**Files:**
- Create: `include/sensor_manager.h` (exact content: Appendix B), `src/sensors/sensor_manager.cpp`, `include/dynamic_subject_map.h`, `src/sensors/dynamic_subject_map.cpp`, `tests/unit/test_sensor_manager_base.cpp`, `tests/test_helpers/sensor_manager_test_access.h`, `tests/fixtures/sensor_settings/{filament,probe,width,input}.json` (copied from real saved configs: take them from `~/.helixscreen/settings.json` backups or the mock personas' saved output; keys exactly as today).
- Add both `.cpp` files to `app_srcs.txt`.

**Interfaces:**
- Produces: Appendix B; `sensor_managers()` returns migrated managers in this order: filament, humidity, width, probe, accel, temperature, load cell, input, `SensorState`. Until a manager is migrated, `include/sensor_managers.h#for_each_sensor_manager` keeps calling it; each migration task moves one manager from that list to the registry, and `status_dispatch.cpp` and `subject_initializer.cpp` walk both until Task 14.
- `DynamicSubjectMap`: `lv_subject_t* get_or_create(const std::string& key, int initial, SubjectLifetime& out_lifetime)`, `void remove_missing(const std::vector<std::string>& keep)`, `void clear()`; signals each subject's lifetime false before deinit (today's `TemperatureSensorManager::deinit_subjects` order).
- Tag `[sensor_manager_base]`.

- [ ] **Step 1: Failing tests** with a `TestSensorManager : SensorManager` (two roles: id 1 exclusive offered "chamber", id 2 shared not offered "aux"; one type "basic"; kinds `{TemperatureSensor}`; config key `test_sensors`):
  - `discover adopts objects of its kinds` (count subject == 2).
  - `reconcile drops a vanished sensor and keeps survivors` (role of survivor intact, count 1).
  - `set_role takes an exclusive role from the other holder`; `a shared role keeps both holders`.
  - `sync mode refreshes inline`; `deferred refresh runs on the queue`; `publish after deinit is dropped` (UpdateQueue drained after deinit: refresh count unchanged).
  - `refresh runs without the mutex held` (refresh_subjects asserts `mutex().try_lock()` succeeds from another thread).
  - `an observer may call back into the manager during refresh` (observer on revision subject calls `set_role`; no deadlock, completes).
  - `settings round trip is byte-identical` for each of the four fixtures (load then save equals input JSON).
  - `a wrong-typed saved field is skipped` (`"enabled": "yes"` leaves enabled true; next entry still read).
  - `offered_roles lists offered roles in RoleSpec order`; `row shows a held role that is not offered as held_role_label`.
  - `DynamicSubjectMap removes subjects for vanished keys and expires their lifetime`.
- [ ] **Step 2:** FAIL. **Step 3:** implement. `deinit_subjects`: lock, `lifetime_.invalidate()`, deinit subjects, clear flag; registers with `StaticSubjectRegistry` under `name()`. `update_from_status`: parse under lock, publish outside. `publish()` bumps the revision subject inside the deferred main-thread callback, before `refresh_subjects()`. **Step 4:** `gmake t F='[sensor_manager_base]'` → PASS; hand-break the exclusivity rule once. **Step 5: Commit** `feat(sensors): SensorManager base, registry and DynamicSubjectMap`.

---

### Tasks 6-13: Migrate each manager

Each task has the same shape. Do them in this order, one commit each: **6 width, 7 humidity, 8 accelerometer, 9 load cell, 10 probe, 11 input, 12 temperature, 13 filament.**

**Per-task steps:**
- [ ] **Step 1:** In the manager's existing test file, add: `saved config reads back` (persisted managers: load the Task 5 fixture, assert roles and extras, save, assert byte-identical) and one test per behavior the manager owns that the base now drives (auto roles in `on_discovered`, its status parsing). Update its `*TestAccess` helper to the new members; never drop an existing assertion.
- [ ] **Step 2:** FAIL.
- [ ] **Step 3:** Derive from `SensorManager`. Delete what the base now owns: mutex, `SensorCollection`, `sync_mode_`, lifetime guard, `init_subjects`/`deinit_subjects`, count subject, `set_sync_mode`, `has_sensors`/`get_sensors`/`sensor_count`, generic role/enable setters, `load_config_from_file`/`save_config_to_file`, `parse_klipper_name`. Keep the public getters real callers use (Task-specific list below), renamed only where the base provides the same thing. Move it from `for_each_sensor_manager` and from `init_subsystems_from_hardware` / `reload_sensor_roles_after_preset` to the registry loops (`for (auto* m : sensor_managers()) { m->discover(hw); m->load_settings(); }`, added in Task 6).
- [ ] **Step 4:** `gmake t F='<manager tags>,[sensor_manager_base],[sensor_golden]'` → PASS with no golden change except the task's listed ones.
- [ ] **Step 5: Commit** `refactor(sensors): <manager> derives SensorManager`.

**Task-specific decisions:**

| Task | Manager | Kinds | Config key | Roles (id, config_name, exclusive, offered) | Keeps / deletes |
|---|---|---|---|---|---|
| 6 | Width | `WidthSensor` | `width_sensors` | FLOW_COMPENSATION ("flow_compensation", excl, offered) | Keeps diameter subjects; `on_discovered` gives the first sensor FLOW_COMPENSATION when none holds it. Deletes `get_flow_compensation_diameter`, `get_sensor_state`, `is_sensor_available`. Tags `[width]` |
| 7 | Humidity | `HumidityChip` | none | CHAMBER, DRYER (excl, not offered) | Keeps `chamber_humidity`. Deletes `chamber_pressure`, `dryer_humidity` subjects and getters, role/enable setters, load/save config. Tags `[humidity]` |
| 8 | Accelerometer | `Accelerometer` (sections) | none | none | `update_from_status` default (no status). Deletes role/enable/config API, `discover_from_config`. Tags `[accel]` |
| 9 | Load cell | `LoadCell` | none | SPOOL_WEIGHT (excl, not offered) | Keeps the AmsState spool-weight push, now fired after the lock in `refresh_subjects`. Deletes stubs. Tags `[load_cell]` |
| 10 | Probe | `Probe` (objects + sections) | `probe_sensors` | Z_PROBE (excl, offered) | Keeps `probe_z_offset`, `set_probe_type_override`, `get_z_offset`, `status_object` and `publishes_last_query`; `required_status_objects` becomes `extra_subscriptions`; z_offset seeding reads `hw.section(...)` in `on_discovered`; single-probe Z_PROBE moves to `on_discovered` and still yields to a saved role. Deletes `probe_triggered`, `probe_last_z` subjects and getters, `get_last_z_result`, stale `ProbeLoadCellTestAccess` friend. Tags `[probe]` |
| 11 | Input | `Input` (objects + sections) | `input_sensors` | DOOR ("door", shared, offered) | Keeps door logic, `door_check`, mid-print toasts, `firmware_reacts_to_opening` (reactions read from `hw.section`). The 16 row slots and their subjects go: Settings rows come from `row()` in Task 17 (until then the overlay keeps working off the existing subjects; delete them in Task 17). Entry extra `inverted`; manager extra `door_check`. `set_option` = invert. Tags `[input_sensors]` |
| 12 | Temperature | `TemperatureSensor`, `TemperatureFan`, `DriverTemperature`, `GenericHeater`, `HumidityChip` | none | CHAMBER, MCU, HOST, STEPPER_DRIVER, AUXILIARY (not exclusive, not offered) | Per-sensor subjects through `DynamicSubjectMap`; keeps `get_sensors_sorted`, `get_temp_subject`, `get_target_subject`, `apply_chamber_sensor_override`. Excludes only real heaters (fix 6) and includes humidity chips (fix 1). Deletes role/enable/config API. `temperature_sensor_objects()` deleted. Tags `[temperature]` |
| 13 | Filament | `FilamentSwitch`, `FilamentMotion` | `filament_sensors` | RUNOUT, TOOLHEAD, ENTRY (offered; RUNOUT exclusive per lane via its `set_role` override, TOOLHEAD/ENTRY excl) | Overrides `update_from_status` (pre-lock AmsState read, post-unlock toasts and callback exactly as today) and `set_role`. `keep_missing_state = true`. Entry extra `lane` written only when set; manager extra `master_enabled`. Deletes `Z_PROBE` role, `probe_triggered` and `motion_active` subjects and getters, the hand-rolled reader. Adds a test: `a runout observer calling set_role during refresh does not deadlock`. Tags `[filament],[bypass-arming],[ams]` |

---

### Task 14: SensorState, then delete the old plumbing

**Files:** Modify `include/sensor_state.h`, `src/printer/sensor_state.cpp`, `tests/unit/test_sensor_state.cpp`. Delete `include/sensor_collection.h`, `tests/unit/test_sensor_collection.cpp`, `include/sensor_managers.h`; update `src/printer/status_dispatch.cpp`, `src/application/subject_initializer.cpp`, `src/system/telemetry_manager.cpp` (counts from the registry).

**Interfaces:** `SensorState` derives `SensorManager` with `from_printer_discovery = false`, no roles, no config key; `set_sensors(list, initial)` calls `reconcile()` then `publish()`; per-(id, key) subjects through `DynamicSubjectMap`; `deinit` no longer drains `UpdateQueue` (the base's lifetime guard drops stale work).

- [ ] Steps: failing test `notify after deinit is dropped` and `reconnect with fewer sensors removes their subjects`; FAIL; implement; `gmake t F='[sensor_state],[sensor_manager_base],[sensor_golden]'`; commit `refactor(sensors): SensorState derives SensorManager; one registry`.

---

### Task 15: Every other classifier onto the table

**Files:** `src/printer/hardware_role_registry.cpp`, `src/printer/printer_hardware.cpp` (`guess_*`), `src/printer/printer_fan_state.cpp`, `src/led/led_controller.cpp`, `src/system/moonraker_performance_source.cpp`, `src/device_display_name.cpp`, `src/printer/hardware_validator.cpp`, `include/power_loss_sensor.h`, `src/printer/power_loss_sensor.cpp`, `src/system/debug_bundle_collector.cpp`; tests `test_hardware_role_registry.cpp`, `test_power_loss_sensor.cpp`, fan/LED tests.

**Interfaces:** `is_*_candidate` predicates take `hw::classify_object(name)` first, then their keyword rule. Each `guess_*` chooses only among names its role's `is_candidate` accepts (fix 8). `power_loss` keeps `required_status_objects(hw)` (now `hw.objects_of({PowerLossMonitor})`); deletes `firmware_reports_power_loss`, `power_loss_asserted` and their tests. The validator reads configured filament sensors through `FilamentSensorManager::entries()` plus its saved settings, not raw `"name"` keys (fix 9).

- [ ] Steps: failing tests `guess never returns a non-candidate` (`bed_fans`, `heat_fan`, `e0` cases from the design) and `a configured filament sensor that disappeared is flagged`; FAIL; implement; `gmake t F='[hwrole],[hardware],[hwvalidate],[power_loss],[fan],[led],[sensor_golden]'`; commit `refactor(hw): role registry, fans, LEDs and MCUs classify through the table`.

---

### Task 16: The shared Settings row component

**Files:** Create `ui_xml/sensor_row.xml`; test `tests/unit/test_sensor_row_xml.cpp`.

**Interfaces:**
- Component props: `section` (string, e.g. `filament`, the manager's `name()` slug), `index` (string), `role_options` (string, newline-separated translated labels, default empty), `icon` (string).
- Subjects, all `sensor_<section>_row_<field>_${index}`: `name` (string), `detail` (string), `state` (string), `has_state` (int, hides state), `alert` (int, warning style), `role` (int, `bind_selected`), `has_role` (int, hides the dropdown), `toggle_kind` (int: 0 none, 1 Enabled, 2 Invert; the label text switches by `bind_flag` pairs), `toggle` (int, `bind_state_if_eq checked`).
- No subject or C++ sets dropdown options (helix-xml has no options binding): each section passes its static `role_options` from `sensors_overlay.xml`, in `offered_roles()` order, with `options_tag` for translation as `ui_xml/input_sensor_row.xml` does.
- Callbacks: `on_sensor_row_role_changed`, `on_sensor_row_toggle_changed`; user_data `<section>:${index}`.
- Icon and layout follow `ui_xml/input_sensor_row.xml`.

- [ ] Steps: binding tests per subject (name text, state hidden when `has_state` 0, warning style on alert, dropdown hidden when `has_role` 0, toggle label per kind); FAIL; implement; `gmake t F='[sensor_row]'`; `make lint-xml` (`scripts/qc/xml_linter.sh`); commit `feat(ui): one sensor_row component for every sensor kind`.

---

### Task 17: Settings > Sensors on the shared row

**Files:** `src/ui/ui_settings_sensors.cpp`, `include/ui_settings_sensors.h`, `ui_xml/sensors_overlay.xml`, `src/ui/ui_wizard_filament_sensor_select.cpp`, `include/printer_hardware.h` (one `is_standalone_filament_sensor(const PrinterDiscovery&, const std::string&)`); delete `ui_xml/{filament,width,load_cell,input,probe,humidity,accel}_sensor_row.xml` (`load_cell_row.xml` too) and their references in `tests/unit/test_job_holds_machine.cpp`; delete Input's row-slot subjects (Task 11 note). Tests: `test_sensor_settings_overlay.cpp`, `test_wizard_filament_sensor_select.cpp`, `test_input_settings_manager.cpp`.

**Interfaces:**
- `class SensorRowSection` (in `ui_settings_sensors.cpp`; one per registered manager with a Settings section): owns that section's per-row subjects (the Task 16 names) plus `sensor_<section>_row_count` (int); `void fill(const SensorManager&)` grows the subject set to the manager's `sensor_count()` before setting the row count (subjects are never shrunk while the overlay lives, so a rebuild never binds a dead subject), then writes each row from `row(i)`. Subjects live with the overlay's `SubjectManager` and deinit with it.
- Each section in `sensors_overlay.xml`: header and badge bound to the manager's count subject (no `ui_status_pill_set_text`), then `<repeat count="sensor_<section>_row_count"><sensor_row section="<section>" index="${i}" role_options="..."/></repeat>` (rule 8). The "no sensors" placeholder binds a `sensor_total_count` int the overlay owns (fix 12).
- A test asserts each section's `role_options` labels equal the translated `label_key`s of that manager's `offered_roles()`, in order.
- Live rows: the overlay observes each manager's `revision_subject()` (with `subjects_lifetime()`) and calls that section's `fill`.
- Role edit: option index → `offered_roles()[index]` → `set_role` → `save_settings()` (fix 10). Toggle: Enabled → `set_enabled`, Invert → `set_option`.
- Chamber assignment dropdowns unchanged.

- [ ] Steps: failing tests `every manager with sensors gets a section`, `section role_options match offered_roles`, `editing a role saves through RoleSpec, not enum order`, `a held role that is not offered shows as text and stays until changed` (Review Focus 5), `input door state updates live while the overlay is open`, `the no-sensors placeholder shows when every count is 0`, `the wizard and overlay agree on standalone sensors`; FAIL; implement; `gmake t F='[sensors],[wizard],[input_sensors],[sensor_row]'`; commit `refactor(ui): Settings > Sensors renders every manager through sensor_row`.

---

### Task 18: Widget rows

**Files:** `src/ui/panel_widgets/thermistor_widget.cpp` (+ `.h`), `src/ui/panel_widgets/temp_graph_widget.cpp`, new `ui_xml/components/thermistor_picker_row.xml`, `ui_xml/components/temp_graph_series_row.xml`; tests in their existing widget test files.

**Interfaces:** both widgets read rows through `TemperatureSensorManager::row(i)` (or `entries()` plus their own value formatting) and bind to their own components' subjects; no `lv_obj_create` rows, no `lv_obj_add_event_cb` (XML `event_cb` with the row index as user_data). Behavior and look unchanged.

- [ ] Steps: failing binding tests for each component; FAIL; implement; `gmake t F='[thermistor],[temp_graph]'`; commit `refactor(ui): thermistor and temp graph rows are declarative`.

---

### Task 19: Documentation

**Files:** Rewrite `docs/devel/SENSOR_MANAGEMENT.md` for the new shape (overview, key files, the table, the base and its hooks, threading rule, persistence shape, Settings rows, "adding a sensor": one table row; or one manager class + one registry line). Update `docs/devel/CLAUDE.md` and `docs/devel/architecture/` references to `for_each_sensor_manager` / `SensorCollection`. Delete `docs/devel/plans/2026-10-27-sensor-architecture-design.md` and this plan.

- [ ] Steps: `make check-doc-anchors` advisory clean for the rewritten doc; commit `docs(sensors): sensor management after the architecture cleanup`.

---

### Finish (orchestrator, once)

1. One review of the whole branch (fork Opus).
2. `make mutate-diff MUTATE_ARGS="--tests '[sensor_golden],[hw_object_table],[sensor_manager_base],[width],[humidity],[accel],[load_cell],[probe],[input_sensors],[temperature],[filament],[bypass-arming],[sensor_state],[hwrole],[sensors],[sensor_row],[thermistor],[temp_graph],[subscription_parity]'"`.
3. Screenshots of every Settings > Sensors section and both widgets, before (main) and after, shown to Preston.
4. ESP32 image size before/after through helixscreen-ae's thelio queue; report to helixscreen-29.
5. One `make land-gate`.

---

## Appendix A: `include/hardware_object_table.h`

Create with exactly the content of the header syntax-checked for this plan (kept beside the plan at `docs/devel/plans/2026-10-27-sensor-architecture/hardware_object_table.h`; copy it to `include/` in Task 2).

## Appendix B: `include/sensor_manager.h`

Same: `docs/devel/plans/2026-10-27-sensor-architecture/sensor_manager.h`, copied to `include/` in Task 5. Both passed `scripts/syntax_check.py` with project flags before execution.
