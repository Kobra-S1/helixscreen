// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_nm_monitor.h"

#include "helix_thread.h"
#include "spdlog/spdlog.h"
#include "system/child_process.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>

namespace helix::wifi {

namespace {

// How long a stopped nmcli gets to exit on SIGTERM before SIGKILL.
constexpr auto kTermGrace = std::chrono::milliseconds(500);
// Between drain() and the waitpid that follows it. drain() itself blocks on
// the pipe, so this only bounds the loop while a stop runs the grace period.
constexpr auto kReapInterval = std::chrono::milliseconds(10);
// drain() wakes at least this often to let waitpid notice a child that died
// without closing its stdout, which a pipe alone would never report.
constexpr int kChildCheckMs = 60000;

// Linux's number on every architecture this builds for; the armhf toolchain's
// headers predate it.
#ifndef SYS_close_range
constexpr long SYS_close_range = 436;
#endif
// For kernels before 5.9, which have no close_range. Not the descriptor limit:
// that can be a million, and this runs before nmcli can start. The app never
// comes near this many open descriptors.
constexpr int kCloseWithoutSyscallLimit = 4096;

// Nothing above stderr is nmcli's. Sockets the app opened without close-on-exec
// would outlive a crashed app in this nmcli, keeping its listening port taken and
// its connections open until nmcli next tries to print. Runs between fork and
// exec, so it only makes system calls.
void close_descriptors_above_stderr() {
    if (::syscall(SYS_close_range, STDERR_FILENO + 1, ~0U, 0U) == 0) {
        return;
    }
    for (int fd = STDERR_FILENO + 1; fd < kCloseWithoutSyscallLimit; ++fd) {
        ::close(fd);
    }
}

} // namespace

NmcliMonitor::NmcliMonitor(std::function<void()> on_change)
    : NmcliMonitor(std::move(on_change), Restart{}) {}

NmcliMonitor::NmcliMonitor(std::function<void()> on_change, Restart restart)
    : on_change_(std::move(on_change)), restart_(restart) {}

NmcliMonitor::~NmcliMonitor() {
    stop();
}

bool NmcliMonitor::start() {
    if (thread_.joinable()) {
        return true;
    }
    // CLOEXEC: nothing this process forks has a use for these descriptors.
    if (::pipe2(wake_pipe_, O_CLOEXEC) < 0) {
        spdlog::error("[NmcliMonitor] pipe2() failed: {}", std::strerror(errno));
        return false;
    }
    // Wrap — EAGAIN throws ([L083]).
    try {
        thread_ = helix::make_thread(&NmcliMonitor::run, this);
    } catch (const std::system_error& e) {
        spdlog::error("[NmcliMonitor] Failed to spawn monitor thread: {}", e.what());
        ::close(wake_pipe_[0]);
        ::close(wake_pipe_[1]);
        wake_pipe_[0] = wake_pipe_[1] = -1;
        return false;
    }
    return true;
}

void NmcliMonitor::stop() {
    if (!thread_.joinable()) {
        return;
    }
    // Never read back: the pipe stays readable, so every later check in the
    // thread, the SIGTERM grace period's included, sees the stop.
    const char byte = 1;
    while (::write(wake_pipe_[1], &byte, 1) < 0 && errno == EINTR) {
    }
    thread_.join();
    ::close(wake_pipe_[0]);
    ::close(wake_pipe_[1]);
    wake_pipe_[0] = wake_pipe_[1] = -1;
}

void NmcliMonitor::run() {
    spdlog::debug("[NmcliMonitor] started");
    auto pause = restart_.first;
    while (!stop_requested()) {
        const auto began = std::chrono::steady_clock::now();
        run_nmcli();
        if (stop_requested()) {
            break;
        }
        if (std::chrono::steady_clock::now() - began >= restart_.longest) {
            pause = restart_.first;
        }
        spdlog::warn("[NmcliMonitor] nmcli monitor exited; starting it again in {}ms",
                     pause.count());
        if (wait_for_stop(pause)) {
            break;
        }
        pause = std::min(pause * 2, restart_.longest);
    }
    spdlog::debug("[NmcliMonitor] stopped");
}

void NmcliMonitor::run_nmcli() {
    int output[2];
    if (::pipe2(output, O_CLOEXEC) < 0) {
        spdlog::error("[NmcliMonitor] pipe2() failed: {}", std::strerror(errno));
        return;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        spdlog::error("[NmcliMonitor] fork() failed: {}", std::strerror(errno));
        ::close(output[0]);
        ::close(output[1]);
        return;
    }
    if (pid == 0) {
        // dup2 clears CLOEXEC on the copy, so only stdout survives the exec.
        ::dup2(output[1], STDOUT_FILENO);
        const int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
        }
        close_descriptors_above_stderr();
        ::execlp("nmcli", "nmcli", "monitor", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(output[1]);

    // Whatever changed while no nmcli was watching is announced by nobody.
    on_change_();

    const helix::ChildWaitResult wait =
        helix::terminate_and_reap(pid, std::chrono::steady_clock::time_point::max(), kTermGrace,
                                  kReapInterval, [this, fd = output[0]] { return drain(fd); });
    ::close(output[0]);
    if (wait.outcome == helix::ChildWaitOutcome::WaitFailed) {
        spdlog::error("[NmcliMonitor] waitpid failed: {}", std::strerror(wait.wait_errno));
    }
}

bool NmcliMonitor::drain(int output_fd) {
    pollfd fds[2] = {{output_fd, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
    const int ready = ::poll(fds, 2, kChildCheckMs);
    if (ready < 0) {
        return errno == EINTR;
    }
    if (fds[1].revents != 0) {
        return false;
    }
    if (fds[0].revents & POLLIN) {
        // What nmcli said does not matter, only that it said something: the
        // backend reads the whole state again either way.
        char buffer[512];
        const ssize_t got = ::read(output_fd, buffer, sizeof(buffer));
        if (got > 0) {
            on_change_();
            return true;
        }
        return got < 0 && errno == EINTR; // 0 is EOF: nmcli closed its stdout
    }
    // POLLHUP or POLLERR alone: an nmcli that can no longer announce anything.
    return (fds[0].revents & (POLLHUP | POLLERR)) == 0;
}

bool NmcliMonitor::stop_requested() const {
    return wait_for_stop(std::chrono::milliseconds(0));
}

bool NmcliMonitor::wait_for_stop(std::chrono::milliseconds timeout) const {
    pollfd wake = {wake_pipe_[0], POLLIN, 0};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int ready = ::poll(&wake, 1, static_cast<int>(std::max<int64_t>(left.count(), 0)));
        if (ready > 0) {
            return true;
        }
        if (ready == 0 || errno != EINTR) {
            return false;
        }
    }
}

} // namespace helix::wifi
