// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_sensor_manager.h"
#include "input_sensor_manager.h"
#include "load_cell_manager.h"
#include "probe_sensor_manager.h"
#include "sensor_manager.h"
#include "temperature_sensor_manager.h"

namespace helix::sensors {

/// Calls fn on every Klipper sensor manager singleton. The one list of them:
/// subject init and the status fan-out both walk it, so a manager added here
/// gets both. Discovery is per manager, because each reads a different input.
template <typename Fn> void for_each_sensor_manager(Fn&& fn) {}

} // namespace helix::sensors
