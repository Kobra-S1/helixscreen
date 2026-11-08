// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <thread>

/**
 * @file wifi_nm_monitor.h
 * @brief Tells the NetworkManager backend when NetworkManager announces a
 *        change, so it reads the state then instead of polling for it.
 */
namespace helix::wifi {

/// Runs `nmcli monitor` on its own thread while started. NetworkManager prints
/// a line there for every change of its state, a device's state (the WiFi
/// radio switching included, as wlan0 going unavailable), the active
/// connection or connectivity; each one calls on_change. So does every start
/// of nmcli, since whatever changed while none was watching is otherwise never
/// seen. An nmcli that exits is started again after a pause that doubles up to
/// Restart::longest, and resets once one has run that long.
///
/// on_change runs on the monitor's thread and must not block. start() and
/// stop() are confined to one thread (the backend's start/stop path).
class NmcliMonitor {
  public:
    struct Restart {
        std::chrono::milliseconds first{1000};
        std::chrono::milliseconds longest{60000};
    };

    explicit NmcliMonitor(std::function<void()> on_change);
    NmcliMonitor(std::function<void()> on_change, Restart restart);
    ~NmcliMonitor();

    NmcliMonitor(const NmcliMonitor&) = delete;
    NmcliMonitor& operator=(const NmcliMonitor&) = delete;

    /// Starts the monitor thread; true while it runs. False when the thread or
    /// its wake pipe could not be created.
    bool start();

    /// Ends nmcli, joins the thread, and returns once both are gone. A no-op
    /// when not started.
    void stop();

  private:
    void run();
    void run_nmcli();
    bool drain(int output_fd);
    bool stop_requested() const;
    bool wait_for_stop(std::chrono::milliseconds timeout) const;

    std::function<void()> on_change_;
    Restart restart_;
    std::thread thread_;
    /// Written once by stop(); the thread polls the read end with nmcli's
    /// output, so a stop never waits for nmcli to print or for a pause to end.
    int wake_pipe_[2] = {-1, -1};
};

} // namespace helix::wifi
