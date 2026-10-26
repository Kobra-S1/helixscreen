// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_sidebar_batch_steps.cpp
 * @brief The step bar a head-picker batch shows on a changer that mounts to feed.
 *
 * The batch path names its operation to the bar. Left to read the first frame,
 * the bar takes an unload of a loaded head for a tool swap and an unload of an
 * empty one for a load.
 */

#include "ui_ams_sidebar.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "filament_action_set.h"
#include "mount_feed_sequence.h"
#include "test_helpers/ams_sidebar_xml.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsState;
using helix::StepOperationType;
using helix::ui::HeadPickOp;
using helix::ui::HeadPickRow;

namespace {

/// A mount-to-feed changer whose stage acks the test hands out.
class FeedChanger : public helix::test::ToolChangerHelper {
  public:
    explicit FeedChanger(int tools) : ToolChangerHelper(tools) {
        set_mounts_to_feed(true);
    }

    helix::AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete,
                                  std::function<void(const MoonrakerError&)> /*on_error*/,
                                  bool /*silent*/) override {
        sent_.push_back(gcode);
        acks_.push_back(std::move(on_complete));
        return helix::AmsErrorHelper::success();
    }

    /// Ack the newest stage and let the deferred handlers run.
    void ack() {
        REQUIRE_FALSE(acks_.empty());
        auto cb = acks_.back();
        cb();
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

  private:
    std::vector<std::function<void()>> acks_;
};

struct BatchStepsFixture : public XMLTestFixture {
    explicit BatchStepsFixture(int tools = 6) {
        auto owned = std::make_unique<FeedChanger>(tools);
        changer_ = owned.get();
        changer_->feed_status("ready", 0); // tool 1 on the carriage, loaded
        AmsState::instance().set_backend(std::move(owned));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();

        helix::test::register_ams_sidebar_xml();
        panel_ = lv_obj_create(test_screen());
        const char* attrs[] = {"name", "ams_operation_sidebar", nullptr};
        REQUIRE(lv_xml_create(panel_, "ams_sidebar", attrs) != nullptr);
        sidebar_ = std::make_unique<helix::ui::AmsOperationSidebar>(state());
        REQUIRE(sidebar_->setup(panel_));
        sidebar_->init_observers();
        // The frames the setup fed land before the user taps anything.
        helix::ui::UpdateQueue::instance().drain();
    }

    ~BatchStepsFixture() override {
        sidebar_.reset();
        if (panel_ && lv_obj_is_valid(panel_)) {
            lv_obj_delete(panel_);
        }
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().set_backend(nullptr);
    }

    void pick(HeadPickOp op, const std::vector<int>& slots) {
        std::vector<HeadPickRow> rows;
        for (int slot : slots) {
            rows.push_back({slot, slot, true, "", true});
        }
        sidebar_->run_picked(op, rows);
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE_FALSE(changer_->sent_.empty()); // the batch reached the backend
        REQUIRE(lv_obj_is_valid(panel_));
    }

    /// Every row label the step bar on screen shows, in order.
    [[nodiscard]] std::vector<std::string> labels() const {
        lv_obj_t* container = lv_obj_find_by_name(panel_, "progress_stepper_container");
        REQUIRE(container != nullptr);
        REQUIRE(lv_obj_get_child_count(container) > 0);
        lv_obj_t* bar = lv_obj_get_child(container, 0);
        std::vector<std::string> out;
        // Step rows carry an indicator and a label; the connectors carry nothing.
        for (uint32_t i = 0; i < lv_obj_get_child_count(bar); ++i) {
            lv_obj_t* item = lv_obj_get_child(bar, static_cast<int32_t>(i));
            if (lv_obj_get_child_count(item) >= 2) {
                out.emplace_back(lv_label_get_text(lv_obj_get_child(item, 1)));
            }
        }
        return out;
    }

    FeedChanger* changer_ = nullptr;
    std::unique_ptr<helix::ui::AmsOperationSidebar> sidebar_;
    lv_obj_t* panel_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(BatchStepsFixture,
                 "A batch unload of a loaded head shows the unload rows of every tool",
                 "[ams][sidebar][mount_feed][batch]") {
    pick(HeadPickOp::Unload, {0, 1, 2, 3, 4, 5});

    CHECK(AmsState::instance().get_active_step_operation() == StepOperationType::UNLOAD);
    const auto shown = labels();
    REQUIRE(shown.size() == helix::mount_feed::bar_rows(/*load=*/false, 6).size());
    CHECK(shown.front() == "Tool 1 of 6: Mount tool");
    CHECK(shown[2] == "Tool 1 of 6: Retract filament");
    CHECK(shown.back() == "Tool 6 of 6: Restore tool");
}

TEST_CASE_METHOD(BatchStepsFixture, "A batch load shows the load rows of every tool",
                 "[ams][sidebar][mount_feed][batch]") {
    pick(HeadPickOp::Load, {1, 2, 3});

    CHECK(AmsState::instance().get_active_step_operation() == StepOperationType::LOAD_FRESH);
    const auto shown = labels();
    REQUIRE(shown.size() == helix::mount_feed::bar_rows(/*load=*/true, 3).size());
    CHECK(shown.front() == "Tool 1 of 3: Insert filament");
    CHECK(shown.back() == "Tool 3 of 3: Restore tool");
}

TEST_CASE_METHOD(BatchStepsFixture, "The live temperature follows the heat row under way",
                 "[ams][sidebar][mount_feed][batch]") {
    pick(HeadPickOp::Unload, {0, 1, 2});
    // Tool 1 is already on the carriage: the run opens on its heat row.
    auto shown = labels();
    REQUIRE(shown[1].rfind("Tool 1 of 3: Heat nozzle ", 0) == 0);
    CHECK(shown[1].find("°C") != std::string::npos);
    CHECK(shown[4] == "Tool 2 of 3: Heat nozzle");

    changer_->ack(); // tool 1 heated
    changer_->ack(); // tool 1 retracted
    changer_->ack(); // tool 2 mounted
    shown = labels();
    CHECK(shown[1] == "Tool 1 of 3: Heat nozzle");
    REQUIRE(shown[4].rfind("Tool 2 of 3: Heat nozzle ", 0) == 0);
    CHECK(shown[4].find("°C") != std::string::npos);
}
