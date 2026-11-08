// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/wifi_nm_monitor.h"
#include "netd_test_server.h" // helix_test::wait_until

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "../catch_amalgamated.hpp"

#ifndef __APPLE__

/**
 * helix::wifi::NmcliMonitor runs `nmcli monitor` so the NetworkManager backend
 * learns of a change when NetworkManager announces it, instead of starting
 * nmcli every few seconds to ask. These run the real fork/exec path against a
 * fake nmcli put first on PATH.
 */

namespace {

/// A throwaway dir holding a fake `nmcli`. FAKE_NMCLI_MODE picks what it does
/// once started: "announce" prints one line and stays up, "quiet" stays up
/// silently, "exit" ends at once. Each start appends its pid and argv.
struct FakeMonitorNmcli {
    std::string dir;

    explicit FakeMonitorNmcli(const char* mode) {
        char tmpl[] = "/tmp/helix-fake-nm-monitor-XXXXXX";
        dir = ::mkdtemp(tmpl);
        // exec keeps the shell's pid, so the recorded pid is the process the
        // monitor has to end.
        std::ofstream(dir + "/nmcli") << R"SH(#!/bin/sh
echo $$ >> "$FAKE_NMCLI_DIR/pids"
printf '%s\n' "$*" >> "$FAKE_NMCLI_DIR/argv"
case "$FAKE_NMCLI_MODE" in
  announce) echo "wlan0: connected"; exec sleep 1000 ;;
  quiet) exec sleep 1000 ;;
  *) exit 1 ;;
esac
)SH";
        ::chmod((dir + "/nmcli").c_str(), 0755);
        ::setenv("FAKE_NMCLI_DIR", dir.c_str(), 1);
        ::setenv("FAKE_NMCLI_MODE", mode, 1);
        const char* path = ::getenv("PATH");
        old_path_ = path ? path : "";
        ::setenv("PATH", (dir + ":" + old_path_).c_str(), 1);
    }
    ~FakeMonitorNmcli() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("FAKE_NMCLI_DIR");
        ::unsetenv("FAKE_NMCLI_MODE");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::vector<std::string> lines(const char* name) const {
        std::ifstream f(dir + "/" + name);
        std::vector<std::string> out;
        for (std::string line; std::getline(f, line);) {
            out.push_back(line);
        }
        return out;
    }
    size_t starts() const {
        return lines("pids").size();
    }

  private:
    std::string old_path_;
};

bool process_gone(const std::string& pid) {
    return ::kill(std::stoi(pid), 0) != 0 && errno == ESRCH;
}

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

} // namespace

TEST_CASE("NmcliMonitor: an announcement reaches on_change", "[network][nm][nm-monitor][slow]") {
    FakeMonitorNmcli fake("announce");
    std::atomic<int> changes{0};
    helix::wifi::NmcliMonitor monitor([&changes] { ++changes; });

    REQUIRE(monitor.start());
    // One call when nmcli starts (whatever changed while nothing watched), one
    // for the line it announced.
    REQUIRE(helix_test::wait_until([&changes] { return changes.load() >= 2; }, 2000));
    monitor.stop();

    CHECK(fake.lines("argv") == std::vector<std::string>{"monitor"});
    CHECK(fake.starts() == 1);
}

TEST_CASE("NmcliMonitor: stop ends nmcli and returns", "[network][nm][nm-monitor][slow]") {
    FakeMonitorNmcli fake("quiet");
    helix::wifi::NmcliMonitor monitor([] {});

    REQUIRE(monitor.start());
    REQUIRE(helix_test::wait_until([&fake] { return fake.starts() == 1; }, 2000));

    const auto began = Clock::now();
    monitor.stop();
    CHECK(Clock::now() - began < milliseconds(1000));
    CHECK(process_gone(fake.lines("pids").front()));
}

TEST_CASE("NmcliMonitor: an nmcli that exits is started again", "[network][nm][nm-monitor][slow]") {
    FakeMonitorNmcli fake("exit");
    std::atomic<int> changes{0};
    helix::wifi::NmcliMonitor monitor([&changes] { ++changes; },
                                      {milliseconds(10), milliseconds(40)});

    REQUIRE(monitor.start());
    // The default first pause (a second, doubling) would take three seconds
    // to get here: the configured one is what this waits on.
    REQUIRE(helix_test::wait_until([&fake] { return fake.starts() >= 3; }, 1500));
    monitor.stop();

    // Every start asks for a refresh: a change while no nmcli watched is
    // otherwise never seen.
    CHECK(changes.load() >= 3);
}

TEST_CASE("NmcliMonitor: stop during the pause before a restart returns at once",
          "[network][nm][nm-monitor][slow]") {
    FakeMonitorNmcli fake("exit");
    helix::wifi::NmcliMonitor monitor([] {}, {milliseconds(10000), milliseconds(10000)});

    REQUIRE(monitor.start());
    REQUIRE(helix_test::wait_until([&fake] { return fake.starts() == 1; }, 2000));

    const auto began = Clock::now();
    monitor.stop();
    CHECK(Clock::now() - began < milliseconds(1000));
    CHECK(fake.starts() == 1);
}

TEST_CASE("NmcliMonitor: start and stop are idempotent", "[network][nm][nm-monitor][slow]") {
    FakeMonitorNmcli fake("quiet");
    helix::wifi::NmcliMonitor monitor([] {});

    monitor.stop(); // before any start

    REQUIRE(monitor.start());
    REQUIRE(monitor.start());
    REQUIRE(helix_test::wait_until([&fake] { return fake.starts() >= 1; }, 2000));
    monitor.stop();
    monitor.stop();
    CHECK(fake.starts() == 1);

    // A stopped monitor starts again with a fresh nmcli.
    REQUIRE(monitor.start());
    REQUIRE(helix_test::wait_until([&fake] { return fake.starts() == 2; }, 2000));
    monitor.stop();
    for (const std::string& pid : fake.lines("pids")) {
        CHECK(process_gone(pid));
    }
}

#endif // __APPLE__
