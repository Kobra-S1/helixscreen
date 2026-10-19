// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_static_subject_registry.cpp
 * @brief A subject source that is rebuilt must not leave its old deinit behind.
 *
 * Entries are keyed by name because the name identifies a subject source, not a
 * registration event. A source that is torn down and re-created re-registers
 * under the same name; if both entries survived, shutdown would run a callback
 * closed over the destroyed instance alongside the live one.
 *
 * Uses deinit_one() rather than deinit_all() so the case never touches the other
 * entries the process-wide registry is holding for the rest of the suite.
 */

#include "../helix_test_fixture.h"
#include "static_subject_registry.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(HelixTestFixture, "StaticSubjectRegistry keeps one entry per name",
                 "[core][subjects]") {
    auto& registry = StaticSubjectRegistry::instance();
    static constexpr const char* kName = "StaticSubjectRegistryTestProbe";

    std::vector<std::string> ran;
    registry.register_deinit(kName, [&ran]() { ran.emplace_back("first"); });
    registry.register_deinit(kName, [&ran]() { ran.emplace_back("second"); });

    REQUIRE(registry.deinit_one(kName));
    // The live registration wins, and nothing from the superseded one is left.
    CHECK(ran == std::vector<std::string>{"second"});
    CHECK_FALSE(registry.deinit_one(kName));
}

TEST_CASE_METHOD(HelixTestFixture,
                 "StaticSubjectRegistry printer switch honours each entry's lifetime",
                 "[core][subjects]") {
    // A switch only re-runs init on the rebuild path, so it must leave alone what
    // boot-time registration owns and still release per-widget state that observes
    // printer subjects. Exit runs everything.
    using Lifetime = StaticSubjectRegistry::Lifetime;
    using Scope = StaticSubjectRegistry::Scope;
    auto& registry = StaticSubjectRegistry::instance();
    static constexpr const char* kPrinter = "StaticSubjectRegistryLifetimePrinter";
    static constexpr const char* kProcess = "StaticSubjectRegistryLifetimeProcess";
    static constexpr const char* kRerun = "StaticSubjectRegistryLifetimeRerun";

    const size_t baseline = registry.count();
    std::vector<std::string> ran;
    registry.register_deinit(kPrinter, [&ran]() { ran.emplace_back("printer"); });
    registry.register_deinit(
        kProcess, [&ran]() { ran.emplace_back("process"); }, Lifetime::Process);
    registry.register_deinit(
        kRerun, [&ran]() { ran.emplace_back("rerun"); }, Lifetime::ProcessRunOnSwitch);
    REQUIRE(registry.count() == baseline + 3);

    REQUIRE(registry.deinit_one(kPrinter, Scope::PrinterSwitch));
    REQUIRE(registry.deinit_one(kProcess, Scope::PrinterSwitch));
    REQUIRE(registry.deinit_one(kRerun, Scope::PrinterSwitch));
    CHECK(ran == std::vector<std::string>{"printer", "rerun"});

    // The printer entry is gone and free to come back from the rebuild path; the
    // other two stay registered.
    CHECK(registry.count() == baseline + 2);
    CHECK_FALSE(registry.deinit_one(kPrinter, Scope::PrinterSwitch));
    registry.register_deinit(kPrinter, [&ran]() { ran.emplace_back("printer:rebuilt"); });

    // A second switch re-runs the run-on-switch cleanup; exit then runs every entry once.
    ran.clear();
    REQUIRE(registry.deinit_one(kRerun, Scope::PrinterSwitch));
    REQUIRE(registry.deinit_one(kPrinter, Scope::ProcessExit));
    REQUIRE(registry.deinit_one(kProcess, Scope::ProcessExit));
    REQUIRE(registry.deinit_one(kRerun, Scope::ProcessExit));
    CHECK(ran == std::vector<std::string>{"rerun", "printer:rebuilt", "process", "rerun"});
    CHECK(registry.count() == baseline);
}
