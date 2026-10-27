// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_occupancy_state.h"

#include "config.h"
#include "connection_state.h"
#include "observer_factory.h"
#include "print_lifecycle_state.h"
#include "printer_excluded_objects_state.h"
#include "printer_network_state.h"
#include "printer_print_state.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {

constexpr const char* kRegistryName = "BedOccupancyState";

BedOccupancyState* g_bed_occupancy_state = nullptr;

bool lifecycle_ends_print(PrintState state) {
    return state == PrintState::Complete || state == PrintState::Cancelled ||
           state == PrintState::Error;
}

bool same_verdict(const BedOccupancy& a, const BedOccupancy& b) {
    return a.occupied == b.occupied && a.part_height_mm == b.part_height_mm && a.job_id == b.job_id;
}

std::string config_path(Config& config, const char* key) {
    return config.df() + key;
}

/// Each defined object's slicer polygon, or its bbox as four corners when it has none.
std::vector<BedObjectOutline> defined_outlines(const PrinterExcludedObjectsState& objects) {
    std::vector<BedObjectOutline> out;
    for (const auto& name : objects.get_defined_objects()) {
        const auto info = objects.get_object_geometry(name);
        if (!info) {
            continue;
        }
        BedObjectOutline outline;
        if (!info->polygon.empty()) {
            outline.points = info->polygon;
        } else if (info->has_bbox) {
            outline.points = {info->bbox_min,
                              {info->bbox_max.x, info->bbox_min.y},
                              info->bbox_max,
                              {info->bbox_min.x, info->bbox_max.y}};
        } else {
            continue;
        }
        out.push_back(std::move(outline));
    }
    return out;
}

} // namespace

BedOccupancyState* get_bed_occupancy_state() {
    return g_bed_occupancy_state;
}

void set_bed_occupancy_state(BedOccupancyState* state) {
    g_bed_occupancy_state = state;
}

BedOccupancyState::BedOccupancyState(PrinterState& printer_state, PrintHistoryManager* history)
    : printer_state_(printer_state), history_(history) {
    if (Config* config = Config::get_instance()) {
        const auto text =
            config->get<std::string>(config_path(*config, kBedOutlineSnapshotKey), "");
        if (!text.empty()) {
            snapshot_ = bed_outline_snapshot_from_json(text);
        }
    }
}

BedOccupancyState::~BedOccupancyState() {
    detach_history();
    lifecycle_observer_.reset();
    connection_observer_.reset();
    objects_observer_.reset();
    if (subjects_initialized_) {
        deinit_subjects();
        StaticSubjectRegistry::instance().unregister(kRegistryName);
    }
}

void BedOccupancyState::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(occupied_, 0, "bed_occupied", subjects_);
    UI_MANAGED_SUBJECT_INT(version_, 0, "bed_occupancy_version", subjects_);
    subjects_initialized_ = true;
    StaticSubjectRegistry::instance().register_deinit(kRegistryName,
                                                      [this]() { deinit_subjects(); });

    if (history_ && !history_attached_) {
        history_cb_ = [this]() { on_history_changed(); };
        history_->add_observer(&history_cb_);
        history_attached_ = true;
    }

    last_lifecycle_ = printer_state_.print_state().get_print_lifecycle();
    const SubjectLifetime lifetime = printer_state_.get_subjects_lifetime();
    lifecycle_observer_ = ui::observe_print_lifecycle(
        printer_state_.print_state().get_print_lifecycle_subject(), this,
        [](BedOccupancyState* self, PrintState state) { self->on_print_lifecycle(state); },
        lifetime);
    connection_observer_ = ui::observe<int>(
        printer_state_.network_state().get_printer_connection_state_subject(), this,
        [](BedOccupancyState* self, int state) {
            if (state == static_cast<int>(ConnectionState::CONNECTED) && self->history_) {
                self->history_->ensure_loaded(HistoryScope::RECENT);
            }
        },
        lifetime);
    objects_observer_ = ui::observe<int>(
        printer_state_.excluded_objects_state().get_defined_objects_version_subject(), this,
        [](BedOccupancyState* self, int /*version*/) { self->bump_version(); }, lifetime);

    refresh();
    spdlog::debug("[BedOccupancy] Subjects initialized");
}

void BedOccupancyState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    lifecycle_observer_.reset();
    connection_observer_.reset();
    objects_observer_.reset();
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void BedOccupancyState::detach_history() {
    if (history_ && history_attached_) {
        history_->remove_observer(&history_cb_);
    }
    history_attached_ = false;
    history_ = nullptr;
}

std::optional<BedJobSummary> BedOccupancyState::newest_job() const {
    // The newest job, not the newest whose file still exists: a part is on the bed even
    // when its file was deleted.
    if (!history_ || history_->get_jobs().empty()) {
        return std::nullopt;
    }
    const PrintHistoryJob& job = history_->get_jobs().front();
    BedJobSummary summary;
    summary.job_id = job.job_id;
    summary.filename = job.filename;
    summary.status = job.status;
    summary.object_height_mm = job.object_height;
    return summary;
}

void BedOccupancyState::refresh() {
    BedOccupancyInput in;
    in.newest = newest_job();
    in.print_active = job_holds_machine(printer_state_.print_state().get_print_lifecycle());
    if (Config* config = Config::get_instance()) {
        in.cleared_job_id = config->get<std::string>(config_path(*config, kBedClearedJobKey), "");
    }

    const BedOccupancy verdict = decide_bed_occupancy(in);
    if (same_verdict(verdict, current_)) {
        return;
    }
    current_ = verdict;

    const char* reason = "the newest job may have left a part";
    if (!verdict.occupied) {
        if (in.print_active) {
            reason = "a print holds the machine";
        } else if (!in.newest) {
            reason = "no print history";
        } else if (in.newest->job_id == in.cleared_job_id) {
            reason = "the user cleared the bed";
        } else {
            reason = "the newest job left no part";
        }
    }
    spdlog::info("[BedOccupancy] {} (job '{}', height {}): {}",
                 verdict.occupied ? "occupied" : "clear",
                 in.newest ? in.newest->job_id : std::string(),
                 verdict.part_height_mm ? fmt::format("{:.2f}mm", *verdict.part_height_mm)
                                        : std::string("unknown"),
                 reason);

    if (subjects_initialized_) {
        lv_subject_set_int(&occupied_, verdict.occupied ? 1 : 0);
    }
    bump_version();
}

void BedOccupancyState::bump_version() {
    if (subjects_initialized_) {
        lv_subject_set_int(&version_, lv_subject_get_int(&version_) + 1);
    }
}

std::vector<BedObjectOutline> BedOccupancyState::outlines() const {
    const bool print_active = job_holds_machine(printer_state_.print_state().get_print_lifecycle());
    if (!current_.occupied && !print_active) {
        return {};
    }

    std::vector<BedObjectOutline> live = defined_outlines(printer_state_.excluded_objects_state());
    if (!live.empty() || print_active) {
        return live;
    }

    const auto newest = newest_job();
    if (snapshot_ && newest && snapshot_->filename == newest->filename) {
        return snapshot_->objects;
    }
    return {};
}

void BedOccupancyState::mark_cleared() {
    if (!current_.occupied) {
        return;
    }
    if (Config* config = Config::get_instance()) {
        config->set<std::string>(config_path(*config, kBedClearedJobKey), current_.job_id);
        config->save();
    }
    spdlog::info("[BedOccupancy] User cleared the bed after job '{}'", current_.job_id);
    refresh();
}

void BedOccupancyState::on_history_changed() {
    refresh();
}

void BedOccupancyState::on_print_lifecycle(PrintState state) {
    const PrintState previous = last_lifecycle_;
    last_lifecycle_ = state;
    if (job_holds_machine(previous) && lifecycle_ends_print(state)) {
        capture_outline_snapshot();
    }
    refresh();
}

void BedOccupancyState::capture_outline_snapshot() {
    BedOutlineSnapshot snapshot;
    snapshot.filename = printer_state_.print_state().get_effective_print_filename();
    snapshot.objects = defined_outlines(printer_state_.excluded_objects_state());
    if (Config* config = Config::get_instance()) {
        config->set<std::string>(config_path(*config, kBedOutlineSnapshotKey),
                                 bed_outline_snapshot_to_json(snapshot));
        config->save();
    }
    spdlog::info("[BedOccupancy] Captured {} object outline(s) for '{}'", snapshot.objects.size(),
                 snapshot.filename);
    snapshot_ = std::move(snapshot);
    bump_version();
}

} // namespace helix
