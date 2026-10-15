// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_update_queue.h"

#include "async_lifetime_guard.h"
#include "config.h"
#include "memory_monitor.h"
#include "system/telemetry_manager.h"
#include "telemetry_manager_test_access.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include "hv/json.hpp"

/**
 * @brief Test fixture for TelemetryManager testing
 *
 * Provides a unique temporary directory for each test and initializes
 * TelemetryManager with that directory as the config root. Cleans up
 * after each test to ensure isolation.
 */
class TelemetryTestFixture {
  public:
    TelemetryTestFixture() {
        // Create unique temp directory per test run
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("helix_telemetry_test_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(temp_dir_);

        auto& tm = TelemetryManager::instance();
        tm.shutdown();

        // Drain any deferred UI callbacks queued by prior tests. Some of those
        // callbacks (settings observers, panel-change notifiers) can route into
        // TelemetryManager::record_* and end up enqueuing a phantom event in
        // the next test. Drain happens BEFORE init so callbacks see the
        // !initialized_ guard and bail out before touching the queue.
        helix::ui::UpdateQueue::instance().drain();

        // Reset Config's /telemetry_enabled to false BEFORE init() so the
        // singleton reload at the end of init() lands at a known false state.
        // Without this, a prior test's set_enabled(true) persisted into the
        // global Config, init() reloaded it as true, and any error/setting
        // change between init() and our explicit set_enabled(false) below
        // would gate-pass and enqueue (off-by-one queue size on macOS where
        // the whole suite shares one process).
        //
        // Also clear the Config singleton's path. Some prior tests
        // (test_external_spool.cpp's TempConfigFixture, test_filament_
        // consumption_tracker.cpp) call Config::get_instance()->init(<temp_dir>)
        // and then delete the directory. The singleton retains a now-dangling
        // path; subsequent set_enabled(true) → Config::save() fails its
        // ofstream open, calls CONFIG_RECORD_ERROR, and enqueues a phantom
        // telemetry "file_io" event between our clear_queue() and the test's
        // first assertion. Empty path makes save() a no-op (line 1426).
        if (auto* cfg = helix::Config::get_instance()) {
            cfg->clear_path();
            cfg->set<bool>("/telemetry_enabled", false);
        }

        // Frozen before init() so every window starts on the manual clock and
        // holds only the time a test steps, however slowly the test runs.
        TelemetryManagerTestAccess::freeze_clock(tm);
        tm.init(temp_dir_.string());
        tm.set_enabled(false);
        tm.clear_queue();
    }

    ~TelemetryTestFixture() {
        // Drain BEFORE shutdown(), while the subject these callbacks write is
        // still alive. set_enabled() defers its subject write, so the ctor's
        // set_enabled(false) — plus any set_enabled() in the test body — is
        // still queued at this point. Leaving them for whichever fixture
        // drained next is what made TelemetryManager::set_enabled the single
        // largest producer in the cross-test leak report: 185 callbacks across
        // 96 tests, 21% of the suite total (#1166).
        helix::ui::UpdateQueue::instance().drain();

        TelemetryManager::instance().shutdown();
        TelemetryManagerTestAccess::unfreeze_clock(TelemetryManager::instance());

        // Clean up temp directory - best effort
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    [[nodiscard]] std::filesystem::path temp_dir() const {
        return temp_dir_;
    }

    /**
     * @brief Write arbitrary content to a file in the temp directory
     */
    void write_file(const std::string& filename, const std::string& content) {
        std::ofstream ofs(temp_dir_ / filename);
        ofs << content;
        ofs.close();
    }

    /**
     * @brief Read content from a file in the temp directory
     */
    [[nodiscard]] std::string read_file(const std::string& filename) const {
        std::ifstream ifs(temp_dir_ / filename);
        return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
    }

  protected:
    std::filesystem::path temp_dir_;
};

/// Drive every record_* path that appends to the queue and return the events,
/// keyed by event name. Each event is built from fixed inputs: the per-panel,
/// overlay, widget and connection maps are non-empty, the tracker clock is
/// stepped by whole seconds so their durations are exact, and the crash file
/// carries every field the crash event forwards.
inline std::map<std::string, nlohmann::json> record_one_of_every_event(TelemetryManager& tm,
                                                                       TelemetryTestFixture& fx) {
    using namespace std::chrono_literals;
    std::map<std::string, nlohmann::json> by_name;
    auto collect = [&]() {
        for (const auto& e : tm.get_queue_snapshot()) {
            by_name[e.value("event", "")] = e;
        }
        tm.clear_queue();
    };

    tm.record_session();
    collect();
    tm.record_print_outcome("completed", 600, 5, 300.0f, "PLA", 200, 60);
    collect();
    tm.record_update_failure("download_failed", "0.14.0", "pi");
    collect();
    fx.write_file("update_success.json",
                  R"({"version":"0.14.0","from_version":"0.13.4","platform":"pi",)"
                  R"("timestamp":"2026-02-26T12:00:00Z"})");
    tm.check_previous_update();
    collect();
    tm.record_memory_snapshot("hourly");
    collect();
    helix::MemoryWarningEvent warning;
    warning.level = helix::MemoryPressureLevel::warning;
    warning.reason = "rss_growth";
    tm.record_memory_warning(warning);
    collect();
    tm.record_hardware_profile();
    collect();
    tm.record_settings_snapshot();
    collect();

    tm.notify_panel_changed("home");
    TelemetryManagerTestAccess::advance_clock(tm, 90s);
    tm.notify_panel_changed("controls");
    tm.notify_overlay_opened("fan_control_overlay");
    tm.notify_widget_interaction("temperature");
    tm.record_panel_usage();
    collect();

    tm.notify_connection_state_changed(2);
    TelemetryManagerTestAccess::advance_clock(tm, 30s);
    tm.notify_connection_state_changed(0);
    TelemetryManagerTestAccess::advance_clock(tm, 5s);
    tm.notify_connection_state_changed(2);
    tm.notify_klippy_state_changed(3);
    tm.record_connection_stability();
    collect();

    tm.record_frame_time(8000); // a snapshot is only built once a frame was sampled
    tm.record_performance_snapshot();
    collect();
    tm.notify_setting_changed("theme", "dark", "light");
    tm.flush_settings_changes();
    collect();
    // The skip counters are process-wide; draining them first leaves this
    // window holding only the skip noted here.
    (void)helix::async_lifetime::take_snapshot();
    helix::async_lifetime::note_skipped("Envelope::cb");
    tm.record_async_lifetime_snapshot();
    collect();
    tm.record_print_start_context("local", true, 5000000, 3600, "PrusaSlicer", 1, false);
    collect();
    tm.record_error("moonraker_api", "timeout", "get_printer_objects");
    collect();
    fx.write_file(
        "crash.txt",
        "signal:11\n"
        "name:SIGSEGV\n"
        "version:0.14.0\n"
        "timestamp:1767225600\n"
        "uptime:3600\n"
        "fault_addr:0x0\n"
        "fault_code:1\n"
        "fault_code_name:SEGV_MAPERR\n"
        "reg_pc:0x401000\n"
        "reg_sp:0x7ffc0000\n"
        "reg_lr:0x401200\n"
        "reg_bp:0x7ffc0040\n"
        "load_base:0x400000\n"
        "bt:0x401000\n"
        "bt:0x401200\n"
        "map:00400000-00500000 r-xp 00000000 00:00 0 /opt/helixscreen/bin/helix-screen\n");
    tm.check_previous_crash();
    collect();
    return by_name;
}
