// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace {
bool g_registry_destroyed = false;

struct Disposition {
    bool run;
    bool keep;
};

/// What one teardown does to one entry. Exit runs and drops everything; a switch spares
/// entries whose init does not run again on the rebuild path.
Disposition disposition(StaticSubjectRegistry::Lifetime lifetime,
                        StaticSubjectRegistry::Scope scope) {
    using Lifetime = StaticSubjectRegistry::Lifetime;
    if (scope == StaticSubjectRegistry::Scope::ProcessExit) {
        return {true, false};
    }
    switch (lifetime) {
    case Lifetime::Process:
        return {false, true};
    case Lifetime::ProcessRunOnSwitch:
        return {true, true};
    case Lifetime::Printer:
        break;
    }
    return {true, false};
}
} // namespace

StaticSubjectRegistry& StaticSubjectRegistry::instance() {
    static StaticSubjectRegistry registry;
    return registry;
}

bool StaticSubjectRegistry::is_destroyed() {
    return g_registry_destroyed;
}

StaticSubjectRegistry::~StaticSubjectRegistry() {
    g_registry_destroyed = true;
}

void StaticSubjectRegistry::register_deinit(const char* name, std::function<void()> deinit_fn,
                                            Lifetime lifetime) {
    // Names identify a subject source, not a registration event. A source that is
    // torn down and rebuilt (PrintStatusWidget's DetailedFormatter, when the last
    // print-status widget leaves the dashboard and one is added back) re-registers
    // under the same name, and appending would leave a stale callback closed over
    // the previous instance to run alongside the live one. Drop the old entry and
    // re-append, so the entry keeps the "last registered, first deinitialized"
    // position deinit_all()'s reverse walk depends on.
    auto it = std::find_if(deinitializers_.begin(), deinitializers_.end(),
                           [name](const DeinitEntry& e) { return e.name == name; });
    if (it != deinitializers_.end()) {
        deinitializers_.erase(it);
        spdlog::trace("[StaticSubjectRegistry] Replacing existing entry: {}", name);
    }
    deinitializers_.push_back({name, std::move(deinit_fn), lifetime});
    spdlog::trace("[StaticSubjectRegistry] Registered: {} (total: {})", name,
                  deinitializers_.size());
}

bool StaticSubjectRegistry::deinit_one(const char* name, Scope scope) {
    auto it = std::find_if(deinitializers_.begin(), deinitializers_.end(),
                           [name](const DeinitEntry& e) { return e.name == name; });
    if (it == deinitializers_.end()) {
        return false;
    }

    const Disposition d = disposition(it->lifetime, scope);
    // Run a copy, never through `it`: the callback may register new entries.
    std::function<void()> fn = d.run ? it->deinit_fn : nullptr;
    if (!d.keep) {
        deinitializers_.erase(it);
    }
    if (fn) {
        fn();
    }
    return true;
}

void StaticSubjectRegistry::clear() {
    deinitializers_.clear();
    spdlog::trace("[StaticSubjectRegistry] Cleared all entries (no callbacks run)");
}

void StaticSubjectRegistry::deinit_all(Scope scope) {
    if (deinitializers_.empty()) {
        spdlog::debug("[StaticSubjectRegistry] No subjects registered, nothing to deinit");
        return;
    }

    spdlog::trace("[StaticSubjectRegistry] Deinitializing {} subject sources in reverse order...",
                  deinitializers_.size());

    // Iterate a detached copy: a deinit callback that re-creates a subject
    // source registers a fresh entry, and a push_back into the vector being
    // iterated would invalidate the loop's iterators. Re-registrations land in
    // the (now empty) member vector instead.
    std::vector<DeinitEntry> entries = std::move(deinitializers_);
    deinitializers_.clear();

    // Deinit in reverse order (last registered = first deinitialized)
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (!disposition(it->lifetime, scope).run) {
            spdlog::trace("[StaticSubjectRegistry] Keeping live: {}", it->name);
            continue;
        }
        spdlog::trace("[StaticSubjectRegistry] Deinitializing: {}", it->name);
        if (it->deinit_fn) {
            it->deinit_fn();
        }
    }

    // Survivors keep their original slots, ahead of anything registered during the walk;
    // a survivor re-registered by a callback takes the newer callback in its old slot.
    std::vector<DeinitEntry> kept;
    for (DeinitEntry& e : entries) {
        if (disposition(e.lifetime, scope).keep) {
            kept.push_back(std::move(e));
        }
    }
    for (DeinitEntry& fresh : deinitializers_) {
        auto slot = std::find_if(kept.begin(), kept.end(),
                                 [&fresh](const DeinitEntry& e) { return e.name == fresh.name; });
        if (slot != kept.end()) {
            *slot = std::move(fresh);
        } else {
            kept.push_back(std::move(fresh));
        }
    }
    deinitializers_ = std::move(kept);
    spdlog::trace("[StaticSubjectRegistry] Subjects deinitialized ({} entries remain)",
                  deinitializers_.size());
}
