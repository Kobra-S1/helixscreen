// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_wake_input_gate.cpp
 * @brief #1245: the wake cancels the press LVGL already holds, until the finger lifts.
 *
 * On a pointer the sleep-aware wrapper does not cover (SDL, which Android's touch
 * arrives through), the touch that wakes the display reaches LVGL first. Its
 * pr_timestamp, long_pr_sent and pointer.act_obj would carry a held finger to
 * LV_EVENT_LONG_PRESSED measured from the original touch-down: home-grid edit mode,
 * opened behind the lock screen. wake_display() runs cancel_pointer_presses(), which
 * resets each pointer and makes a pressed one wait for its release.
 *
 * The control case (the same held press DOES long-press when nothing cancels it)
 * keeps the regression assertion from passing on a harness that never drove LVGL.
 * Reads and the clock are driven by hand rather than through process_lvgl(), whose
 * pump runs only one-shot timers and so never fires the indev read timer.
 */

#include "app_constants.h"
#include "display_manager.h"
#include "lvgl_test_fixture.h"
#include "test_helpers/display_manager_test_access.h"

#include "../../catch_amalgamated.hpp"

namespace {

// Where the synthetic finger lands, and the target it lands on. Well inside the
// 800x480 fixture display.
constexpr int TOUCH_X = 120;
constexpr int TOUCH_Y = 120;
constexpr int TARGET_POS = 60;
constexpr int TARGET_SIZE = 160;

// Virtual-clock step between synthetic reads. Finer than the 33ms production
// read period so the long-press threshold is crossed with little overshoot.
constexpr int READ_STEP_MS = 10;

/// State the synthetic indev's read callback reports. File-scope because LVGL
/// retains the callback for as long as the indev lives; ScopedWakeIndev bounds
/// that to one test.
struct WakeIndevState {
    lv_point_t point{0, 0};
    lv_indev_state_t state{LV_INDEV_STATE_RELEASED};
};

WakeIndevState g_wake_indev_state;

void wake_indev_read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->point = g_wake_indev_state.point;
    data->state = g_wake_indev_state.state;
}

int g_pressed_count = 0;
int g_long_pressed_count = 0;
int g_clicked_count = 0;

void count_pressed_cb(lv_event_t* /*e*/) {
    ++g_pressed_count;
}

void count_long_pressed_cb(lv_event_t* /*e*/) {
    ++g_long_pressed_count;
}

void count_clicked_cb(lv_event_t* /*e*/) {
    ++g_clicked_count;
}

/// Owns a pointer indev for one test. lv_indev_create() also arms a periodic
/// read timer; leaving the indev behind would keep that timer (and this
/// callback) alive for the rest of the binary's run.
class ScopedWakeIndev {
  public:
    ScopedWakeIndev() {
        indev_ = lv_indev_create();
        lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev_, wake_indev_read_cb);
        // Production value, applied in DisplayManager::init()
        // (src/application/display_manager.cpp) — LVGL's own default is 400ms.
        lv_indev_set_long_press_time(indev_, AppConstants::Input::LONG_PRESS_MS);
    }
    ~ScopedWakeIndev() {
        g_wake_indev_state.state = LV_INDEV_STATE_RELEASED;
        lv_indev_delete(indev_);
    }
    ScopedWakeIndev(const ScopedWakeIndev&) = delete;
    ScopedWakeIndev& operator=(const ScopedWakeIndev&) = delete;

    /// Hold the current touch state for @p ms of virtual time, reading the
    /// device the way LVGL's own read timer would.
    void hold(int ms) const {
        for (int elapsed = 0; elapsed < ms; elapsed += READ_STEP_MS) {
            lv_tick_inc(READ_STEP_MS);
            lv_indev_read(indev_);
        }
    }

    lv_indev_t* get() const {
        return indev_;
    }

  private:
    lv_indev_t* indev_ = nullptr;
};

/// Target the synthetic finger presses, wired to the two event counters.
lv_obj_t* make_press_target(lv_obj_t* parent) {
    lv_obj_t* target = lv_obj_create(parent);
    lv_obj_remove_flag(target, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(target, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(target, TARGET_POS, TARGET_POS);
    lv_obj_set_size(target, TARGET_SIZE, TARGET_SIZE);
    lv_obj_add_event_cb(target, count_pressed_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(target, count_long_pressed_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(target, count_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(target);
    return target;
}

void reset_counters() {
    g_pressed_count = 0;
    g_long_pressed_count = 0;
    g_clicked_count = 0;
}

} // namespace

// ============================================================================
// Control case — proves the harness really drives LVGL's long-press machinery.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "held press fires LONG_PRESSED once with no wake gate engaged",
                 "[application][display][input][wake][1245]") {
    reset_counters();
    ScopedWakeIndev indev;
    make_press_target(test_screen());

    g_wake_indev_state.point = {TOUCH_X, TOUCH_Y};
    g_wake_indev_state.state = LV_INDEV_STATE_PRESSED;

    // Press registers on the target.
    indev.hold(100);
    REQUIRE(g_pressed_count == 1);
    REQUIRE(g_long_pressed_count == 0); // 100ms < the 500ms threshold

    // Same 700ms total hold the gated test below uses.
    indev.hold(600);

    // Exactly once — further holding produces LONG_PRESSED_REPEAT, not LONG_PRESSED.
    REQUIRE(g_long_pressed_count == 1);
}

// ============================================================================
// Regression: #1245, the wake must cancel the press, not merely pause input.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "the wake's press cancel leaves no long press or click for the held finger",
                 "[application][display][input][wake][1245]") {
    reset_counters();
    ScopedWakeIndev indev;
    make_press_target(test_screen());

    g_wake_indev_state.point = {TOUCH_X, TOUCH_Y};
    g_wake_indev_state.state = LV_INDEV_STATE_PRESSED;

    // The wake touch lands and LVGL starts counting toward the long press.
    indev.hold(100);
    REQUIRE(g_pressed_count == 1);
    REQUIRE(g_long_pressed_count == 0);

    // wake_display() runs while the finger is still down.
    DisplayManagerTestAccess::cancel_pointer_presses();

    // Held well past the 500ms threshold from the original touch-down, then lifted.
    indev.hold(600);
    CHECK(g_long_pressed_count == 0);
    g_wake_indev_state.state = LV_INDEV_STATE_RELEASED;
    indev.hold(50);
    CHECK(g_clicked_count == 0);

    // The next touch is an ordinary one.
    g_wake_indev_state.state = LV_INDEV_STATE_PRESSED;
    indev.hold(50);
    g_wake_indev_state.state = LV_INDEV_STATE_RELEASED;
    indev.hold(50);
    CHECK(g_clicked_count == 1);
}
