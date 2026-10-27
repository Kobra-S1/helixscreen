// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file crash_record.h
 * @brief What the crash file says about a signal, shared by the app and its watchdog
 *
 * The app's crash handler writes the full record from inside the dying process. A
 * signal it does not handle, or one that kills the process before the handler can
 * write, leaves nothing; the watchdog sees the death from outside and writes the
 * minimal record here, which crash_handler::read_crash_file() reads like any other.
 */

#pragma once

#include <csignal>
#include <string>

namespace helix::crash_record {

/// The fatal signals a crash is recorded for. SIGILL is what a jump into non-code
/// bytes raises on ARM (a zero word is `udf #0`), SIGTRAP what `__builtin_trap`
/// raises there, SIGSYS a syscall a seccomp filter refuses.
inline constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGSYS};

/// Whether @p sig is one of kCrashSignals: a crash, as opposed to a stop request.
bool is_crash_signal(int sig);

/// "SIGSEGV", "SIGILL", ... for kCrashSignals; "UNKNOWN" for any other. Async-signal-safe.
const char* signal_name(int sig);

/// Write a record for a process killed by @p sig into @p path, unless a record is
/// already there: one present is the dying process's own, fuller account. @p writer
/// names who wrote it ("watchdog"). Returns true when a record was written.
bool write_external(const std::string& path, int sig, const char* version, long timestamp,
                    const char* writer);

} // namespace helix::crash_record
