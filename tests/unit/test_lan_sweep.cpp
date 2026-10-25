// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../mocks/mock_mdns_discovery.h"
#include "../test_helpers/update_queue_test_access.h"
#include "lan_sweep.h"
#include "printer_discovery_list.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr uint32_t ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a << 24) | (b << 16) | (c << 8) | d;
}

LocalIpv4 net(uint32_t address, uint32_t netmask) {
    return LocalIpv4{address, netmask};
}

void drain() {
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

/// Drain the UI queue until @p done or two seconds pass.
template <typename F> bool pump_until(F done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    drain();
    return true;
}

} // namespace

// ============================================================================
// Which addresses a sweep asks
// ============================================================================

TEST_CASE("LAN sweep: a /24 is every host address, ours included", "[lan_sweep]") {
    const auto hosts = lan_sweep_hosts(net(ip(192, 168, 1, 42), ip(255, 255, 255, 0)));
    REQUIRE(hosts.size() == 254);
    CHECK(hosts.front() == "192.168.1.1");
    CHECK(hosts.back() == "192.168.1.254");
    // A printer running HelixScreen lists its own Moonraker.
    CHECK(std::find(hosts.begin(), hosts.end(), "192.168.1.42") != hosts.end());
    CHECK(std::find(hosts.begin(), hosts.end(), "192.168.1.52") != hosts.end());
}

TEST_CASE("LAN sweep: a wider network is narrowed to our /24", "[lan_sweep]") {
    const auto hosts = lan_sweep_hosts(net(ip(10, 4, 7, 9), ip(255, 0, 0, 0)));
    REQUIRE(hosts.size() == 254);
    CHECK(hosts.front() == "10.4.7.1");
    CHECK(hosts.back() == "10.4.7.254");
    CHECK(lan_sweep_hosts(net(ip(172, 16, 5, 9), ip(255, 255, 0, 0))).size() == 254);
}

TEST_CASE("LAN sweep: a narrower network is kept", "[lan_sweep]") {
    const auto hosts = lan_sweep_hosts(net(ip(192, 168, 1, 42), ip(255, 255, 255, 128)));
    REQUIRE(hosts.size() == 126);
    CHECK(hosts.front() == "192.168.1.1");
    CHECK(hosts.back() == "192.168.1.126");
}

TEST_CASE("LAN sweep: a public or unknown network is not swept", "[lan_sweep]") {
    CHECK(lan_sweep_hosts(net(ip(8, 8, 8, 8), ip(255, 255, 255, 0))).empty());
    CHECK(lan_sweep_hosts(net(ip(172, 32, 0, 5), ip(255, 255, 255, 0))).empty());
    CHECK(lan_sweep_hosts(net(ip(192, 169, 1, 5), ip(255, 255, 255, 0))).empty());
    CHECK(lan_sweep_hosts(net(0, 0)).empty());
}

// ============================================================================
// What counts as Moonraker
// ============================================================================

TEST_CASE("LAN sweep: only Moonraker's /server/info answer counts", "[lan_sweep]") {
    const std::string moonraker = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
                                  R"({"result":{"klippy_connected":true,"klippy_state":"ready",)"
                                  R"("components":["file_manager"],"moonraker_version":"v0.9.3"}})";
    CHECK(is_moonraker_server_info_response(moonraker));

    // A printer firmware that blanks the version still carries Moonraker's fields.
    CHECK(is_moonraker_server_info_response(
        "HTTP/1.0 200 OK\r\n\r\n"
        R"({"result":{"klippy_state":"ready","components":[],"moonraker_version":"?"}})"));

    CHECK_FALSE(is_moonraker_server_info_response(
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html>Router login</html>"));
    CHECK_FALSE(is_moonraker_server_info_response(
        "HTTP/1.1 404 Not Found\r\n\r\n"
        R"({"result":{"klippy_state":"ready","components":[],"moonraker_version":"x"}})"));
    CHECK_FALSE(is_moonraker_server_info_response("HTTP/1.1 200 OK\r\n\r\n"
                                                  R"({"result":{"status":"ok"}})"));
    CHECK_FALSE(is_moonraker_server_info_response("HTTP/1.1 200 OK\r\n\r\n"
                                                  R"({"result":"moonraker_version"})"));
    CHECK_FALSE(is_moonraker_server_info_response(""));

    // Moonraker behind its login still counts; another server's 401 does not.
    CHECK(is_moonraker_server_info_response(
        "HTTP/1.1 401 Unauthorized\r\n\r\n"
        R"({"error": {"code": 401, "message": "Unauthorized", "traceback": ""}})"));
    CHECK(is_moonraker_server_info_response("HTTP/1.1 403 Forbidden\r\n\r\n"
                                            R"({"error":{"code":403,"message":"Forbidden"}})"));
    CHECK_FALSE(is_moonraker_server_info_response(
        "HTTP/1.1 401 Unauthorized\r\n\r\n<html>Router login</html>"));
    CHECK_FALSE(
        is_moonraker_server_info_response("HTTP/1.1 404 Not Found\r\n\r\n"
                                          R"({"error":{"code":404,"message":"Not Found"}})"));

    // Only the result object's own keys count.
    CHECK_FALSE(
        is_moonraker_server_info_response("HTTP/1.1 200 OK\r\n\r\n"
                                          R"({"result":{"inner":{"moonraker_version":"x"}}})"));
    CHECK_FALSE(is_moonraker_server_info_response(
        "HTTP/1.1 200 OK\r\n\r\n"
        R"({"result":{"inner":{"a":1,"moonraker_version":"x"}}})"));
    CHECK_FALSE(is_moonraker_server_info_response(
        "HTTP/1.1 200 OK\r\n\r\n"
        R"({"result":{"note":"moonraker_version","list":["klippy_state","components"]}})"));
    CHECK_FALSE(
        is_moonraker_server_info_response("HTTP/1.1 200 OK\r\n\r\n"
                                          R"({"other":{"moonraker_version":"x"},"result":{}})"));
    CHECK(is_moonraker_server_info_response(
        "HTTP/1.1 200 OK\r\n\r\n"
        R"({"result":{"note":"a \"quoted\" {brace}","moonraker_version":"v1"}})"));
    CHECK_FALSE(is_moonraker_server_info_response("HTTP/1.1 200 OK\r\n\r\n{\"result\":"));
}

// ============================================================================
// Merging with mDNS, and the dropdown
// ============================================================================

TEST_CASE("Printer discovery: one entry per address, mDNS's name kept", "[lan_sweep]") {
    const std::vector<DiscoveredPrinter> mdns = {
        {"voron", "voron.local", "192.168.1.112", 7125, ""}};
    const std::vector<DiscoveredPrinter> sweep = {{"", "", "192.168.1.112", 7125, ""},
                                                  {"", "", "192.168.1.52", 80, ""},
                                                  {"", "", "192.168.1.52", 80, ""}};
    const auto merged = merge_discovered_printers(mdns, sweep);
    REQUIRE(merged.size() == 2);
    CHECK(merged[0].name == "voron");
    CHECK(merged[1].ip_address == "192.168.1.52");
    CHECK(merged[1].port == 80);

    CHECK(discovered_printer_label(merged[0]) == "voron (192.168.1.112:7125)");
    CHECK(discovered_printer_label(merged[1]) == "192.168.1.52:80");
}

TEST_CASE("Printer discovery: dropdown row 0 is the prompt, row i+1 is printer i", "[lan_sweep]") {
    const std::vector<DiscoveredPrinter> found = {{"", "", "192.168.1.52", 80, ""}};
    CHECK(discovered_printer_options(found, true) ==
          std::string(lv_tr("Choose a discovered printer")) + "\n192.168.1.52:80");
    CHECK(discovered_printer_options({}, true) == lv_tr("Scanning..."));
    CHECK(discovered_printer_options({}, false) == lv_tr("No printers found"));

    CHECK(discovered_printer_at(found, 0) == nullptr);
    REQUIRE(discovered_printer_at(found, 1) != nullptr);
    CHECK(discovered_printer_at(found, 1)->port == 80);
    CHECK(discovered_printer_at(found, 2) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "Printer discovery: mDNS and sweep updates merge as they arrive",
                 "[lan_sweep]") {
    auto mdns = std::make_unique<MockMdnsDiscovery>();
    auto sweep = std::make_unique<MockMdnsDiscovery>();
    MockMdnsDiscovery* mdns_raw = mdns.get();
    MockMdnsDiscovery* sweep_raw = sweep.get();
    MoonrakerDiscovery discovery(std::move(mdns), std::move(sweep));

    std::vector<std::vector<DiscoveredPrinter>> updates;
    discovery.start_discovery(
        [&](const std::vector<DiscoveredPrinter>& p) { updates.push_back(p); });

    sweep_raw->add_fake_printer("", "", "192.168.1.52", 80);
    sweep_raw->add_fake_printer("", "", "192.168.1.112", 7125);
    sweep_raw->simulate_discovery();
    REQUIRE_FALSE(updates.empty());
    CHECK(updates.back().size() == 2);

    mdns_raw->add_fake_printer("voron", "voron.local", "192.168.1.112", 7125);
    mdns_raw->simulate_discovery();
    REQUIRE(updates.back().size() == 2);
    CHECK(updates.back()[0].name == "voron");
    CHECK(discovery.is_discovering());

    discovery.stop_discovery();
    CHECK_FALSE(discovery.is_discovering());
}

// ============================================================================
// The sweep itself, with the network faked at the probe
// ============================================================================

namespace {

/// A probe that answers as Moonraker on the given host:port pairs and records the load.
struct FakeProbe {
    std::set<std::pair<std::string, uint16_t>> moonraker;
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::chrono::milliseconds delay{2};
    std::mutex mu;
    std::vector<std::pair<std::string, uint16_t>> asked;

    bool operator()(const std::string& host, uint16_t port) {
        started++;
        const int now = ++in_flight;
        int prev = max_in_flight.load();
        while (now > prev && !max_in_flight.compare_exchange_weak(prev, now)) {
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            asked.emplace_back(host, port);
        }
        std::this_thread::sleep_for(delay);
        --in_flight;
        const bool hit = moonraker.count({host, port}) > 0;
        finished++;
        return hit;
    }

    bool was_asked(const std::string& host, uint16_t port) {
        std::lock_guard<std::mutex> lock(mu);
        return std::find(asked.begin(), asked.end(), std::make_pair(host, port)) != asked.end();
    }
};

LanSweepDiscovery::LocalNetFn on_192_168_1() {
    return [] { return std::optional<LocalIpv4>(net(ip(192, 168, 1, 42), ip(255, 255, 255, 0))); };
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "LAN sweep: finds Moonraker on 7125 or 80, never more than the cap at once",
                 "[lan_sweep]") {
    FakeProbe probe;
    probe.moonraker = {{"192.168.1.52", 80}, {"192.168.1.112", 7125}, {"192.168.1.112", 80}};
    LanSweepLimits limits;
    limits.max_concurrent = 4;
    LanSweepDiscovery sweep(limits, on_192_168_1(),
                            [&probe](const std::string& h, uint16_t p) { return probe(h, p); });

    std::vector<DiscoveredPrinter> latest;
    int updates = 0;
    sweep.start_discovery([&](const std::vector<DiscoveredPrinter>& p) {
        latest = p;
        ++updates;
    });
    REQUIRE(sweep.is_discovering());
    REQUIRE(pump_until([&] { return !sweep.is_discovering(); }));

    CHECK(probe.max_in_flight.load() <= 4);
    CHECK(probe.max_in_flight.load() >= 2);
    REQUIRE(latest.size() == 2);
    CHECK(std::find(latest.begin(), latest.end(),
                    DiscoveredPrinter{"", "", "192.168.1.52", 80, ""}) != latest.end());
    CHECK(std::find(latest.begin(), latest.end(),
                    DiscoveredPrinter{"", "", "192.168.1.112", 7125, ""}) != latest.end());
    // One update per find, then one when the sweep ends.
    CHECK(updates == 3);

    // Each address is asked 7125 first and 80 only when 7125 is silent.
    CHECK(probe.was_asked("192.168.1.52", 7125));
    CHECK_FALSE(probe.was_asked("192.168.1.112", 80));
    CHECK(probe.was_asked("192.168.1.42", 7125));
    CHECK(probe.started.load() == 254 * 2 - 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "LAN sweep: stopping drops results in flight and asks no more",
                 "[lan_sweep]") {
    std::atomic<bool> release{false};
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    LanSweepLimits limits;
    limits.max_concurrent = 2;
    auto sweep = std::make_unique<LanSweepDiscovery>(
        limits, on_192_168_1(), [&](const std::string&, uint16_t) {
            started++;
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            finished++;
            return true;
        });

    int updates = 0;
    sweep->start_discovery([&](const std::vector<DiscoveredPrinter>&) { ++updates; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (started.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(started.load() == 2);

    // The screen closes with two probes running, both about to report Moonraker.
    sweep.reset();
    release = true;
    const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (finished.load() < 2 && std::chrono::steady_clock::now() < settle) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(finished.load() == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    drain();

    CHECK(updates == 0);
    CHECK(started.load() == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "LAN sweep: no private network means no sweep", "[lan_sweep]") {
    int probes = 0;
    LanSweepDiscovery sweep(
        LanSweepLimits{},
        [] { return std::optional<LocalIpv4>(net(ip(8, 8, 8, 8), ip(255, 255, 255, 0))); },
        [&](const std::string&, uint16_t) {
            ++probes;
            return false;
        });
    sweep.start_discovery([](const std::vector<DiscoveredPrinter>&) {});
    CHECK_FALSE(sweep.is_discovering());
    CHECK(probes == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "LAN sweep: a worker rests after every probe", "[lan_sweep]") {
    LanSweepLimits limits;
    limits.max_concurrent = 1;
    limits.pause_between_probes = std::chrono::milliseconds(40);
    std::vector<std::chrono::steady_clock::time_point> asked;
    std::mutex mu;
    // A /30 holds two host addresses, ours among them, each asked on 7125 and then 80.
    LanSweepDiscovery sweep(
        limits,
        [] { return std::optional<LocalIpv4>(net(ip(192, 168, 1, 42), ip(255, 255, 255, 252))); },
        [&](const std::string&, uint16_t) {
            std::lock_guard<std::mutex> lock(mu);
            asked.push_back(std::chrono::steady_clock::now());
            return false;
        });
    sweep.start_discovery([](const std::vector<DiscoveredPrinter>&) {});
    REQUIRE(pump_until([&] { return !sweep.is_discovering(); }));

    std::lock_guard<std::mutex> lock(mu);
    REQUIRE(asked.size() == 4);
    for (size_t i = 1; i < asked.size(); ++i) {
        CHECK(asked[i] - asked[i - 1] >= std::chrono::milliseconds(40));
    }
}
