// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_snapmaker.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"

#include <chrono>
#include <mutex>
#include <string>

#include "hv/json.hpp"

// Friend shim (declared as friend in ams_backend_snapmaker.h) to reach the
// protected handle_status_update() so tests can drive slot state through the
// real firmware parse. Distinct from test_ams_backend_snapmaker.cpp's
// SnapmakerTestAccess to avoid an ODR clash when both TUs link. Defined in
// ONE header so every translation unit that needs it shares one definition.
class RunoutScopeTestAccess {
  public:
    static void handle_status(AmsBackendSnapmaker& b, const nlohmann::json& n) {
        b.handle_status_update(n);
    }

    // Set a sensor's role directly, bypassing set_sensor_role()'s single-RUNOUT
    // exclusivity. Production reaches this multi-RUNOUT state via load_config
    // (settings.json restore), which writes sensor->role per entry with no
    // exclusivity check — exactly how a Snapmaker U1's four e{N}_filament
    // sensors all end up RUNOUT-roled.
    static void force_role(helix::FilamentSensorManager& mgr, const std::string& klipper,
                           helix::FilamentSensorRole role) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
        for (auto& s : mgr.sensors_) {
            if (s.klipper_name == klipper) {
                s.role = role;
                s.enabled = true;
                return;
            }
        }
    }

    // Reset the manager to a clean slate (sensors + states cleared, grace
    // period already expired) so each test starts deterministic despite the
    // process-singleton.
    static void reset(helix::FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
        mgr.sensors_.clear();
        mgr.states_.clear();
        mgr.master_enabled_ = true;
        mgr.sync_mode_ = true;
        mgr.initial_status_received_ = false;
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    }
};
