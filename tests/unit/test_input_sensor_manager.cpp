// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_sensor_manager.cpp
 * @brief InputSensorManager: discovery, default roles, state parsing, role
 *        persistence, the door-check default and the overlay row subjects.
 *
 * Run with: ./build/bin/helix-tests "[input_sensors]"
 */

#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "app_globals.h"
#include "config.h"
#include "input_sensor_manager.h"
#include "input_sensor_types.h"
#include "printer_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;
using json = nlohmann::json;

namespace helix::sensors {
class InputSensorManagerTestAccess {
  public:
    static void reset(InputSensorManager& obj) {
        std::lock_guard<std::recursive_mutex> lock(obj.mutex_);
        obj.sensors_.clear();
        obj.door_check_.reset();
        obj.config_reactions_.clear();
        obj.open_doors_seen_.clear();
        obj.sync_mode_ = true;
        obj.update_subjects();
    }
};
} // namespace helix::sensors

namespace {

const std::vector<std::string> kC5Objects = {
    "extruder",
    "gcode_button frontDoor",
    "gcode_button topDoor",
    "gcode_button extruder_pos1",
    "gcode_button extruder_grab1",
    "filament_switch_sensor runout",
};

class InputSensorFixture : public LVGLTestFixture {
  public:
    InputSensorFixture() {
        mgr().init_subjects();
        InputSensorManagerTestAccess::reset(mgr());
        config_slot() = json::object();
    }
    ~InputSensorFixture() override {
        InputSensorManagerTestAccess::reset(mgr());
        config_slot() = json::object();
    }

  protected:
    static InputSensorManager& mgr() {
        return InputSensorManager::instance();
    }
    static json& config_slot() {
        return Config::get_instance()->get_json(Config::get_instance()->df() + "input_sensors");
    }
    static void report(const std::string& name, const char* state) {
        mgr().update_from_status(json{{name, {{"state", state}}}});
    }
    static std::string subject_string(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }
    static int subject_int(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
};

} // namespace

TEST_CASE("default_input_role: door, lid and cover names are doors", "[input_sensors]") {
    CHECK(default_input_role("frontDoor") == InputSensorRole::DOOR);
    CHECK(default_input_role("TOP_LID") == InputSensorRole::DOOR);
    CHECK(default_input_role("cover_switch") == InputSensorRole::DOOR);
    CHECK(default_input_role("extruder_grab1") == InputSensorRole::NONE);
    CHECK(default_input_role("motor_a") == InputSensorRole::NONE);
}

TEST_CASE("input_reads_open: a door reads open when released, unless inverted", "[input_sensors]") {
    InputSensorConfig door("gcode_button d", "d", InputSensorType::GCODE_BUTTON);
    door.role = InputSensorRole::DOOR;
    InputSensorState state;
    state.reported = true;

    state.pressed = true;
    CHECK_FALSE(input_reads_open(door, state));
    state.pressed = false;
    CHECK(input_reads_open(door, state));

    door.inverted = true;
    CHECK_FALSE(input_reads_open(door, state));
    state.pressed = true;
    CHECK(input_reads_open(door, state));

    // No state yet, or not a door: never open.
    state.reported = false;
    CHECK_FALSE(input_reads_open(door, state));
    state.reported = true;
    door.role = InputSensorRole::NONE;
    CHECK_FALSE(input_reads_open(door, state));
}

TEST_CASE("InputSensorManager::inputs_in finds every gcode_button with default roles",
          "[input_sensors]") {
    const auto inputs = InputSensorManager::inputs_in(kC5Objects);
    REQUIRE(inputs.size() == 4);
    CHECK(inputs[0].sensor_name == "frontDoor");
    CHECK(inputs[0].role == InputSensorRole::DOOR);
    CHECK(inputs[1].role == InputSensorRole::DOOR);
    CHECK(inputs[2].sensor_name == "extruder_pos1");
    CHECK(inputs[2].role == InputSensorRole::NONE);
    CHECK(inputs[3].role == InputSensorRole::NONE);

    const auto subs = InputSensorManager::required_status_objects(kC5Objects);
    CHECK(subs.size() == 4);
    CHECK(subs["gcode_button topDoor"] == json::array({"state"}));
    CHECK_FALSE(subs.contains("filament_switch_sensor runout"));
    // A bare prefix with no name is not an input.
    CHECK(InputSensorManager::inputs_in({"gcode_button "}).empty());
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager parses PRESSED/RELEASED into open doors",
                 "[input_sensors]") {
    mgr().discover(kC5Objects);
    // Nothing reported yet: no door reads open.
    CHECK(mgr().open_doors().empty());

    report("gcode_button frontDoor", "PRESSED");
    report("gcode_button topDoor", "RELEASED");
    CHECK(mgr().open_doors() == std::vector<std::string>{"topDoor"});

    // An unknown state word or a non-string is ignored, keeping the last state.
    report("gcode_button topDoor", "WEIRD");
    mgr().update_from_status(json{{"gcode_button topDoor", {{"state", nullptr}}}});
    CHECK(mgr().open_doors() == std::vector<std::string>{"topDoor"});

    report("gcode_button topDoor", "PRESSED");
    CHECK(mgr().open_doors().empty());
    // An unknown word does not read as released either.
    report("gcode_button topDoor", "WEIRD");
    CHECK(mgr().open_doors().empty());

    // A released non-door input is never an open door.
    report("gcode_button extruder_grab1", "RELEASED");
    CHECK(mgr().open_doors().empty());
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager door check defaults to having a door",
                 "[input_sensors]") {
    mgr().discover({"gcode_button extruder_grab1"});
    CHECK_FALSE(mgr().has_door_input());
    CHECK_FALSE(mgr().door_check_enabled());

    mgr().discover(kC5Objects);
    CHECK(mgr().has_door_input());
    CHECK(mgr().door_check_enabled());
    CHECK(subject_int("input_door_check") == 1);

    mgr().set_door_check_enabled(false);
    CHECK_FALSE(mgr().door_check_enabled());
    CHECK(subject_int("input_door_check") == 0);
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager persists roles, invert and door check",
                 "[input_sensors]") {
    mgr().discover(kC5Objects);
    mgr().set_sensor_role("gcode_button topDoor", InputSensorRole::NONE);
    mgr().set_sensor_role("gcode_button extruder_pos1", InputSensorRole::DOOR);
    mgr().set_sensor_inverted("gcode_button frontDoor", true);
    mgr().set_door_check_enabled(false);
    config_slot() = mgr().save_config();

    // A fresh discovery restores the name defaults; the saved file overrides them.
    InputSensorManagerTestAccess::reset(mgr());
    mgr().discover(kC5Objects);
    CHECK(mgr().get_sensors()[1].role == InputSensorRole::DOOR);
    CHECK_FALSE(mgr().get_sensors()[0].inverted);
    CHECK(mgr().door_check_enabled());

    mgr().load_config_from_file();
    const auto sensors = mgr().get_sensors();
    CHECK(sensors[0].inverted);
    CHECK(sensors[1].role == InputSensorRole::NONE);
    CHECK(sensors[2].role == InputSensorRole::DOOR);
    CHECK_FALSE(mgr().door_check_enabled());

    // No saved door_check: back to the derived default.
    config_slot().erase("door_check");
    mgr().load_config_from_file();
    CHECK(mgr().door_check_enabled());
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager fills the overlay row subjects",
                 "[input_sensors]") {
    mgr().discover(kC5Objects);
    CHECK(subject_int("input_sensor_count") == 4);
    CHECK(subject_string("input_sensor_name_0") == "frontDoor");
    CHECK(subject_int("input_sensor_role_0") == 1);
    CHECK(subject_int("input_sensor_role_2") == 0);
    CHECK(subject_string("input_sensor_state_0") == "--");

    report("gcode_button frontDoor", "RELEASED");
    report("gcode_button extruder_grab1", "PRESSED");
    CHECK(subject_string("input_sensor_state_0") == "Opened");
    CHECK(subject_int("input_sensor_alert_0") == 1);
    CHECK(subject_string("input_sensor_state_3") == "Pressed");
    CHECK(subject_int("input_sensor_alert_3") == 0);

    mgr().set_sensor_inverted("gcode_button frontDoor", true);
    CHECK(subject_string("input_sensor_state_0") == "Closed");
    CHECK(subject_int("input_sensor_alert_0") == 0);
    CHECK(subject_int("input_sensor_inverted_0") == 1);

    CHECK(mgr().klipper_name_at(1) == "gcode_button topDoor");
    CHECK(mgr().klipper_name_at(4).empty());
}

TEST_CASE("doors_newly_open: only doors absent before are owed a warning", "[input_sensors]") {
    CHECK(doors_newly_open({}, {"frontDoor"}) == std::vector<std::string>{"frontDoor"});
    CHECK(doors_newly_open({"frontDoor"}, {"frontDoor"}).empty());
    CHECK(doors_newly_open({"frontDoor"}, {"frontDoor", "topDoor"}) ==
          std::vector<std::string>{"topDoor"});
    CHECK(doors_newly_open({"frontDoor", "topDoor"}, {}).empty());
}

TEST_CASE("input_config_reaction: an edge counts only with non-blank gcode", "[input_sensors]") {
    const auto r = input_config_reaction(
        json{{"press_gcode", "  \n\t "}, {"release_gcode", "\nRESPOND MSG=open"}});
    CHECK_FALSE(r.on_press);
    CHECK(r.on_release);

    const auto none = input_config_reaction(json{{"pin", "PB0"}});
    CHECK_FALSE(none.on_press);
    CHECK_FALSE(none.on_release);
    // A non-string value is not gcode.
    CHECK_FALSE(input_config_reaction(json{{"release_gcode", 1}}).on_release);
}

TEST_CASE("firmware_reacts_to_opening: a door opens on release, or on press when inverted",
          "[input_sensors]") {
    const InputConfigReaction on_release{false, true};
    const InputConfigReaction on_press{true, false};
    CHECK(firmware_reacts_to_opening(on_release, false));
    CHECK_FALSE(firmware_reacts_to_opening(on_release, true));
    CHECK(firmware_reacts_to_opening(on_press, true));
    CHECK_FALSE(firmware_reacts_to_opening(on_press, false));
}

TEST_CASE_METHOD(InputSensorFixture, "InputSensorManager reads which doors the printer announces",
                 "[input_sensors]") {
    mgr().discover(kC5Objects);
    mgr().discover_from_config(json{
        {"gcode_button frontDoor", {{"release_gcode", "\nRESPOND MSG=open"}}},
        {"gcode_button topDoor", {{"release_gcode", ""}, {"press_gcode", "M117 closed"}}},
        {"extruder", {{"release_gcode", "ignored: not a button"}}},
    });
    CHECK(mgr().firmware_reacts_to_opening("gcode_button frontDoor"));
    CHECK_FALSE(mgr().firmware_reacts_to_opening("gcode_button topDoor"));
    // No config section at all: the printer says nothing.
    CHECK_FALSE(mgr().firmware_reacts_to_opening("gcode_button extruder_grab1"));

    // Inverted, the top door opens on press, which its config does act on.
    mgr().set_sensor_inverted("gcode_button topDoor", true);
    CHECK(mgr().firmware_reacts_to_opening("gcode_button topDoor"));
    mgr().set_sensor_inverted("gcode_button frontDoor", true);
    CHECK_FALSE(mgr().firmware_reacts_to_opening("gcode_button frontDoor"));

    // A later config read replaces the earlier one.
    mgr().discover_from_config(json::object());
    CHECK_FALSE(mgr().firmware_reacts_to_opening("gcode_button topDoor"));
}

TEST_CASE_METHOD(InputSensorFixture,
                 "InputSensorManager warns mid-print only for enclosure materials",
                 "[input_sensors]") {
    mgr().discover(kC5Objects);
    const std::vector<std::string> opened = {"gcode_button frontDoor"};

    // A material that needs a warm chamber: warned, by short name.
    CHECK(mgr().mid_print_door_warnings(opened, {"PLA", "ASA"}) ==
          std::vector<std::string>{"frontDoor"});
    // PLA alone wants air: quiet. Unknown materials (empty) stay quiet too.
    CHECK(mgr().mid_print_door_warnings(opened, {"PLA"}).empty());
    CHECK(mgr().mid_print_door_warnings(opened, {}).empty());
    // Nothing newly open, or the check off: quiet.
    CHECK(mgr().mid_print_door_warnings({}, {"ABS"}).empty());
    mgr().set_door_check_enabled(false);
    CHECK(mgr().mid_print_door_warnings(opened, {"ABS"}).empty());
    mgr().set_door_check_enabled(true);

    // The printer announcing the door itself silences only that door.
    mgr().discover_from_config(
        json{{"gcode_button frontDoor", {{"release_gcode", "RESPOND MSG=open"}}}});
    CHECK(mgr().mid_print_door_warnings({"gcode_button frontDoor", "gcode_button topDoor"},
                                        {"ABS"}) == std::vector<std::string>{"topDoor"});
}

namespace {
/// PrinterState's print lifecycle needs the app's subjects, which only the UI
/// fixture initialises.
class InputSensorPrintFixture : public LVGLUITestFixture {
  public:
    InputSensorPrintFixture() {
        InputSensorManager::instance().init_subjects();
        InputSensorManagerTestAccess::reset(InputSensorManager::instance());
    }
    ~InputSensorPrintFixture() override {
        InputSensorManagerTestAccess::reset(InputSensorManager::instance());
    }
};
} // namespace

TEST_CASE_METHOD(
    InputSensorPrintFixture,
    "InputSensorManager toasts a door opened mid-print and not a role or invert change",
    "[input_sensors][door]") {
    auto& ps = get_printer_state();
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();
    ps.print_state().set_print_materials({"ABS"});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ps.print_state().print_materials() == std::vector<std::string>{"ABS"});
    struct Restore {
        ~Restore() {
            auto& st = get_printer_state();
            st.print_state().set_print_materials({});
            helix::test::set_wire_state(st, PrintJobState::STANDBY);
            helix::ui::UpdateQueue::instance().drain();
            ToastManager::instance().set_observer({});
        }
    } restore;
    REQUIRE(ps.print_state().get_print_lifecycle() == PrintState::Printing);

    std::vector<std::string> toasts;
    ToastManager::instance().set_observer(
        [&toasts](const ToastManager::Event& e) { toasts.push_back(e.message); });

    InputSensorManager::instance().discover(kC5Objects);
    InputSensorManager::instance().update_from_status(
        json{{"gcode_button frontDoor", {{"state", "PRESSED"}}},
             {"gcode_button extruder_pos1", {{"state", "RELEASED"}}}});
    REQUIRE(toasts.empty());

    // A released input set to Door now reads open, but nothing moved.
    InputSensorManager::instance().set_sensor_role("gcode_button extruder_pos1",
                                                   InputSensorRole::DOOR);
    // Inverting a closed door makes it read open, and nothing moved either.
    InputSensorManager::instance().set_sensor_inverted("gcode_button frontDoor", true);
    CHECK(toasts.empty());

    // The front door really opening is what warns.
    InputSensorManager::instance().set_sensor_inverted("gcode_button frontDoor", false);
    InputSensorManager::instance().update_from_status(
        json{{"gcode_button frontDoor", {{"state", "RELEASED"}}}});
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].find("frontDoor") != std::string::npos);
}
