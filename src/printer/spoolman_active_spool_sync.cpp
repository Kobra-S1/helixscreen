// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_active_spool_sync.h"

#include "ui_update_queue.h"

#include "ams_state.h"
#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"

#include <spdlog/spdlog.h>

namespace helix::spoolman_sync {

void request_startup_sync(ISpoolmanAPI& spoolman, ApplySpoolFn apply) {
    // The reply arrives on the WebSocket thread and sync_from_status() reads AmsState.
    spoolman.get_spoolman_status(
        [&spoolman, apply = std::move(apply)](bool connected, int active_spool_id) {
            helix::ui::queue_update("spoolman_sync::sync_from_status", [&spoolman, apply, connected,
                                                                        active_spool_id]() {
                sync_from_status(spoolman, connected, active_spool_id, apply);
            });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[Application] Spoolman status unavailable: {}", err.message);
        },
        true); // silent: Spoolman not configured is normal
}

void sync_from_status(ISpoolmanAPI& spoolman, bool connected, int active_spool_id,
                      const ApplySpoolFn& apply) {
    if (!connected || active_spool_id <= 0) {
        spdlog::debug("[Application] No active Spoolman spool to sync "
                      "(connected={}, spool_id={})",
                      connected, active_spool_id);
        return;
    }

    auto existing = AmsState::instance().get_external_spool_info();
    if (existing && existing->spoolman_id == active_spool_id) {
        spdlog::debug("[Application] External spool already matches active "
                      "Spoolman spool {}",
                      active_spool_id);
        return;
    }

    spdlog::info("[Application] Syncing external spool from Moonraker active "
                 "spool {}",
                 active_spool_id);
    spoolman.get_spoolman_spool(
        active_spool_id,
        [active_spool_id, apply](const std::optional<SpoolInfo>& spool_opt) {
            if (!spool_opt) {
                spdlog::warn("[Application] Active spool {} not found in Spoolman",
                             active_spool_id);
                return;
            }
            apply(*spool_opt, "synced");
        },
        [active_spool_id](const MoonrakerError& err) {
            spdlog::warn("[Application] Failed to fetch active spool {}: {}", active_spool_id,
                         err.message);
        });
}

} // namespace helix::spoolman_sync
