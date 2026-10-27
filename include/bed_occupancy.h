// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Whether a finished print may still be on the bed, decided from the newest history job.
//
// Free of LVGL and of the network layer so it is unit-testable. BedOccupancyState feeds it
// the newest job and the persisted acknowledgement; the plate, the lift rule and the
// Cleared chip read the verdict.

#include "print_history_data.h"

#include <glm/vec2.hpp>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/// The newest history job, reduced to what the decision reads.
struct BedJobSummary {
    std::string job_id;
    std::string filename;
    PrintJobStatus status = PrintJobStatus::UNKNOWN;
    double object_height_mm = 0.0; ///< file metadata; 0 when the slicer did not record it
};

struct BedOccupancyInput {
    std::optional<BedJobSummary> newest; ///< nullopt: no history (fresh install, disabled)
    std::string cleared_job_id;          ///< the job the user tapped Cleared for
    bool print_active = false;           ///< a job is printing or paused right now
};

struct BedOccupancy {
    bool occupied = false;                ///< a part may be on the bed
    std::optional<double> part_height_mm; ///< nullopt when occupied but the height is unknown
    std::string job_id;                   ///< the job the part came from; empty when not occupied
};

/**
 * @brief Decide whether the newest job may have left a part on the bed
 *
 * Occupied when the newest job ended COMPLETED, CANCELLED or ERROR, its id is not
 * @p cleared_job_id, and no print is active. A cancelled or failed job leaves a partial
 * part, and its file's full object height is the conservative bound for it. UNKNOWN and
 * IN_PROGRESS never occupy. part_height_mm is the object height when it is above 0.
 */
[[nodiscard]] BedOccupancy decide_bed_occupancy(const BedOccupancyInput& in);

/// One object's outline in printer millimetres: the slicer's polygon, or its bbox as four
/// corners when the polygon is missing.
struct BedObjectOutline {
    std::vector<glm::vec2> points;
};

/// Outlines captured when a print ended, kept so a Klipper restart does not forget them.
struct BedOutlineSnapshot {
    std::string filename; ///< matched against the newest job's filename
    std::vector<BedObjectOutline> objects;
};

/// At most this many objects and points per object are persisted.
inline constexpr std::size_t kMaxSnapshotObjects = 64;
inline constexpr std::size_t kMaxSnapshotPointsPerObject = 32;

/// Serialise and parse a snapshot for the per-printer config. Parsing is non-throwing: a
/// malformed value yields nullopt. Objects and points past the caps are dropped on write.
[[nodiscard]] std::string bed_outline_snapshot_to_json(const BedOutlineSnapshot& snapshot);
[[nodiscard]] std::optional<BedOutlineSnapshot>
bed_outline_snapshot_from_json(const std::string& text);

} // namespace helix
