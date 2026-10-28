// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_sensor_row_xml.cpp
 * @brief sensor_row.xml binds every part of a Settings > Sensors row to the
 *        per-index subjects sensor_<section>_row_<field>_<index>.
 */

#include "ui_switch.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "lvgl/lvgl.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

constexpr const char* kSection = "rowtest";
constexpr const char* kIndex = "0";

/// Registers the twelve row subjects for rowtest/0 once for the process: the XML
/// subject registry never forgets a name, so the storage must outlive the test.
struct RowSubjects {
    lv_subject_t name;
    lv_subject_t detail;
    lv_subject_t state;
    lv_subject_t has_state;
    lv_subject_t alert;
    lv_subject_t role;
    lv_subject_t has_role;
    lv_subject_t toggle_kind;
    lv_subject_t toggle;
    lv_subject_t held_role;
    lv_subject_t has_detail;
    lv_subject_t has_held_role;
    char name_buf[64] = "Front Door";
    char held_role_buf[32] = "";
    char detail_buf[64] = "detail";
    char state_buf[32] = "Open";

    RowSubjects() {
        lv_subject_init_string(&name, name_buf, nullptr, sizeof(name_buf), name_buf);
        lv_subject_init_string(&detail, detail_buf, nullptr, sizeof(detail_buf), detail_buf);
        lv_subject_init_string(&state, state_buf, nullptr, sizeof(state_buf), state_buf);
        lv_subject_init_int(&has_state, 1);
        lv_subject_init_int(&alert, 0);
        lv_subject_init_int(&role, 0);
        lv_subject_init_int(&has_role, 1);
        lv_subject_init_int(&toggle_kind, 0);
        lv_subject_init_int(&toggle, 0);
        lv_subject_init_string(&held_role, held_role_buf, nullptr, sizeof(held_role_buf), "");
        lv_subject_init_int(&has_held_role, 0);
        lv_subject_init_int(&has_detail, 1);
        reg("name", &name);
        reg("detail", &detail);
        reg("state", &state);
        reg("has_state", &has_state);
        reg("alert", &alert);
        reg("role", &role);
        reg("has_role", &has_role);
        reg("toggle_kind", &toggle_kind);
        reg("toggle", &toggle);
        reg("held_role", &held_role);
        reg("has_held_role", &has_held_role);
        reg("has_detail", &has_detail);
    }

    static void reg(const char* field, lv_subject_t* subj) {
        std::string key = std::string("sensor_") + kSection + "_row_" + field + "_" + kIndex;
        lv_xml_register_subject(nullptr, key.c_str(), subj);
    }
};

RowSubjects& subjects() {
    static RowSubjects s;
    return s;
}

class SensorRowFixture : public XMLTestFixture {
  public:
    SensorRowFixture() : XMLTestFixture() {
        ui_switch_register();
        REQUIRE(register_component("sensor_row"));
        auto& s = subjects();
        lv_subject_copy_string(&s.name, "Front Door");
        lv_subject_copy_string(&s.state, "Open");
        lv_subject_set_int(&s.has_state, 1);
        lv_subject_set_int(&s.alert, 0);
        lv_subject_set_int(&s.role, 0);
        lv_subject_set_int(&s.has_role, 1);
        lv_subject_set_int(&s.toggle_kind, 0);
        lv_subject_set_int(&s.toggle, 0);
        lv_subject_copy_string(&s.held_role, "");
        lv_subject_set_int(&s.has_held_role, 0);
        lv_subject_set_int(&s.has_detail, 1);
        const char* attrs[] = {"section",     kSection, "index",           kIndex, "role_options",
                               "Other\nDoor", "icon",   "electric_switch", nullptr};
        row_ = create_component("sensor_row", attrs);
        REQUIRE(row_ != nullptr);
        process_lvgl(50);
    }

    lv_obj_t* find(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(row_, name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    bool hidden_in_row(lv_obj_t* obj) const {
        for (; obj && obj != row_; obj = lv_obj_get_parent(obj)) {
            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                return true;
            }
        }
        return false;
    }

    lv_obj_t* row_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(SensorRowFixture, "sensor_row shows the name and state text",
                 "[sensor_row][xml]") {
    CHECK(std::string(lv_label_get_text(find("name_label"))) == "Front Door");
    CHECK(std::string(lv_label_get_text(find("state_label"))) == "Open");

    lv_subject_copy_string(&subjects().state, "Closed");
    process_lvgl(10);
    CHECK(std::string(lv_label_get_text(find("state_label"))) == "Closed");
}

TEST_CASE_METHOD(SensorRowFixture, "sensor_row hides the state when has_state is 0",
                 "[sensor_row][xml]") {
    CHECK_FALSE(hidden_in_row(find("state_label")));
    lv_subject_set_int(&subjects().has_state, 0);
    process_lvgl(10);
    CHECK(hidden_in_row(find("state_label")));
}

TEST_CASE_METHOD(SensorRowFixture, "sensor_row applies the warning style on alert",
                 "[sensor_row][xml]") {
    lv_obj_t* label = find("state_label");
    lv_color_t normal = lv_obj_get_style_text_color(label, LV_PART_MAIN);
    lv_subject_set_int(&subjects().alert, 1);
    process_lvgl(10);
    lv_color_t warn = lv_obj_get_style_text_color(label, LV_PART_MAIN);
    CHECK_FALSE(lv_color_eq(normal, warn));
    lv_subject_set_int(&subjects().alert, 0);
    process_lvgl(10);
    CHECK(lv_color_eq(normal, lv_obj_get_style_text_color(label, LV_PART_MAIN)));
}

TEST_CASE_METHOD(SensorRowFixture, "sensor_row dropdown follows role and has_role",
                 "[sensor_row][xml]") {
    lv_obj_t* dd = find("role_dropdown");
    CHECK_FALSE(hidden_in_row(dd));
    CHECK(lv_dropdown_get_option_count(dd) == 2);
    CHECK(lv_dropdown_get_selected(dd) == 0);

    lv_subject_set_int(&subjects().role, 1);
    process_lvgl(10);
    CHECK(lv_dropdown_get_selected(dd) == 1);

    lv_subject_set_int(&subjects().has_role, 0);
    process_lvgl(10);
    CHECK(hidden_in_row(dd));
}

TEST_CASE_METHOD(SensorRowFixture, "sensor_row toggle label follows toggle_kind",
                 "[sensor_row][xml]") {
    lv_obj_t* enabled = find("toggle_enabled_label");
    lv_obj_t* invert = find("toggle_invert_label");
    lv_obj_t* toggle = find("row_toggle");

    // Kind 0: no toggle at all.
    CHECK(hidden_in_row(toggle));
    CHECK(hidden_in_row(enabled));
    CHECK(hidden_in_row(invert));

    lv_subject_set_int(&subjects().toggle_kind, 1);
    process_lvgl(10);
    CHECK_FALSE(hidden_in_row(toggle));
    CHECK_FALSE(hidden_in_row(enabled));
    CHECK(hidden_in_row(invert));
    CHECK(std::string(lv_label_get_text(enabled)) == "Enabled");

    lv_subject_set_int(&subjects().toggle_kind, 2);
    process_lvgl(10);
    CHECK_FALSE(hidden_in_row(toggle));
    CHECK(hidden_in_row(enabled));
    CHECK_FALSE(hidden_in_row(invert));
    CHECK(std::string(lv_label_get_text(invert)) == "Invert");
}

TEST_CASE_METHOD(SensorRowFixture, "sensor_row toggle checks with the toggle subject",
                 "[sensor_row][xml]") {
    lv_obj_t* toggle = find("row_toggle");
    lv_subject_set_int(&subjects().toggle_kind, 1);
    CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    lv_subject_set_int(&subjects().toggle, 1);
    process_lvgl(10);
    CHECK(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    lv_subject_set_int(&subjects().toggle, 0);
    process_lvgl(10);
    CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_CHECKED));
}

TEST_CASE_METHOD(SensorRowFixture, "sensor_row shows a held role as text beside the dropdown",
                 "[sensor_row][xml]") {
    lv_obj_t* held = find("held_role_label");
    lv_obj_t* dd = find("role_dropdown");
    CHECK(hidden_in_row(held));
    CHECK(lv_obj_get_style_text_opa(dd, LV_PART_MAIN) == LV_OPA_COVER);

    lv_subject_copy_string(&subjects().held_role, "Z Probe");
    lv_subject_set_int(&subjects().has_held_role, 1);
    process_lvgl(10);
    CHECK_FALSE(hidden_in_row(held));
    CHECK(std::string(lv_label_get_text(held)) == "Z Probe");
    // The dropdown stays usable, but its own selection is not drawn.
    CHECK_FALSE(hidden_in_row(dd));
    CHECK(lv_obj_get_style_text_opa(dd, LV_PART_MAIN) == LV_OPA_TRANSP);

    // With no dropdown, the held role still shows, with its Role: label.
    lv_subject_set_int(&subjects().has_role, 0);
    process_lvgl(10);
    CHECK(hidden_in_row(dd));
    CHECK_FALSE(hidden_in_row(held));

    lv_subject_set_int(&subjects().has_held_role, 0);
    process_lvgl(10);
    CHECK(hidden_in_row(find("role_group")));
}

TEST_CASE_METHOD(SensorRowFixture, "sensor_row hides an empty detail line", "[sensor_row][xml]") {
    CHECK_FALSE(hidden_in_row(find("detail_label")));
    lv_subject_set_int(&subjects().has_detail, 0);
    process_lvgl(10);
    CHECK(hidden_in_row(find("detail_label")));
}
