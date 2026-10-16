// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_op_heater.cpp
 * @brief The nozzle heat before a runout dialog's filament op: FilamentOpHeater,
 *        and the two dialogs that go through it.
 *
 * Run with: ./build/bin/helix-tests "[filament][preheat][heater]"
 *
 * The pause-time runout dialog (FilamentRunoutHandler) and the idle one
 * (PrintStatusWidget) close the moment a button is pressed, so these drive their
 * dispatch with a real TemperatureController and read the gcode Moonraker
 * receives: the heat command, then the filament macro, in that order.
 */

#include "ui_filament_runout_handler.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_runout_handler_test_access.h"
#include "../test_helpers/print_status_widget_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_backend_qidi.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_op_heater.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "post_op_cooldown_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "standard_macros.h"
#include "temperature_controller.h"

#include <chrono>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::PrintStatusWidget;
using helix::PrintStatusWidgetTestAccess;
using helix::ui::decide_preheat;
using helix::ui::FilamentOpHeater;
using helix::ui::FilamentRunoutHandler;
using helix::ui::FilamentRunoutHandlerTestAccess;
using helix::ui::PreheatDecision;
using helix::ui::PreheatInputs;

// ============================================================================
// decide_preheat(): the rule, with no printer
// ============================================================================

namespace {

PreheatInputs nozzle(int current, int target, std::optional<int> material = std::nullopt,
                     int held = 0) {
    PreheatInputs in;
    in.current_c = current;
    in.target_c = target;
    in.material_c = material;
    in.held_c = held;
    in.floor_c = 170;
    in.ceiling_c = 300;
    return in;
}

} // namespace

TEST_CASE("decide_preheat: a cold nozzle heats to the material", "[filament][preheat][heater]") {
    const PreheatDecision d = decide_preheat(nozzle(25, 0, 240));
    CHECK(d.heat);
    CHECK(d.send_c == 240);
    CHECK(d.wait_c == 240);
}

TEST_CASE("decide_preheat: with no material named it heats to the default load temperature",
          "[filament][preheat][heater]") {
    const PreheatDecision d = decide_preheat(nozzle(25, 0));
    CHECK(d.heat);
    CHECK(d.send_c == 220);
    CHECK(d.wait_c == 220);
}

TEST_CASE("decide_preheat: a reached live target counts only where the op honours it",
          "[filament][preheat][heater]") {
    PreheatInputs honoured = nozzle(210, 210, 240);
    honoured.honor_live_target = true;
    CHECK_FALSE(decide_preheat(honoured).heat);
    honoured.current_c = 206; // within the margin
    CHECK_FALSE(decide_preheat(honoured).heat);
    honoured.current_c = 150; // still heating toward it is not reached
    CHECK(decide_preheat(honoured).heat);

    // A load while idle heats to the material: a nozzle left at the last
    // material's temperature is no reason to load PETG at PLA's.
    const PreheatDecision load = decide_preheat(nozzle(210, 210, 240));
    CHECK(load.heat);
    CHECK(load.wait_c == 240);
}

TEST_CASE("preheat_reached: within the margin, never below the extrusion floor",
          "[filament][preheat][heater]") {
    CHECK(helix::ui::preheat_reached(216, 220, 170));
    CHECK_FALSE(helix::ui::preheat_reached(214, 220, 170));
    CHECK_FALSE(helix::ui::preheat_reached(166, 170, 170));
    CHECK(helix::ui::preheat_reached(170, 170, 170));
}

TEST_CASE("decide_preheat: a held target below the extrusion floor is not hot enough",
          "[filament][preheat][heater]") {
    PreheatInputs in = nozzle(150, 150, 210);
    in.honor_live_target = true;
    const PreheatDecision d = decide_preheat(in);
    CHECK(d.heat);
    CHECK(d.send_c == 210);
}

TEST_CASE("decide_preheat: the swap latch raises both the bar and the wait",
          "[filament][preheat][heater]") {
    const PreheatDecision d = decide_preheat(nozzle(215, 0, 210, /*held=*/250));
    CHECK(d.heat);
    CHECK(d.send_c == 210); // the controller's keep_previous_hot holds it at 250
    CHECK(d.wait_c == 250);
}

TEST_CASE("decide_preheat: targets stay between the floor and the hotend's maximum",
          "[filament][preheat][heater]") {
    const PreheatDecision hot = decide_preheat(nozzle(25, 0, 350));
    CHECK(hot.send_c == 300);
    CHECK(hot.wait_c == 300);
    const PreheatDecision cool = decide_preheat(nozzle(25, 0, 150));
    CHECK(cool.send_c == 170);
    CHECK(cool.wait_c == 170);
}

// ============================================================================
// preheat_skip_reason(): who heats for the op
// ============================================================================

namespace {

/// A backend whose heat claims the test sets.
class ClaimingBackend : public AmsBackendMock {
  public:
    ClaimingBackend() : AmsBackendMock(4) {}
    bool load_heats = false;
    bool unload_heats = false;
    [[nodiscard]] bool supports_auto_heat_on_load() const override {
        return load_heats;
    }
    [[nodiscard]] bool unload_heats_hotend(int /*slot_index*/) const override {
        return unload_heats;
    }
};

/// The cold-extrude setting, live (its subject initialised) and put back after.
class SkipFixture : public LVGLTestFixture {
  public:
    SkipFixture() {
        helix::SafetySettingsManager::instance().init_subjects();
        was_ = helix::SafetySettingsManager::instance().get_allow_cold_extrude();
    }
    ~SkipFixture() override {
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(was_);
    }
    static void allow_cold_extrude(bool allow) {
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(allow);
        REQUIRE(helix::SafetySettingsManager::instance().get_allow_cold_extrude() == allow);
    }

  private:
    bool was_ = false;
};

} // namespace

TEST_CASE_METHOD(SkipFixture,
                 "preheat_skip_reason: only a backend that heats for that op skips the preheat",
                 "[filament][preheat][heater][skip]") {
    using helix::ui::AmsCall;
    using helix::ui::FilamentHeatOp;
    using helix::ui::FilamentOpPlan;
    using helix::ui::FilamentTier;
    using helix::ui::PreheatSkip;
    allow_cold_extrude(false);
    ClaimingBackend backend;

    const FilamentOpPlan load{FilamentTier::AmsBackend, {}, AmsCall::Load, 0};
    const FilamentOpPlan swap{FilamentTier::AmsBackend, {}, AmsCall::ChangeTool, 0};
    const FilamentOpPlan unload{FilamentTier::AmsBackend, {}, AmsCall::Unload, 0};
    const FilamentOpPlan macro{FilamentTier::Macro, {}, AmsCall::None, -1};

    CHECK(helix::ui::preheat_skip_reason(FilamentHeatOp::Load, &load, &backend) ==
          PreheatSkip::None);
    CHECK(helix::ui::preheat_skip_reason(FilamentHeatOp::Unload, &unload, &backend) ==
          PreheatSkip::None);

    backend.load_heats = true;
    CHECK(helix::ui::preheat_skip_reason(FilamentHeatOp::Load, &load, &backend) ==
          PreheatSkip::BackendSelfHeats);
    CHECK(helix::ui::preheat_skip_reason(FilamentHeatOp::Load, &swap, &backend) ==
          PreheatSkip::BackendSelfHeats);
    // A claim about loads says nothing about unloads.
    CHECK(helix::ui::preheat_skip_reason(FilamentHeatOp::Unload, &unload, &backend) ==
          PreheatSkip::None);

    backend.unload_heats = true;
    CHECK(helix::ui::preheat_skip_reason(FilamentHeatOp::Unload, &unload, &backend) ==
          PreheatSkip::BackendSelfHeats);
    // Nothing tells us a macro heats, whatever the backend claims.
    CHECK(helix::ui::preheat_skip_reason(FilamentHeatOp::Load, &macro, &backend) ==
          PreheatSkip::None);
    CHECK(helix::ui::preheat_skip_reason(FilamentHeatOp::Purge, nullptr, &backend) ==
          PreheatSkip::None);
}

TEST_CASE_METHOD(SkipFixture,
                 "preheat_skip_reason: the allow-cold-extrude setting skips every tier",
                 "[filament][preheat][heater][skip]") {
    using helix::ui::FilamentHeatOp;
    using helix::ui::PreheatSkip;
    allow_cold_extrude(true);
    const helix::ui::FilamentOpPlan macro{helix::ui::FilamentTier::Macro, {}, {}, -1};
    CHECK(helix::ui::preheat_skip_reason(FilamentHeatOp::Load, &macro, nullptr) ==
          PreheatSkip::UserOverride);
    CHECK(helix::ui::preheat_skip_reason(FilamentHeatOp::Purge, nullptr, nullptr) ==
          PreheatSkip::UserOverride);
}

TEST_CASE("unload_heats_hotend: only backends whose unload command heats claim it",
          "[filament][preheat][heater][skip]") {
    // Each true is cited where the backend overrides it; a wrong true runs a
    // cold extrude, so everything else must keep asking for our preheat.
    CHECK(AmsBackendAfc(nullptr, nullptr).unload_heats_hotend(0));
    CHECK(AmsBackendQidi(nullptr, nullptr).unload_heats_hotend(0));
    CHECK(AmsBackendAd5xIfs(nullptr, nullptr).unload_heats_hotend(0));
    CHECK(AmsBackendToolChanger(nullptr, nullptr).unload_heats_hotend(0));
    CHECK_FALSE(AmsBackendHappyHare(nullptr, nullptr).unload_heats_hotend(0));
    CHECK_FALSE(AmsBackendAce(nullptr, nullptr).unload_heats_hotend(0));
    CHECK_FALSE(helix::printer::AmsBackendCfs(nullptr, nullptr).unload_heats_hotend(0));
}

// ============================================================================
// The dialogs, end to end
// ============================================================================

namespace {

constexpr const char* HEAT_GCODE = "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=";

bool s_widget_subjects_ready = false;

/// Registers @p T as the app's shared resource for one test, and clears it after.
template <typename T> class ScopedResource {
  public:
    explicit ScopedResource(std::unique_ptr<T> owned) : owned_(std::move(owned)) {
        helix::PanelWidgetManager::instance().register_shared_resource<T>(owned_.get());
    }
    ~ScopedResource() {
        helix::PanelWidgetManager::instance().register_shared_resource<T>(static_cast<T*>(nullptr));
    }
    T& get() {
        return *owned_;
    }
    T* ptr() {
        return owned_.get();
    }

  private:
    std::unique_ptr<T> owned_;
};

class HeaterFixture : public LVGLTestFixture {
  public:
    HeaterFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        if (!s_widget_subjects_ready) {
            helix::PanelWidgetManager::instance().init_widget_subjects();
            s_widget_subjects_ready = true;
        }
        PrintStatusWidget::init_static_subjects();

        state.init_subjects(false);
        state.init_extruders({"extruder"});
        state.set_klippy_state_sync(helix::KlippyState::READY);
        client.connect("ws://mock/websocket", []() {}, []() {});
        client.stop_temperature_simulation();
        api = std::make_unique<MoonrakerAPI>(client, state);
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        handler = std::make_unique<FilamentRunoutHandler>(api.get());
        controller = std::make_unique<ScopedResource<helix::TemperatureController>>(
            std::make_unique<helix::TemperatureController>(state, api.get()));
        heater = std::make_unique<ScopedResource<FilamentOpHeater>>(
            std::make_unique<FilamentOpHeater>(state, [this]() { return fake_now; }));

        AmsState::instance().init_subjects(true);
        AmsState::instance().clear_backends();
        AmsState::instance().clear_external_spool_info();
        helix::MacroParamCache::instance().clear();
        configure_filament_macros();
        helix::SafetySettingsManager::instance().init_subjects();
        cold_extrude_was_ = helix::SafetySettingsManager::instance().get_allow_cold_extrude();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
        PostOpCooldownManager::instance().init();
        PostOpCooldownManager::instance().cancel();
        drain_all();
        client.clear_gcode_script_history();
    }

    ~HeaterFixture() override {
        heater.reset();
        controller.reset();
        handler.reset();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(cold_extrude_was_);
        PostOpCooldownManager::instance().cancel();
        set_moonraker_api(previous_api_);
        StandardMacros::instance().reset();
        helix::MacroParamCache::instance().clear();
        AmsState::instance().clear_backends();
        AmsState::instance().clear_external_spool_info();
        drain_all();
        client.disconnect();
        api.reset();
    }

    void configure_filament_macros() {
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"extruder", "gcode_macro LOAD_FILAMENT",
                                  "gcode_macro UNLOAD_FILAMENT", "gcode_macro PURGE"};
        hardware.parse_objects(objects);
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::LoadFilament).is_empty());
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::UnloadFilament).is_empty());
    }

    /// Klipper's report for the extruder, through the real status path.
    void feed_nozzle(double actual_c, double target_c) {
        state.update_from_status({{"extruder", {{"temperature", actual_c}, {"target", target_c}}}});
        drain_all();
    }

    /// The heater's handlers run deferred, so drain until quiet.
    static void drain_all() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Assign an external spool of @p material, which a load with no lane heats for.
    void set_spool(const char* material) {
        SlotInfo spool;
        spool.material = material;
        AmsState::instance().set_external_spool_info_in_memory(spool);
    }

    void set_print_state(const char* wire) {
        state.update_from_status({{"print_stats", {{"state", wire}}}});
        drain_all();
    }

    /// Fire the heater's deadline check, as its LVGL timer does every second.
    void fire_deadline_timer() {
        for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
            if (lv_timer_get_user_data(t) == heater->ptr() && t->timer_cb) {
                t->timer_cb(t);
            }
        }
        drain_all();
    }

    void runout_load() {
        FilamentRunoutHandlerTestAccess::dispatch_load(*handler);
        drain_all();
    }

    /// Index of the first script containing @p needle, or -1.
    [[nodiscard]] int sent_at(const std::string& needle) const {
        const auto& hist = client.gcode_script_history();
        for (size_t i = 0; i < hist.size(); ++i) {
            if (hist[i].find(needle) != std::string::npos) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    [[nodiscard]] bool sent(const std::string& needle) const {
        return sent_at(needle) >= 0;
    }
    /// The load macro, not UNLOAD_FILAMENT, which contains it.
    [[nodiscard]] bool sent_load() const {
        for (const auto& script : client.gcode_script_history()) {
            if (script.rfind("LOAD_FILAMENT", 0) == 0) {
                return true;
            }
        }
        return false;
    }

    std::chrono::steady_clock::time_point fake_now = std::chrono::steady_clock::now();
    MoonrakerClientMock client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    std::unique_ptr<ScopedResource<helix::TemperatureController>> controller;
    std::unique_ptr<ScopedResource<FilamentOpHeater>> heater;
    std::unique_ptr<FilamentRunoutHandler> handler;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
    bool cold_extrude_was_ = false;
};

} // namespace

TEST_CASE_METHOD(HeaterFixture, "The runout dialog's Load heats a cold nozzle before the macro",
                 "[filament][preheat][heater][runout]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    client.clear_gcode_script_history();

    runout_load();

    REQUIRE(heater->get().pending());
    CHECK(sent(std::string(HEAT_GCODE) + "220"));
    CHECK_FALSE(sent_load());

    feed_nozzle(150, 220);
    CHECK_FALSE(sent_load()); // still heating

    feed_nozzle(218, 220);
    CHECK_FALSE(heater->get().pending());
    REQUIRE(sent_load());
    CHECK(sent_at(HEAT_GCODE) < sent_at("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "The runout dialog's Unload heats a cold nozzle before the macro",
                 "[filament][preheat][heater][runout]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    client.clear_gcode_script_history();

    FilamentRunoutHandlerTestAccess::dispatch_unload(*handler);
    drain_all();

    REQUIRE(heater->get().pending());
    CHECK(sent(HEAT_GCODE));
    CHECK_FALSE(sent("UNLOAD_FILAMENT"));

    feed_nozzle(220, 220);
    CHECK(sent("UNLOAD_FILAMENT"));
    CHECK(sent_at(HEAT_GCODE) < sent_at("UNLOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "The runout dialog's Purge heats a cold nozzle before the macro",
                 "[filament][preheat][heater][runout]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    client.clear_gcode_script_history();

    FilamentRunoutHandlerTestAccess::dispatch_purge(*handler);
    drain_all();

    REQUIRE(heater->get().pending());
    CHECK_FALSE(sent("PURGE"));

    feed_nozzle(220, 220);
    CHECK(sent("PURGE"));
    CHECK(sent_at(HEAT_GCODE) < sent_at("PURGE"));
}

TEST_CASE_METHOD(HeaterFixture,
                 "The idle runout dialog's Load heats a cold nozzle before the macro",
                 "[filament][preheat][heater][print_status_widget]") {
    set_print_state("standby");
    feed_nozzle(25, 0);
    client.clear_gcode_script_history();

    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();

    REQUIRE(heater->get().pending());
    CHECK(sent(std::string(HEAT_GCODE) + "220"));
    CHECK_FALSE(sent_load());

    feed_nozzle(218, 220);
    REQUIRE(sent_load());
    CHECK(sent_at(HEAT_GCODE) < sent_at("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "The idle Load runs after the widget that asked for it is gone",
                 "[filament][preheat][heater][print_status_widget]") {
    set_print_state("standby");
    feed_nozzle(25, 0);
    {
        PrintStatusWidget closing;
        PrintStatusWidgetTestAccess::dispatch_load(closing);
        drain_all();
        REQUIRE(heater->get().pending());
    }
    feed_nozzle(220, 220);
    CHECK(sent_load());
}

TEST_CASE_METHOD(HeaterFixture, "A hot nozzle loads at once with no heat command",
                 "[filament][preheat][heater][runout]") {
    set_print_state("standby");
    feed_nozzle(222, 220);
    client.clear_gcode_script_history();

    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent_load());
    CHECK_FALSE(sent(HEAT_GCODE));
}

TEST_CASE_METHOD(HeaterFixture, "The allow-cold-extrude setting skips the preheat",
                 "[filament][preheat][heater][runout]") {
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(true);
    REQUIRE(helix::SafetySettingsManager::instance().get_allow_cold_extrude());
    set_print_state("paused");
    feed_nozzle(25, 0);
    client.clear_gcode_script_history();

    runout_load();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent_load());
    CHECK_FALSE(sent(HEAT_GCODE));
}

TEST_CASE_METHOD(HeaterFixture, "A runout reload during a pause with a hot nozzle does not wait",
                 "[filament][preheat][heater][runout]") {
    // The paused print holds its own temperature; the reload runs at it, even
    // for a spool whose material names a hotter one.
    set_print_state("paused");
    REQUIRE(state.get_print_lifecycle() == PrintState::Paused);
    set_spool("PETG");
    feed_nozzle(210, 210);
    client.clear_gcode_script_history();

    runout_load();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent_load());
    CHECK_FALSE(sent(HEAT_GCODE));
}

TEST_CASE_METHOD(HeaterFixture, "The same reload while idle heats to the spool's material",
                 "[filament][preheat][heater][print_status_widget]") {
    set_print_state("standby");
    set_spool("PETG");
    feed_nozzle(210, 210);
    client.clear_gcode_script_history();

    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();

    CHECK(heater->get().pending());
    CHECK_FALSE(sent_load());
}

TEST_CASE_METHOD(HeaterFixture, "A second tap while heating cancels and turns the heater back off",
                 "[filament][preheat][heater][cancel]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    runout_load();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    runout_load();

    CHECK_FALSE(heater->get().pending());
    CHECK(sent(std::string(HEAT_GCODE) + "0")); // it was off when the user asked

    feed_nozzle(220, 220);
    CHECK_FALSE(sent_load());
}

TEST_CASE_METHOD(HeaterFixture, "Cancelling leaves alone a target someone else had set",
                 "[filament][preheat][heater][cancel]") {
    set_print_state("paused");
    feed_nozzle(25, 150); // someone's own target, lower than the load needs
    runout_load();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    REQUIRE(heater->get().cancel());
    drain_all();

    CHECK_FALSE(heater->get().pending());
    CHECK_FALSE(sent(HEAT_GCODE)); // neither back to 150 nor off
}

TEST_CASE_METHOD(HeaterFixture, "Another op from the same dialog is refused while a preheat waits",
                 "[filament][preheat][heater][cancel]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    runout_load();
    REQUIRE(heater->get().pending());

    FilamentRunoutHandlerTestAccess::dispatch_unload(*handler);
    drain_all();
    REQUIRE(heater->get().pending()); // the load still waits

    feed_nozzle(220, 220);
    CHECK(sent_load());
    CHECK_FALSE(sent("UNLOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture,
                 "Load then Purge from the runout dialog loads, and refuses the purge",
                 "[filament][preheat][heater][cancel]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    runout_load();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    FilamentRunoutHandlerTestAccess::dispatch_purge(*handler);
    drain_all();
    REQUIRE(heater->get().pending());
    CHECK_FALSE(sent(HEAT_GCODE)); // the purge sent no heat of its own

    feed_nozzle(220, 220);
    CHECK(sent_load());
    CHECK_FALSE(sent("PURGE"));
}

TEST_CASE_METHOD(HeaterFixture, "An op from another dialog replaces the pending preheat",
                 "[filament][preheat][heater][cancel]") {
    set_print_state("standby");
    feed_nozzle(25, 0);
    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();
    REQUIRE(heater->get().pending());

    FilamentRunoutHandlerTestAccess::dispatch_unload(*handler);
    drain_all();
    REQUIRE(heater->get().pending());

    feed_nozzle(220, 220);
    CHECK(sent("UNLOAD_FILAMENT"));
    CHECK_FALSE(sent_load());
}

TEST_CASE_METHOD(HeaterFixture, "A preheat whose target is taken away runs nothing",
                 "[filament][preheat][heater][cancel]") {
    set_print_state("standby");
    feed_nozzle(25, 0);
    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();
    REQUIRE(heater->get().pending());

    feed_nozzle(100, 220); // the target lands
    REQUIRE(heater->get().pending());
    feed_nozzle(100, 0); // and is cleared: the user, a heater fault, a shutdown
    CHECK_FALSE(heater->get().pending());

    feed_nozzle(220, 220);
    CHECK_FALSE(sent_load());
}

TEST_CASE_METHOD(HeaterFixture, "A print started during an idle preheat abandons it",
                 "[filament][preheat][heater][print_guard]") {
    set_print_state("standby");
    feed_nozzle(25, 0);
    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();
    REQUIRE(heater->get().pending());

    client.clear_gcode_script_history();
    set_print_state("printing");
    CHECK_FALSE(heater->get().pending());
    CHECK_FALSE(sent(std::string(HEAT_GCODE) + "0")); // the print owns the heater now

    feed_nozzle(220, 220); // PRINT_START heats the nozzle
    CHECK_FALSE(sent_load());
}

TEST_CASE_METHOD(HeaterFixture, "A reload requested during a pause is not abandoned by the job",
                 "[filament][preheat][heater][print_guard][runout]") {
    set_print_state("paused");
    feed_nozzle(25, 0); // the paused print let the nozzle cool

    runout_load();
    REQUIRE(heater->get().pending());

    feed_nozzle(220, 220);
    CHECK(sent_load());
}

TEST_CASE_METHOD(HeaterFixture, "A paused reload still heating when the print resumes is dropped",
                 "[filament][preheat][heater][print_guard][runout]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    runout_load();
    REQUIRE(heater->get().pending());

    client.clear_gcode_script_history();
    set_print_state("printing");
    CHECK_FALSE(heater->get().pending());
    // The resumed print runs on whatever target it has: turning it off would
    // starve the print.
    CHECK_FALSE(sent(std::string(HEAT_GCODE) + "0"));

    feed_nozzle(220, 220);
    CHECK_FALSE(sent_load());
}

TEST_CASE_METHOD(HeaterFixture,
                 "A paused reload still heating when the print is cancelled is dropped",
                 "[filament][preheat][heater][print_guard][runout]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    FilamentRunoutHandlerTestAccess::dispatch_unload(*handler);
    drain_all();
    REQUIRE(heater->get().pending());

    client.clear_gcode_script_history();
    set_print_state("cancelled");
    CHECK_FALSE(heater->get().pending());
    CHECK(sent(std::string(HEAT_GCODE) + "0")); // it was off when the user asked

    feed_nozzle(220, 220);
    CHECK_FALSE(sent("UNLOAD_FILAMENT"));
}

TEST_CASE_METHOD(HeaterFixture, "A target that never lands times the preheat out",
                 "[filament][preheat][heater][timeout]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    runout_load();
    REQUIRE(heater->get().pending());

    fire_deadline_timer(); // within the window: still waiting
    REQUIRE(heater->get().pending());

    client.clear_gcode_script_history();
    fake_now += FilamentOpHeater::TARGET_LAND_TIMEOUT + std::chrono::seconds(1);
    fire_deadline_timer();
    CHECK_FALSE(heater->get().pending());
    // The heat it sent is taken back: the heater was off before the request.
    CHECK(sent(std::string(HEAT_GCODE) + "0"));

    feed_nozzle(220, 220);
    CHECK_FALSE(sent_load());
}

TEST_CASE_METHOD(HeaterFixture, "A nozzle that never gets hot times the preheat out",
                 "[filament][preheat][heater][timeout]") {
    set_print_state("paused");
    feed_nozzle(25, 0);
    runout_load();
    feed_nozzle(120, 220); // the target landed; the heater plateaus
    REQUIRE(heater->get().pending());

    fake_now += FilamentOpHeater::TARGET_LAND_TIMEOUT + std::chrono::seconds(1);
    fire_deadline_timer();
    REQUIRE(heater->get().pending()); // a seen target is not the short timeout

    client.clear_gcode_script_history();
    fake_now += FilamentOpHeater::PREHEAT_TIMEOUT;
    fire_deadline_timer();
    CHECK_FALSE(heater->get().pending());
    CHECK(sent(std::string(HEAT_GCODE) + "0"));
    CHECK_FALSE(sent_load());
}

TEST_CASE_METHOD(HeaterFixture, "Never runs below the extrusion floor",
                 "[filament][preheat][heater][floor]") {
    // A printer whose min_extrude_temp sits above the default load temperature:
    // the wait is the floor itself, and the margin must not undercut it.
    SafetyLimits limits = api->get_safety_limits();
    limits.min_extrude_temp_celsius = 230.0;
    api->set_safety_limits(limits);
    set_print_state("paused");
    feed_nozzle(25, 0);
    runout_load();
    REQUIRE(heater->get().pending());
    CHECK(sent(std::string(HEAT_GCODE) + "230"));

    feed_nozzle(227, 230);
    CHECK(heater->get().pending());
    CHECK_FALSE(sent_load());

    feed_nozzle(230, 230);
    CHECK(sent_load());
}

TEST_CASE_METHOD(HeaterFixture, "An idle load that heated cools down after its macro finishes",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    feed_nozzle(25, 0);
    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();
    feed_nozzle(220, 220);
    REQUIRE(sent_load());

    REQUIRE(wait_until(
        [&cd]() {
            drain_all();
            return cd.has_pending_timer();
        },
        3000));
}

TEST_CASE_METHOD(HeaterFixture, "A paused reload that heated leaves the nozzle hot for the resume",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("paused");
    feed_nozzle(25, 0);
    runout_load();
    feed_nozzle(220, 220);
    REQUIRE(sent_load());

    // Give the macro's completion every chance to land.
    wait_until(
        [&cd]() {
            drain_all();
            return cd.has_pending_timer();
        },
        500);
    CHECK_FALSE(cd.has_pending_timer());
}

TEST_CASE_METHOD(HeaterFixture, "A load with no preheat is not cooled down after",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    feed_nozzle(222, 220); // the user's own heat
    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();
    REQUIRE(sent_load());

    wait_until(
        [&cd]() {
            drain_all();
            return cd.has_pending_timer();
        },
        500);
    CHECK_FALSE(cd.has_pending_timer());
}

TEST_CASE_METHOD(HeaterFixture, "A preheat takes over a cooldown left from the previous op",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    cd.schedule();
    drain_all();
    REQUIRE(cd.has_pending_timer());

    feed_nozzle(25, 0);
    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();

    REQUIRE(heater->get().pending());
    CHECK_FALSE(cd.has_pending_timer());
}

TEST_CASE_METHOD(HeaterFixture, "A preheat over a pending cooldown counts the nozzle as off",
                 "[filament][preheat][heater][cooldown]") {
    // The target the previous op left is heat a cooldown still owes: cancelling
    // this preheat turns it off rather than leaving it as someone's target.
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    cd.schedule();
    drain_all();
    REQUIRE(cd.has_pending_timer());

    feed_nozzle(100, 200);
    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();
    REQUIRE(heater->get().pending());
    client.clear_gcode_script_history();

    REQUIRE(heater->get().cancel());
    drain_all();
    CHECK(sent(std::string(HEAT_GCODE) + "0"));
}

TEST_CASE_METHOD(HeaterFixture, "A load that runs at once over a pending cooldown is cooled after",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    cd.schedule();
    drain_all();
    REQUIRE(cd.has_pending_timer());

    feed_nozzle(222, 220); // still hot from the previous op
    PrintStatusWidget widget;
    PrintStatusWidgetTestAccess::dispatch_load(widget);
    drain_all();
    REQUIRE_FALSE(heater->get().pending());
    REQUIRE(sent_load());
    CHECK_FALSE(sent(HEAT_GCODE));

    REQUIRE(wait_until(
        [&cd]() {
            drain_all();
            return cd.has_pending_timer();
        },
        3000));
}

TEST_CASE_METHOD(HeaterFixture, "A backend op's end on the action feed schedules the cooldown",
                 "[filament][preheat][heater][cooldown]") {
    auto& cd = PostOpCooldownManager::instance();
    set_print_state("standby");
    lv_subject_t* action = AmsState::instance().get_ams_action_subject();
    lv_subject_set_int(action, static_cast<int>(AmsAction::IDLE));

    feed_nozzle(25, 0);
    helix::ui::FilamentOpTicket ticket = 0;
    helix::ui::PreheatRequest req;
    req.op = helix::ui::FilamentHeatOp::Load;
    req.run = [&ticket](helix::ui::FilamentOpTicket t) { ticket = t; };
    REQUIRE(heater->get().run_after_preheat(std::move(req)) == FilamentOpHeater::Outcome::Heating);
    feed_nozzle(220, 220);
    REQUIRE(ticket != 0);

    SECTION("busy, then IDLE") {
        heater->get().watch_backend_completion(ticket);
        drain_all();
        CHECK_FALSE(cd.has_pending_timer()); // IDLE at dispatch is not the end

        lv_subject_set_int(action, static_cast<int>(AmsAction::LOADING));
        drain_all();
        lv_subject_set_int(action, static_cast<int>(AmsAction::IDLE));
        drain_all();
        CHECK(cd.has_pending_timer());
    }
    SECTION("busy, then ERROR leaves the heater for the recovery") {
        heater->get().watch_backend_completion(ticket);
        lv_subject_set_int(action, static_cast<int>(AmsAction::LOADING));
        drain_all();
        lv_subject_set_int(action, static_cast<int>(AmsAction::ERROR));
        drain_all();
        CHECK_FALSE(cd.has_pending_timer());
    }
}
