// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_slot_ceiling.cpp
 * @brief A large multi-unit setup binds every lane up to AMS_MAX_SLOTS, and a
 *        lane past it degrades with one warning instead of blanking.
 *
 * The 23-lane backend is the shape of a seven-unit AFC rig: twenty-two unit
 * lanes plus a direct-drive extruder.
 */

#include "ui_ams_slot.h"
#include "ui_filament_path_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr int kLargeRigLanes = 23;
constexpr int kLastLane = kLargeRigLanes - 1;

/// Give @p slot a colour and material no sample filament carries, so a match
/// proves the value came from this slot.
void paint_slot(AmsBackendMock& mock, int slot, uint32_t color, const char* material) {
    SlotInfo info = mock.get_slot_info(slot);
    info.color_rgb = color;
    info.material = material;
    REQUIRE(mock.set_slot_info(slot, info).success());
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "AmsState: every lane of a 23-lane rig binds its own subjects",
                 "[ams][slot_ceiling]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(kLargeRigLanes);
    AmsBackendMock* mock = owned.get();
    REQUIRE(mock->get_system_info().total_slots == kLargeRigLanes);
    paint_slot(*mock, kLastLane, 0x2266AA, "PETG");
    ams.set_backend(std::move(owned));
    ams.sync_from_backend();

    lv_subject_t* color = ams.get_slot_color_subject(kLastLane);
    REQUIRE(color != nullptr);
    CHECK(static_cast<uint32_t>(lv_subject_get_int(color)) == 0x2266AA);
    REQUIRE(ams.get_slot_status_subject(kLastLane) != nullptr);
    CHECK(lv_subject_get_int(ams.get_slot_status_subject(kLastLane)) ==
          static_cast<int>(SlotStatus::AVAILABLE));
    REQUIRE(ams.get_slot_material_subject(kLastLane) != nullptr);
    CHECK(std::string(lv_subject_get_string(ams.get_slot_material_subject(kLastLane))) == "PETG");

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: lane 23 shows its material, not the unbound dash",
                 "[ams][slot_ceiling][ui]") {
    ui_ams_slot_register();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(kLargeRigLanes);
    paint_slot(*owned, kLastLane, 0x2266AA, "PETG");
    ams.set_backend(std::move(owned));
    ams.sync_from_backend();

    const std::string index = std::to_string(kLastLane);
    const char* attrs[] = {"slot_index", index.c_str(), nullptr};
    auto* slot = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_slot", attrs));
    REQUIRE(slot != nullptr);
    lv_obj_t* material = lv_obj_find_by_name(slot, "material_label");
    REQUIRE(material != nullptr);
    CHECK(std::string(lv_label_get_text(material)) == "PETG");

    lv_obj_delete(slot);
    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "FilamentPath: the canvas holds state for lane 23",
                 "[ams][slot_ceiling][filament_path]") {
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);
    ui_filament_path_canvas_set_slot_count(path, kLargeRigLanes);
    ui_filament_path_canvas_set_slot_filament(path, kLastLane, static_cast<int>(PathSegment::HUB),
                                              0x2266AA);
    CHECK(ui_filament_path_canvas_get_slot_filament(path, kLastLane) ==
          static_cast<int>(PathSegment::HUB));
    lv_obj_delete(path);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsState: a loaded lane past the ceiling still drives Currently Loaded",
                 "[ams][slot_ceiling]") {
    constexpr int kLanes = AMS_MAX_SLOTS + 2;
    constexpr int kBeyond = kLanes - 1;
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(kLanes);
    AmsBackendMock* mock = owned.get();
    REQUIRE(mock->get_system_info().total_slots == kLanes);
    REQUIRE(mock->start().success());
    mock->set_operation_delay(10);
    paint_slot(*mock, kBeyond, 0x2266AA, "PETG");
    ams.set_backend(std::move(owned));

    helix::LogCapture log;
    REQUIRE(mock->load_filament(kBeyond).success());
    mock->wait_for_operation_thread();
    ams.sync_from_backend();
    ams.update_slot(kBeyond);
    ams.sync_from_backend();

    REQUIRE(mock->get_current_slot() == kBeyond);
    CHECK(ams.get_slot_color_subject(kBeyond) == nullptr);
    CHECK(static_cast<uint32_t>(lv_subject_get_int(ams.get_current_color_subject())) == 0x2266AA);
    // One warning names the overflow, however often the rig syncs.
    CHECK(log.count_containing("per-slot subjects") == 1);

    ams.clear_backends();
}
