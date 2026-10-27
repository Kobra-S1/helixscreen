// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_medusahc_controller.cpp
 * @brief The MedusaHC mock speaks the stock controller's surface.
 *
 * Irbis3D's medusahc.py and MHC_macros.cfg: offsets on `_TOOL_OFFSET`
 * (t<n>_off_<axis>) restored from save_variables, MHC_TOOL_OFFSET applying the
 * mounted tool's, CLEAN brushing the mounted nozzle, the layer from
 * MHC_LAYER_SET, and the PAUSE/RESUME recovery after a failed swap: RESUME runs
 * MHC_RESUME, which re-picks `_GLOBAL_STATE.target_tool` and refuses while that
 * nozzle is at or below 190C.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;
using Catch::Approx;

namespace {

json query(MoonrakerClientMock& mock, const std::vector<std::string>& names) {
    json objects = json::object();
    for (const auto& n : names) {
        objects[n] = nullptr;
    }
    json response;
    mock.send_jsonrpc(
        "printer.objects.query", json{{"objects", objects}},
        [&response](const json& r) { response = r; }, [](const MoonrakerError&) {});
    return response["result"]["status"];
}

/// Advance the swap a notification at a time until the controller rests.
void run_to_rest(MoonrakerClientMock& mock) {
    for (int i = 0; i < 40; ++i) {
        MoonrakerClientMockTestAccess::advance_medusa(mock);
        if (mock.medusa_status_json()["operation"] == "idle") {
            break;
        }
    }
    ui::UpdateQueue::instance().drain();
}

class StockMedusaFixture : public LVGLTestFixture {
  public:
    ScopedEnv printer_env{"HELIX_MOCK_PRINTER", nullptr};
    ScopedEnv ams_env{"HELIX_MOCK_AMS", "medusahc"};
};

} // namespace

TEST_CASE_METHOD(StockMedusaFixture, "MedusaHC mock: offsets live on _TOOL_OFFSET and saved_vars",
                 "[medusahc][mock][tool_offsets]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    REQUIRE(mock.gcode_script("SET_GCODE_VARIABLE MACRO=_TOOL_OFFSET VARIABLE=t1_off_x "
                              "VALUE=0.120") == 0);
    REQUIRE(mock.gcode_script("SAVE_VARIABLE VARIABLE=t1_gcode_y_offset VALUE=-0.045") == 0);

    const json status =
        query(mock, {"gcode_macro _TOOL_OFFSET", "save_variables", "gcode_macro _GLOBAL_STATE"});
    REQUIRE(status.contains("gcode_macro _TOOL_OFFSET"));
    CHECK(status["gcode_macro _TOOL_OFFSET"]["t1_off_x"].get<double>() == Approx(0.12));
    CHECK(status["gcode_macro _TOOL_OFFSET"]["t3_off_z"].get<double>() == Approx(0.0));
    CHECK(status["save_variables"]["variables"]["t1_gcode_y_offset"].get<double>() ==
          Approx(-0.045));
    CHECK(status["gcode_macro _GLOBAL_STATE"]["max_tool"].get<int>() == 4);
}

TEST_CASE_METHOD(StockMedusaFixture, "MedusaHC mock: MHC_TOOL_OFFSET applies the tool's offset",
                 "[medusahc][mock][tool_offsets]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(mock.gcode_script("SET_GCODE_VARIABLE MACRO=_TOOL_OFFSET VARIABLE=t0_off_z "
                              "VALUE=-0.250") == 0);

    json origin;
    const auto sub = mock.register_notify_update([&origin](const json& notification) {
        const auto& params = notification["params"];
        if (params.is_array() && !params.empty() && params[0].contains("gcode_move")) {
            origin = params[0]["gcode_move"]["homing_origin"];
        }
    });
    REQUIRE(mock.gcode_script("MHC_TOOL_OFFSET T=0 MOVE=0") == 0);
    mock.unsubscribe_notify_update(sub);
    REQUIRE(origin.is_array());
    CHECK(origin[2].get<double>() == Approx(-0.25));

    CHECK(mock.gcode_script("MHC_TOOL_OFFSET T=9 MOVE=0") != 0);
    CHECK(mock.get_last_gcode_error() == "Tool T9 is outside configured range T0..T3");
}

TEST_CASE_METHOD(StockMedusaFixture, "MedusaHC mock: the layer follows MHC_LAYER_SET",
                 "[medusahc][mock]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(mock.gcode_script("MHC_LAYER_SET L=5") == 0);
    CHECK(mock.medusa_status_json()["layer"].get<int>() == 5);
    REQUIRE(mock.gcode_script("_LAYER_SET L=7") == 0);
    CHECK(mock.medusa_status_json()["layer"].get<int>() == 7);
}

TEST_CASE_METHOD(StockMedusaFixture, "MedusaHC mock: CLEAN brushes only a mounted nozzle",
                 "[medusahc][mock][clean]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(mock.medusa_status_json()["current_tool"].get<int>() == 0);
    CHECK(mock.gcode_script("CLEAN") == 0);
    CHECK(mock.medusa_status_json()["operation"] == "idle"); // MHC_CLEAN has no phase

    mock.medusa_place_tools_by_hand(-1);
    CHECK(mock.gcode_script("MHC_CLEAN") == 0); // "no tool installed", not an error
}

TEST_CASE_METHOD(StockMedusaFixture, "MedusaHC mock: a failed swap mid-print recovers on RESUME",
                 "[medusahc][mock][resume]") {
    ScopedEnv fail_env("HELIX_MOCK_MEDUSA_SWAP_FAIL", "1");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(mock.start_print_internal("3DBenchy.gcode"));

    REQUIRE(mock.gcode_script("T1") == 0);
    run_to_rest(mock);
    REQUIRE(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PAUSED);
    json global = query(mock, {"gcode_macro _GLOBAL_STATE"})["gcode_macro _GLOBAL_STATE"];
    CHECK(global["error_state"].get<int>() == 1);
    CHECK(global["target_tool"].get<int>() == 1);

    // extruder1 sits at 150C in the mock: MHC_RESUME refuses.
    CHECK(mock.gcode_script("RESUME") != 0);
    CHECK(mock.get_last_gcode_error() == "MHC_RESUME: heat T1 above 190C before resuming");
    CHECK(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PAUSED);

    // The user seats the hot ends by hand; T2 (248C) is hot: picked, then the
    // print continues.
    mock.medusa_place_tools_by_hand(-1);
    REQUIRE(mock.gcode_script("SET_GCODE_VARIABLE MACRO=_GLOBAL_STATE VARIABLE=target_tool "
                              "VALUE=2") == 0);
    REQUIRE(mock.gcode_script("RESUME") == 0);
    run_to_rest(mock);
    CHECK(mock.medusa_status_json()["current_tool"].get<int>() == 2);
    CHECK_FALSE(mock.medusa_status_json()["sensor_error"].get<bool>());
    CHECK(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PRINTING);
    global = query(mock, {"gcode_macro _GLOBAL_STATE"})["gcode_macro _GLOBAL_STATE"];
    CHECK(global["error_state"].get<int>() == 0);
}

TEST_CASE_METHOD(StockMedusaFixture, "MedusaHC mock: a manual pause with the tool mounted resumes",
                 "[medusahc][mock][resume]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    REQUIRE(mock.start_print_internal("3DBenchy.gcode"));
    REQUIRE(mock.gcode_script("PAUSE") == 0);
    REQUIRE(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PAUSED);

    // T0 on the carriage and the PAUSE saved it: no re-pick, no heat rule.
    REQUIRE(mock.gcode_script("RESUME") == 0);
    CHECK(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PRINTING);
    CHECK(mock.medusa_status_json()["operation"] == "idle");
}
