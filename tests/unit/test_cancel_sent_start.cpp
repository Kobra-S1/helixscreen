// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_cancel_sent_start.cpp
 * @brief Stop during preparing, after the start was sent to the printer (#1890).
 *
 * A firmware that runs its print-start sequence inside SDCARD_PRINT_FILE keeps
 * print_stats idle while it homes, heats and probes. The job is already on the
 * printer, so withdrawing it locally stops nothing: the cancel has to go to the
 * printer, and the job is not cancelled until the printer says so.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/print_control_buttons_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "print_control_buttons.h"
#include "print_job_ref.h"
#include "printer_state.h"

#include <functional>
#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::PrintControlButtons;
using helix::ui::PrintControlButtonsTestAccess;
using json = nlohmann::json;

namespace {

struct SentStartFixture : LVGLTestFixture {
    SentStartFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        PrintControlButtonsTestAccess::reset();
        helix::PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        PrintControlButtons::instance().init_subjects();

        client.connect("ws://mock/websocket", []() {}, []() {});
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            client, "printer.print.cancel",
            [this](MoonrakerClientMock*, const json&, std::function<void(const json&)> ok,
                   std::function<void(const MoonrakerError&)> err) -> bool {
                ++cancels;
                cancel_ok = std::move(ok);
                cancel_err = std::move(err);
                return true;
            });
        api = std::make_unique<MoonrakerAPIMock>(client, get_printer_state());
        PrintControlButtons::instance().set_api(api.get());
    }

    ~SentStartFixture() override {
        PrintControlButtons::instance().set_api(nullptr);
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    static helix::PrinterPrintState& ps() {
        return get_printer_state().print_state();
    }

    static int stop_enabled() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "print_control_stop_enabled"));
    }

    MoonrakerClientMock client;
    std::unique_ptr<MoonrakerAPIMock> api;
    int cancels = 0;
    std::function<void(const json&)> cancel_ok;
    std::function<void(const MoonrakerError&)> cancel_err;
};

} // namespace

TEST_CASE_METHOD(SentStartFixture, "Stop before the start is sent withdraws it locally",
                 "[print_control][preparing_cancel]") {
    ps().begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    drain();

    PrintControlButtonsTestAccess::confirm_stop();
    drain();

    CHECK(cancels == 0);
    CHECK_FALSE(ps().has_preparing_job());
    CHECK(ps().last_preparing_exit() == helix::PreparingExit::Cancelled);
}

TEST_CASE_METHOD(SentStartFixture,
                 "Stop after the start was sent cancels on the printer and waits for its answer",
                 "[print_control][preparing_cancel]") {
    ps().begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    ps().note_preparing_start_sent();
    drain();

    PrintControlButtonsTestAccess::confirm_stop();
    drain();

    // The cancel went to the printer, and the job is not shown cancelled yet:
    // the printer is still running its start sequence.
    REQUIRE(cancels == 1);
    CHECK(ps().has_preparing_job());
    CHECK(PrintControlButtonsTestAccess::cancel_requested());
    CHECK(stop_enabled() == 0);

    // CANCEL_PRINT ran once the sequence yielded.
    REQUIRE(cancel_ok);
    cancel_ok(json::object());
    drain();

    CHECK_FALSE(ps().has_preparing_job());
    CHECK(ps().last_preparing_exit() == helix::PreparingExit::Cancelled);
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
}

TEST_CASE_METHOD(SentStartFixture, "A refused cancel of a sent start leaves the job and Stop live",
                 "[print_control][preparing_cancel]") {
    ps().begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    ps().note_preparing_start_sent();
    drain();

    PrintControlButtonsTestAccess::confirm_stop();
    drain();
    REQUIRE(cancel_err);
    cancel_err(MoonrakerError::timeout("printer.print.cancel", 300000));
    drain();

    CHECK(ps().has_preparing_job());
    CHECK_FALSE(PrintControlButtonsTestAccess::cancel_requested());
    CHECK(stop_enabled() == 1);
}

TEST_CASE_METHOD(SentStartFixture,
                 "A cancel answered after a newer job began leaves that job alone",
                 "[print_control][preparing_cancel]") {
    ps().begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    ps().note_preparing_start_sent();
    drain();
    PrintControlButtonsTestAccess::confirm_stop();
    drain();
    REQUIRE(cancel_ok);

    ps().retire_preparing(helix::PreparingExit::Confirmed);
    ps().begin_preparing(helix::PrintJobRef{"next.gcode", "", ""});
    cancel_ok(json::object());
    drain();

    CHECK(ps().has_preparing_job());
    CHECK(ps().preparing_job().filename == "next.gcode");
}

TEST_CASE_METHOD(SentStartFixture, "The start-sent mark belongs to one preparing job",
                 "[print_control][preparing_cancel]") {
    // Nothing to mark without a job.
    ps().note_preparing_start_sent();
    CHECK_FALSE(ps().preparing_start_sent());

    ps().begin_preparing(helix::PrintJobRef{"part.gcode", "", ""});
    CHECK_FALSE(ps().preparing_start_sent());
    ps().note_preparing_start_sent();
    CHECK(ps().preparing_start_sent());

    ps().retire_preparing(helix::PreparingExit::Cancelled);
    CHECK_FALSE(ps().preparing_start_sent());

    ps().note_preparing_start_sent();
    ps().begin_preparing(helix::PrintJobRef{"next.gcode", "", ""});
    CHECK_FALSE(ps().preparing_start_sent());
}
