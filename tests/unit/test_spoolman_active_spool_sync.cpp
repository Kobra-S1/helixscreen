// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_spoolman_active_spool_sync.cpp
 * @brief The startup sync of the external spool slot from Moonraker's active
 *        Spoolman spool.
 *
 * Moonraker answers server.spoolman.status on the WebSocket thread, and deciding
 * whether to fetch reads AmsState, which the main thread owns.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/unique_temp_dir.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_state.h"
#include "app_constants.h"
#include "config.h"
#include "i_moonraker_sub_apis.h"
#include "spoolman_active_spool_sync.h"
#include "spoolman_types.h"

#include <filesystem>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Config;
using nlohmann::json;
namespace fs = std::filesystem;
namespace spool_sync = helix::spoolman_sync;

namespace {

/// Records the Spoolman calls the sync makes and keeps their callbacks so a test
/// decides when, and from which thread, the server answers.
class RecordingSpoolman : public ISpoolmanAPI {
  public:
    struct SpoolRequest {
        int id;
        helix::SpoolCallback on_success;
        bool on_main_thread;
    };
    std::vector<SpoolRequest> spool_requests;
    std::function<void(bool, int)> status_callback;

    void get_spoolman_status(std::function<void(bool, int)> on_success, ErrorCallback,
                             bool) override {
        status_callback = std::move(on_success);
    }
    void get_spoolman_spool(int spool_id, helix::SpoolCallback on_success, ErrorCallback,
                            bool) override {
        spool_requests.push_back({spool_id, std::move(on_success), helix::ui::is_main_thread()});
    }

    void get_spoolman_spools(helix::SpoolListCallback, ErrorCallback) override {}
    void set_active_spool(int, SuccessCallback, ErrorCallback) override {}
    void get_spool_usage_history(int, std::function<void(const std::vector<FilamentUsageRecord>&)>,
                                 ErrorCallback) override {}
    void update_spoolman_spool_weight(int, double, SuccessCallback, ErrorCallback) override {}
    void update_spoolman_spool(int, const json&, SuccessCallback, ErrorCallback) override {}
    void update_spoolman_filament(int, const json&, SuccessCallback, ErrorCallback) override {}
    void update_spoolman_filament_color(int, const std::string&, SuccessCallback,
                                        ErrorCallback) override {}
    void get_spoolman_vendors(helix::VendorListCallback, ErrorCallback) override {}
    void get_spoolman_filaments(helix::FilamentListCallback, ErrorCallback) override {}
    void get_spoolman_filaments(int, helix::FilamentListCallback, ErrorCallback) override {}
    void create_spoolman_vendor(const json&, helix::VendorCreateCallback, ErrorCallback) override {}
    void create_spoolman_filament(const json&, helix::FilamentCreateCallback,
                                  ErrorCallback) override {}
    void create_spoolman_spool(const json&, helix::SpoolCreateCallback, ErrorCallback) override {}
    void delete_spoolman_spool(int, SuccessCallback, ErrorCallback) override {}
    void delete_spoolman_vendor(int, SuccessCallback, ErrorCallback) override {}
    void delete_spoolman_filament(int, SuccessCallback, ErrorCallback) override {}
    void get_spoolman_external_vendors(helix::VendorListCallback, ErrorCallback) override {}
    void get_spoolman_external_filaments(const std::string&, helix::FilamentListCallback,
                                         ErrorCallback) override {}
};

struct SyncFixture : LVGLTestFixture {
    std::string temp_dir = helix::test::unique_temp_dir("helix_spool_sync");
    RecordingSpoolman spoolman;
    std::vector<int> applied;

    SyncFixture() {
        fs::create_directories(temp_dir);
        fs::remove(AppConstants::Update::config_backup_fallback());
        fs::remove(AppConstants::Update::legacy_config_backup_fallback());
        fs::remove(AppConstants::Update::env_backup_fallback());
        Config::get_instance()->init(temp_dir + "/settings.json");
        AmsState::instance().clear_external_spool_info();
        spool_sync::request_startup_sync(
            spoolman, [this](const SpoolInfo& spool, std::string) { applied.push_back(spool.id); });
    }

    ~SyncFixture() override {
        helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        AmsState::instance().clear_external_spool_info();
        Config::get_instance()->clear_path();
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    void pump() {
        helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    }

    void answer_with_spool(size_t index, int id) {
        SpoolInfo spool;
        spool.id = id;
        spoolman.spool_requests.at(index).on_success(spool);
    }
};

} // namespace

TEST_CASE_METHOD(SyncFixture, "the startup status reply is acted on from the main thread",
                 "[spoolman_sync][threading]") {
    REQUIRE(spoolman.status_callback);
    std::thread([this] { spoolman.status_callback(true, 5); }).join();

    CHECK(spoolman.spool_requests.empty());
    pump();
    REQUIRE(spoolman.spool_requests.size() == 1);
    CHECK(spoolman.spool_requests[0].id == 5);
    CHECK(spoolman.spool_requests[0].on_main_thread);

    answer_with_spool(0, 5);
    CHECK(applied == std::vector<int>{5});
}

TEST_CASE_METHOD(SyncFixture, "a startup status the external slot already holds costs no fetch",
                 "[spoolman_sync]") {
    SlotInfo slot;
    slot.slot_index = -2;
    slot.global_index = -2;
    slot.spoolman_id = 8;
    AmsState::instance().set_external_spool_info_in_memory(slot);

    SECTION("same spool: no fetch") {
        spoolman.status_callback(true, 8);
        pump();
        CHECK(spoolman.spool_requests.empty());
    }
    SECTION("different spool: fetched") {
        spoolman.status_callback(true, 9);
        pump();
        REQUIRE(spoolman.spool_requests.size() == 1);
        CHECK(spoolman.spool_requests[0].id == 9);
    }
    SECTION("Spoolman down or no active spool: no fetch") {
        spoolman.status_callback(false, 9);
        spoolman.status_callback(true, 0);
        pump();
        CHECK(spoolman.spool_requests.empty());
    }
}
