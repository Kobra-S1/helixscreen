// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_port_probe.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "platform_info.h"
#include "utils/network_validation.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <utility>

namespace helix {

namespace {

/// One probe sequence. Read and written on the main thread only: the client's callbacks
/// touch nothing but the token until they are deferred.
struct PortProbe {
    explicit PortProbe(LifetimeToken token) : tok(std::move(token)) {}

    std::string host;
    std::vector<int> ports;
    size_t index = 0;
    bool connected = false;
    uint32_t timeout_ms = 0;
    LifetimeToken tok;
    std::function<void(int)> on_connected;
    std::function<void(int)> on_failed;
};

int start_attempt(const std::shared_ptr<PortProbe>& probe);

void attempt_connected(const std::shared_ptr<PortProbe>& probe, size_t attempt) {
    if (attempt != probe->index || probe->connected) {
        return;
    }
    probe->connected = true;
    const int port = probe->ports[attempt];
    if (attempt > 0) {
        spdlog::info("[MoonrakerPortProbe] No Moonraker on {}:{}, found it on :{}", probe->host,
                     probe->ports.front(), port);
    }
    probe->on_connected(port);
}

void attempt_failed(const std::shared_ptr<PortProbe>& probe, size_t attempt) {
    // A superseded attempt's close arrives after the next attempt has started.
    if (attempt != probe->index) {
        return;
    }
    const int port = probe->ports[attempt];
    if (!probe->connected && attempt + 1 < probe->ports.size()) {
        probe->index = attempt + 1;
        spdlog::info("[MoonrakerPortProbe] Nothing answered on {}:{}, trying :{}", probe->host,
                     port, probe->ports[probe->index]);
        if (start_attempt(probe) != 0) {
            probe->on_failed(probe->ports[probe->index]);
        }
        return;
    }
    probe->on_failed(port);
}

int start_attempt(const std::shared_ptr<PortProbe>& probe) {
    IMoonrakerClient* client = get_moonraker_client();
    if (!client) {
        return -1;
    }
    const size_t attempt = probe->index;
    const std::string address = probe->host + ":" + std::to_string(probe->ports[attempt]);

    // MoonrakerClient's on_connected handlers issue REST calls the moment the socket opens,
    // so the HTTP base URL must name this attempt's port before connect().
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        api->set_http_base_url("http://" + address);
    }

    client->set_connection_timeout(probe->timeout_ms);
    const std::string ws_url = "ws://" + address + "/websocket";
    // One deferred body for both outcomes keeps the firmware image to one instantiation.
    auto report = [probe, attempt](bool connected) {
        probe->tok.defer("MoonrakerPortProbe::result", [probe, attempt, connected]() {
            if (connected) {
                attempt_connected(probe, attempt);
            } else {
                attempt_failed(probe, attempt);
            }
        });
    };
    const int result = client->connect(
        ws_url.c_str(), [report]() { report(true); }, [report]() { report(false); });
    client->set_auto_reconnect(false);
    return result;
}

} // namespace

int probe_moonraker_ports(const std::string& host, std::vector<int> ports, uint32_t timeout_ms,
                          LifetimeToken tok, std::function<void(int port)> on_connected,
                          std::function<void(int port)> on_failed) {
    if (ports.empty()) {
        return -1;
    }
    auto probe = std::make_shared<PortProbe>(std::move(tok));
    probe->host = host;
    probe->ports = std::move(ports);
    probe->timeout_ms = timeout_ms;
    probe->on_connected = std::move(on_connected);
    probe->on_failed = std::move(on_failed);
    return start_attempt(probe);
}

std::string connect_failure_status(ConnectFailureKind kind, const std::vector<int>& ports_tried) {
    std::string text = lv_tr(connect_failure_hint(kind, is_android_platform()));
    if (ports_tried.size() > 1) {
        text += " ";
        text += lv_tr("Tried ports 7125 and 80.");
    }
    return text;
}

} // namespace helix
