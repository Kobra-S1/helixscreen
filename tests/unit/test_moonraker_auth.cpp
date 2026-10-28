// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_auth.cpp
 * @brief The Moonraker API key reaches the printer it belongs to, over HTTP and
 *        the WebSocket upgrade, and nothing else.
 *
 * Run with: ./build/bin/helix-tests "[moonraker-auth]"
 */

#include "../mocks/mock_websocket_server.h"
#include "../test_helpers/loopback_http_server.h"
#include "config.h"
#include "hv/EventLoopThread.h"
#include "hv/requests.h"
#include "moonraker_auth.h"
#include "moonraker_auth_http.h"
#include "moonraker_client.h"
#include "printer_retarget.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::moonraker_auth;

namespace {

/// Leaves no credential behind for the next test.
struct CredentialGuard {
    ~CredentialGuard() {
        set_credential({});
    }
};

} // namespace

TEST_CASE("moonraker auth: a printer entry's key", "[moonraker-auth]") {
    SECTION("a string key is trimmed") {
        const auto c = credential_from_printer({{"moonraker_host", "printer.local"},
                                                {"moonraker_port", 7125},
                                                {"moonraker_api_key", "  abc123  "}});
        CHECK(c.host == "printer.local");
        CHECK(c.port == 7125);
        CHECK(c.api_key == "abc123");
    }
    SECTION("false, absent and blank all mean no key") {
        CHECK(credential_from_printer({{"moonraker_api_key", false}}).api_key.empty());
        CHECK(credential_from_printer({{"moonraker_host", "h"}}).api_key.empty());
        CHECK(credential_from_printer({{"moonraker_api_key", "   "}}).api_key.empty());
    }
    SECTION("not an object") {
        CHECK(credential_from_printer(nullptr).host.empty());
    }
}

TEST_CASE("moonraker auth: the key goes only to its own printer", "[moonraker-auth]") {
    CredentialGuard guard;
    set_credential({"Printer.LAN", 7125, "k"});

    CHECK(key_for("printer.lan", 7125) == "k");
    CHECK(key_for("printer.lan", 7126).empty());
    CHECK(key_for("other.lan", 7125).empty());

    CHECK(key_for_url("ws://printer.lan:7125/websocket") == "k");
    CHECK(key_for_url("http://printer.lan:7125/server/info?x=1") == "k");
    CHECK(key_for_url("http://printer.lan/server/info").empty());
    CHECK(key_for_url("http://printer.lan:80/server/info").empty());

    set_credential({"::1", 7125, "v6"});
    CHECK(key_for_url("ws://[::1]:7125/websocket") == "v6");
    CHECK(key_for_url("ws://[::1]/websocket").empty());

    set_credential({"printer.lan", 7125, ""});
    CHECK(key_for("printer.lan", 7125).empty());
}

TEST_CASE("moonraker auth: what a request does with its header", "[moonraker-auth]") {
    CredentialGuard guard;
    set_credential({"printer.lan", 7125, "k"});

    CHECK(header_action("printer.lan", 7125, "") == HeaderAction::Set);
    CHECK(header_action("printer.lan", 7125, "stale") == HeaderAction::Set);
    CHECK(header_action("printer.lan", 7125, "k") == HeaderAction::Leave);
    CHECK(header_action("webcam.lan", 80, "") == HeaderAction::Leave);
    // Another service's key belongs to the caller that set it.
    CHECK(header_action("telemetry.example", 443, "other") == HeaderAction::Leave);
    // Ours, carried to a host it does not belong to (a redirect), comes off.
    CHECK(header_action("elsewhere.example", 443, "k") == HeaderAction::Remove);
}

TEST_CASE("moonraker auth: every libhv request to the printer carries the key",
          "[moonraker-auth]") {
    CredentialGuard guard;
    install_http_request_hook();

    std::mutex m;
    std::string seen;
    test::LoopbackHttpServer server;
    server.service().GET("/server/info", [&](HttpRequest* req, HttpResponse* resp) {
        std::lock_guard<std::mutex> lock(m);
        seen = req->GetHeader(kApiKeyHeader);
        return resp->String("{}");
    });
    REQUIRE(server.start());
    const std::string url = server.url("/server/info");
    const int port = std::stoi(url.substr(url.rfind(':') + 1));

    auto get_and_read = [&](const http_headers& headers = {}) {
        {
            std::lock_guard<std::mutex> lock(m);
            seen = "<none>";
        }
        auto resp = requests::get(url.c_str(), headers);
        REQUIRE(resp);
        REQUIRE(resp->status_code == HTTP_STATUS_OK);
        std::lock_guard<std::mutex> lock(m);
        return seen;
    };

    set_credential({"127.0.0.1", port, "secret"});
    CHECK(get_and_read() == "secret");

    // Another port is another server: it gets no key.
    set_credential({"127.0.0.1", port + 1, "secret"});
    CHECK(get_and_read().empty());

    // A key the caller set for that server stays as the caller set it.
    CHECK(get_and_read({{kApiKeyHeader, "callers-own"}}) == "callers-own");
}

TEST_CASE("moonraker auth: the WebSocket upgrade carries the key",
          "[moonraker-auth][moonraker][client]") {
    CredentialGuard guard;
    MockWebSocketServer server;
    REQUIRE(server.start(0) > 0);
    set_credential({"127.0.0.1", server.port(), "ws-secret"});

    auto loop = std::make_shared<hv::EventLoopThread>();
    loop->start();
    auto client = std::make_unique<MoonrakerClient>(loop->loop());
    client->connect(server.url().c_str(), []() {}, []() {});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && server.connection_count() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(server.connection_count() > 0);
    CHECK(server.last_upgrade_header(kApiKeyHeader) == "ws-secret");

    client->disconnect();
    loop->stop();
    loop->join();
    client.reset();
}

TEST_CASE("moonraker auth: the active printer's saved key", "[moonraker-auth]") {
    Config* config = Config::get_instance();
    const std::string df = config->df();
    const nlohmann::json* before = config->try_get_json(df + "moonraker_api_key");
    const nlohmann::json saved = before ? *before : nlohmann::json(false);

    config->set<std::string>(df + "moonraker_api_key", "from-settings");
    const Credential c = active_printer_credential();
    CHECK(c.api_key == "from-settings");
    CHECK(c.host == config->get<std::string>(df + "moonraker_host", ""));

    config->set<bool>(df + "moonraker_api_key", false);
    CHECK(active_printer_credential().api_key.empty());

    config->get_json(df + "moonraker_api_key") = saved;
}
