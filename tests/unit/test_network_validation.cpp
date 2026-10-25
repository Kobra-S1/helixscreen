// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "utils/network_validation.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// IP Address Validation Tests (via shared utils)
// ============================================================================

TEST_CASE("Network validation: Valid IPv4 addresses", "[validation][network][ip]") {
    REQUIRE(is_valid_ip_or_hostname("192.168.1.1") == true);
    REQUIRE(is_valid_ip_or_hostname("10.0.0.1") == true);
    REQUIRE(is_valid_ip_or_hostname("172.16.0.1") == true);
    REQUIRE(is_valid_ip_or_hostname("127.0.0.1") == true);
    REQUIRE(is_valid_ip_or_hostname("255.255.255.255") == true);
    REQUIRE(is_valid_ip_or_hostname("0.0.0.0") == true);
}

TEST_CASE("Network validation: Invalid IPv4 addresses", "[validation][network][ip]") {
    REQUIRE(is_valid_ip_or_hostname("999.1.1.1") == false);
    REQUIRE(is_valid_ip_or_hostname("192.168.1.256") == false);
    REQUIRE(is_valid_ip_or_hostname("192.168.1") == false);
    REQUIRE(is_valid_ip_or_hostname("192.168.1.1.1") == false);
    REQUIRE(is_valid_ip_or_hostname("192.168..1") == false);
    REQUIRE(is_valid_ip_or_hostname("192.168.1.") == false);
    REQUIRE(is_valid_ip_or_hostname(".192.168.1.1") == false);
}

TEST_CASE("Network validation: Valid hostnames", "[validation][network][hostname]") {
    REQUIRE(is_valid_ip_or_hostname("printer") == true);
    REQUIRE(is_valid_ip_or_hostname("printer.local") == true);
    REQUIRE(is_valid_ip_or_hostname("my-printer") == true);
    REQUIRE(is_valid_ip_or_hostname("my_printer") == true);
    REQUIRE(is_valid_ip_or_hostname("PRINTER123") == true);
    REQUIRE(is_valid_ip_or_hostname("voron-2.4") == true);
    REQUIRE(is_valid_ip_or_hostname("k1.local") == true);
}

TEST_CASE("Network validation: Invalid hostnames", "[validation][network][hostname]") {
    REQUIRE(is_valid_ip_or_hostname("") == false);
    REQUIRE(is_valid_ip_or_hostname("-printer") == false);
    REQUIRE(is_valid_ip_or_hostname("!invalid") == false);
    REQUIRE(is_valid_ip_or_hostname("print@r") == false);
    REQUIRE(is_valid_ip_or_hostname("print er") == false);
}

TEST_CASE("Network validation: Valid ports", "[validation][network][port]") {
    REQUIRE(is_valid_port("1") == true);
    REQUIRE(is_valid_port("80") == true);
    REQUIRE(is_valid_port("443") == true);
    REQUIRE(is_valid_port("7125") == true);
    REQUIRE(is_valid_port("8080") == true);
    REQUIRE(is_valid_port("65535") == true);
}

TEST_CASE("Network validation: Invalid ports", "[validation][network][port]") {
    REQUIRE(is_valid_port("") == false);
    REQUIRE(is_valid_port("0") == false);
    REQUIRE(is_valid_port("65536") == false);
    REQUIRE(is_valid_port("99999") == false);
    REQUIRE(is_valid_port("-1") == false);
    REQUIRE(is_valid_port("abc") == false);
    REQUIRE(is_valid_port("12.34") == false);
    REQUIRE(is_valid_port("80a") == false);
    REQUIRE(is_valid_port("   ") == false);
}

TEST_CASE("Network validation: Whitespace trimming", "[validation][network][trim]") {
    // Port: leading/trailing whitespace should be trimmed
    REQUIRE(is_valid_port(" 80") == true);
    REQUIRE(is_valid_port("80 ") == true);
    REQUIRE(is_valid_port("  7125  ") == true);
    REQUIRE(is_valid_port("\t443\t") == true);
    REQUIRE(is_valid_port(" 0") == false);      // Still invalid after trim
    REQUIRE(is_valid_port(" 65536 ") == false); // Still invalid after trim

    // IP/hostname: leading/trailing whitespace should be trimmed
    REQUIRE(is_valid_ip_or_hostname(" 192.168.1.1") == true);
    REQUIRE(is_valid_ip_or_hostname("192.168.1.1 ") == true);
    REQUIRE(is_valid_ip_or_hostname("  printer.local  ") == true);
    REQUIRE(is_valid_ip_or_hostname("\t127.0.0.1\t") == true);
    REQUIRE(is_valid_ip_or_hostname("  ") == false); // All whitespace
}

TEST_CASE("Network validation: Edge cases", "[validation][network][edge]") {
    REQUIRE(is_valid_ip_or_hostname("localhost") == true);
    REQUIRE(is_valid_ip_or_hostname("raspberrypi") == true);
    REQUIRE(is_valid_ip_or_hostname("mainsailos") == true);
    REQUIRE(is_valid_port("1") == true);
    REQUIRE(is_valid_port("65535") == true);
}

// ============================================================================
// Connection test failure classification
// ============================================================================

#include <cerrno>
#include <string>
#include <vector>

using helix::classify_connect_failure;
using helix::connect_failure_hint;
using helix::ConnectFailureKind;
using helix::moonraker_ports_to_try;

TEST_CASE("Connect failure: socket errors classify by what the user should check",
          "[validation][network][connect_failure]") {
    CHECK(classify_connect_failure(ETIMEDOUT, 0) == ConnectFailureKind::Timeout);
    CHECK(classify_connect_failure(EHOSTUNREACH, 0) == ConnectFailureKind::Timeout);
    CHECK(classify_connect_failure(ENETUNREACH, 0) == ConnectFailureKind::Timeout);
    CHECK(classify_connect_failure(ECONNREFUSED, 0) == ConnectFailureKind::Refused);
    CHECK(classify_connect_failure(0, 0) == ConnectFailureKind::Unknown);
    CHECK(classify_connect_failure(ECONNRESET, 0) == ConnectFailureKind::Unknown);
}

TEST_CASE("Connect failure: ENOTCONN defers to the socket's pending error",
          "[validation][network][connect_failure]") {
    CHECK(classify_connect_failure(ENOTCONN, ECONNREFUSED) == ConnectFailureKind::Refused);
    CHECK(classify_connect_failure(ENOTCONN, EHOSTUNREACH) == ConnectFailureKind::Timeout);
    CHECK(classify_connect_failure(ENOTCONN, 0) == ConnectFailureKind::Unknown);
    // A pending error is only consulted for ENOTCONN: a decisive io error stands.
    CHECK(classify_connect_failure(ETIMEDOUT, ECONNREFUSED) == ConnectFailureKind::Timeout);
}

TEST_CASE("Connect failure: each kind gets its own hint, mobile data on Android only",
          "[validation][network][connect_failure]") {
    const std::string timeout_desktop = connect_failure_hint(ConnectFailureKind::Timeout, false);
    const std::string timeout_android = connect_failure_hint(ConnectFailureKind::Timeout, true);
    const std::string refused = connect_failure_hint(ConnectFailureKind::Refused, false);
    const std::string unknown = connect_failure_hint(ConnectFailureKind::Unknown, false);

    CHECK(timeout_android.find("mobile data") != std::string::npos);
    CHECK(timeout_desktop.find("mobile data") == std::string::npos);
    CHECK(timeout_desktop.find("IP address") != std::string::npos);
    CHECK(refused.find("port") != std::string::npos);
    CHECK(refused == connect_failure_hint(ConnectFailureKind::Refused, true));
    CHECK(unknown == "Connection failed. Check IP/port and try again.");
    CHECK(unknown == connect_failure_hint(ConnectFailureKind::Unknown, true));

    CHECK(timeout_desktop != refused);
    CHECK(timeout_desktop != unknown);
    CHECK(refused != unknown);
}

// ============================================================================
// Which ports a connection test tries
// ============================================================================

TEST_CASE("Moonraker ports: the untouched default falls back to port 80",
          "[validation][network][port_fallback]") {
    using V = std::vector<int>;
    CHECK(moonraker_ports_to_try("7125", false) == V{7125, 80});
    CHECK(moonraker_ports_to_try(" 7125 ", false) == V{7125, 80});
    // An empty field is auto-filled with the default, typed or not.
    CHECK(moonraker_ports_to_try("", false) == V{7125, 80});
    CHECK(moonraker_ports_to_try("", true) == V{7125, 80});
}

TEST_CASE("Moonraker ports: a typed port is tried alone", "[validation][network][port_fallback]") {
    using V = std::vector<int>;
    CHECK(moonraker_ports_to_try("7125", true) == V{7125});
    CHECK(moonraker_ports_to_try("80", true) == V{80});
    CHECK(moonraker_ports_to_try("7126", true) == V{7126});
}

TEST_CASE("Moonraker ports: a filled-in port other than the default is tried alone",
          "[validation][network][port_fallback]") {
    using V = std::vector<int>;
    // A saved or discovered port is a known address, not a guess.
    CHECK(moonraker_ports_to_try("80", false) == V{80});
    CHECK(moonraker_ports_to_try("7126", false) == V{7126});
}

TEST_CASE("Moonraker ports: an invalid port tries nothing",
          "[validation][network][port_fallback]") {
    CHECK(moonraker_ports_to_try("0", false).empty());
    CHECK(moonraker_ports_to_try("70000", true).empty());
    CHECK(moonraker_ports_to_try("abc", false).empty());
}
