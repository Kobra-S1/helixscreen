// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "connection_state.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief Connect the live Moonraker client to @p host on each of @p ports in turn
 *
 * The connection-test sequence shared by the setup wizard and the Change Host / Add Printer
 * modal. Each attempt sets the API's HTTP base URL, connects with auto-reconnect off and
 * waits for the WebSocket to open; a failed attempt moves silently to the next port. Pair it
 * with moonraker_ports_to_try() for the port list.
 *
 * Main thread only. Both callbacks run on the main thread through @p tok, so invalidating
 * the owner's lifetime abandons the rest of the sequence.
 *
 * @param on_connected Moonraker answered on this port
 * @param on_failed    The last port failed, or a connection that had opened dropped; the
 *                     port is the one that was being tried or was connected
 * @return connect()'s result for the first attempt: nonzero (or -1 with no client) means the
 *         test could not start and neither callback will run
 */
int probe_moonraker_ports(const std::string& host, std::vector<int> ports, uint32_t timeout_ms,
                          LifetimeToken tok, std::function<void(int port)> on_connected,
                          std::function<void(int port)> on_failed);

/**
 * @brief The translated status line for a failed connection test
 *
 * The hint for @p kind, followed by the ports tried when the test tried more than one.
 */
std::string connect_failure_status(ConnectFailureKind kind, const std::vector<int>& ports_tried);

} // namespace helix
