// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_occupancy.h"

#include "json_utils.h"

#include <algorithm>

#include "hv/json.hpp"

namespace helix {

namespace {

constexpr const char* kFilenameField = "filename";
constexpr const char* kObjectsField = "objects";

bool status_leaves_part(PrintJobStatus status) {
    switch (status) {
    case PrintJobStatus::COMPLETED:
    case PrintJobStatus::CANCELLED:
    case PrintJobStatus::ERROR:
        return true;
    default:
        return false;
    }
}

} // namespace

BedOccupancy decide_bed_occupancy(const BedOccupancyInput& in) {
    BedOccupancy out;
    if (in.print_active || !in.newest || !status_leaves_part(in.newest->status)) {
        return out;
    }
    if (!in.cleared_job_id.empty() && in.newest->job_id == in.cleared_job_id) {
        return out;
    }
    out.occupied = true;
    out.job_id = in.newest->job_id;
    if (in.newest->object_height_mm > 0.0) {
        out.part_height_mm = in.newest->object_height_mm;
    }
    return out;
}

std::string bed_outline_snapshot_to_json(const BedOutlineSnapshot& snapshot) {
    nlohmann::json objects = nlohmann::json::array();
    const std::size_t object_count = std::min(snapshot.objects.size(), kMaxSnapshotObjects);
    for (std::size_t i = 0; i < object_count; ++i) {
        const auto& points = snapshot.objects[i].points;
        nlohmann::json pts = nlohmann::json::array();
        const std::size_t point_count = std::min(points.size(), kMaxSnapshotPointsPerObject);
        for (std::size_t p = 0; p < point_count; ++p) {
            pts.push_back(nlohmann::json::array({points[p].x, points[p].y}));
        }
        objects.push_back(std::move(pts));
    }
    nlohmann::json root = {{kFilenameField, snapshot.filename},
                           {kObjectsField, std::move(objects)}};
    return json_util::safe_dump(root);
}

std::optional<BedOutlineSnapshot> bed_outline_snapshot_from_json(const std::string& text) {
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json* objects = json_util::find_member(root, kObjectsField);
    if (objects == nullptr || !objects->is_array()) {
        return std::nullopt;
    }
    BedOutlineSnapshot snapshot;
    snapshot.filename = json_util::safe_string(root, kFilenameField);
    for (const auto& object : *objects) {
        if (snapshot.objects.size() >= kMaxSnapshotObjects) {
            break;
        }
        if (!object.is_array()) {
            return std::nullopt;
        }
        BedObjectOutline outline;
        for (const auto& point : object) {
            if (outline.points.size() >= kMaxSnapshotPointsPerObject) {
                break;
            }
            if (!point.is_array() || point.size() != 2 || !point[0].is_number() ||
                !point[1].is_number()) {
                return std::nullopt;
            }
            outline.points.emplace_back(json_util::as_float(point[0]),
                                        json_util::as_float(point[1]));
        }
        snapshot.objects.push_back(std::move(outline));
    }
    return snapshot;
}

} // namespace helix
