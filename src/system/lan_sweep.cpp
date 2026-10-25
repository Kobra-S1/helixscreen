// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lan_sweep.h"

#include "printer_discovery_list.h"
#include "utils/network_validation.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace helix {

// ============================================================================
// Address range
// ============================================================================

namespace {

bool is_private_ipv4(uint32_t a) {
    return (a >> 24) == 10 || (a >> 20) == ((172u << 4) | 1u) || (a >> 16) == ((192u << 8) | 168u);
}

std::string dotted(uint32_t a) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", static_cast<unsigned>(a >> 24),
                  static_cast<unsigned>((a >> 16) & 0xff), static_cast<unsigned>((a >> 8) & 0xff),
                  static_cast<unsigned>(a & 0xff));
    return buf;
}

} // namespace

std::vector<std::string> lan_sweep_hosts(const LocalIpv4& local) {
    if (!is_private_ipv4(local.address)) {
        return {};
    }
    // Never wider than a /24: a /16 is 65k addresses of traffic for one screen.
    const uint32_t mask = local.netmask | 0xffffff00u;
    const uint32_t network = local.address & mask;
    const uint32_t broadcast = network | ~mask;
    std::vector<std::string> hosts;
    for (uint32_t a = network + 1; a < broadcast; ++a) {
        if (a != local.address) {
            hosts.push_back(dotted(a));
        }
    }
    return hosts;
}

// ============================================================================
// The Moonraker check
// ============================================================================

namespace {

/// Whether @p key appears in @p body as an object key: quoted and followed by a colon.
bool has_json_key(const std::string& body, size_t from, const char* key) {
    const std::string quoted = std::string("\"") + key + "\"";
    for (size_t at = body.find(quoted, from); at != std::string::npos;
         at = body.find(quoted, at + 1)) {
        size_t i = at + quoted.size();
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) {
            ++i;
        }
        if (i < body.size() && body[i] == ':') {
            return true;
        }
    }
    return false;
}

} // namespace

bool is_moonraker_server_info_response(const std::string& http_response) {
    // "HTTP/1.1 200 OK"
    if (http_response.compare(0, 5, "HTTP/") != 0) {
        return false;
    }
    const size_t space = http_response.find(' ');
    if (space == std::string::npos || space + 1 >= http_response.size() ||
        http_response[space + 1] != '2') {
        return false;
    }
    const size_t body_start = http_response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        return false;
    }
    // A key scan keeps this small enough for the firmware image; the answer only needs
    // `"result": {` carrying Moonraker's keys.
    const size_t result = http_response.find("\"result\"", body_start);
    if (result == std::string::npos) {
        return false;
    }
    size_t i = result + 8;
    auto skip_space = [&]() {
        while (i < http_response.size() &&
               std::isspace(static_cast<unsigned char>(http_response[i]))) {
            ++i;
        }
    };
    skip_space();
    if (i >= http_response.size() || http_response[i] != ':') {
        return false;
    }
    ++i;
    skip_space();
    if (i >= http_response.size() || http_response[i] != '{') {
        return false;
    }
    return has_json_key(http_response, i, "moonraker_version") ||
           (has_json_key(http_response, i, "klippy_state") &&
            has_json_key(http_response, i, "components"));
}

bool probe_moonraker_http(const std::string& host, uint16_t port,
                          std::chrono::milliseconds connect_timeout,
                          std::chrono::milliseconds total_timeout) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + total_timeout;
    auto remaining_ms = [&deadline]() {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        return left > 0 ? static_cast<int>(left) : 0;
    };

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    struct Closer {
        int fd;
        ~Closer() {
            ::close(fd);
        }
    } closer{fd};

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno != EINPROGRESS) {
            return false;
        }
        pollfd pfd{fd, POLLOUT, 0};
        const int wait = std::min(static_cast<int>(connect_timeout.count()), remaining_ms());
        if (::poll(&pfd, 1, wait) != 1) {
            return false;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            return false;
        }
    }

    // HTTP/1.0 so the reply is not chunked and the server closes when done.
    const std::string request = "GET /server/info HTTP/1.0\r\nHost: " + host + ":" +
                                std::to_string(port) + "\r\nAccept: application/json\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{fd, POLLOUT, 0};
            if (::poll(&pfd, 1, remaining_ms()) != 1) {
                return false;
            }
            continue;
        }
        return false;
    }

    // /server/info is a few KB; anything far larger is not it.
    constexpr size_t kMaxResponse = 64 * 1024;
    std::string response;
    char buf[2048];
    while (response.size() < kMaxResponse) {
        pollfd pfd{fd, POLLIN, 0};
        const int left = remaining_ms();
        if (left == 0 || ::poll(&pfd, 1, left) != 1) {
            break;
        }
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        response.append(buf, static_cast<size_t>(n));
    }
    return is_moonraker_server_info_response(response);
}

// ============================================================================
// LanSweepDiscovery
// ============================================================================

struct LanSweepDiscovery::Sweep {
    std::vector<std::string> hosts;
    std::atomic<size_t> next{0};
    std::atomic<bool> cancelled{false};
    std::atomic<size_t> workers_left{0};
    ProbeFn probe;
};

LanSweepDiscovery::LanSweepDiscovery() : LanSweepDiscovery(lan_sweep_default_limits()) {}

LanSweepDiscovery::LanSweepDiscovery(LanSweepLimits limits)
    : LanSweepDiscovery(
          limits, active_local_ipv4, [limits](const std::string& host, uint16_t port) {
              return probe_moonraker_http(host, port, limits.connect_timeout, limits.total_timeout);
          }) {}

LanSweepDiscovery::LanSweepDiscovery(LanSweepLimits limits, LocalNetFn local_net, ProbeFn probe)
    : limits_(limits), local_net_(std::move(local_net)), probe_(std::move(probe)) {}

LanSweepDiscovery::~LanSweepDiscovery() {
    stop_discovery();
}

void LanSweepDiscovery::start_discovery(DiscoveryCallback on_update) {
    stop_discovery();
    on_update_ = std::move(on_update);
    found_.clear();

    const auto local = local_net_ ? local_net_() : std::nullopt;
    std::vector<std::string> hosts = local ? lan_sweep_hosts(*local) : std::vector<std::string>{};
    if (hosts.empty()) {
        spdlog::debug("[LanSweep] No private IPv4 network to sweep");
        return;
    }

    auto sweep = std::make_shared<Sweep>();
    sweep->hosts = std::move(hosts);
    sweep->probe = probe_;
    const size_t workers =
        std::max<size_t>(1, std::min(limits_.max_concurrent, sweep->hosts.size()));
    sweep->workers_left = workers;
    sweep_ = sweep;
    discovering_ = true;
    spdlog::info("[LanSweep] Sweeping {} addresses ({} at a time)", sweep->hosts.size(), workers);

    const auto tok = lifetime_.token();
    const std::vector<int> ports = moonraker_ports_to_try("", PortOrigin::FilledIn);
    const auto pause = limits_.pause_between_probes;
    // Each worker takes the next address until none are left. Results and the
    // finish reach `this` only through tok.defer, on the main thread.
    auto body = [this, sweep, tok, ports, pause]() {
        while (!sweep->cancelled.load()) {
            const size_t i = sweep->next.fetch_add(1);
            if (i >= sweep->hosts.size()) {
                break;
            }
            const std::string& host = sweep->hosts[i];
            for (int port : ports) {
                if (sweep->cancelled.load()) {
                    break;
                }
                const bool found = sweep->probe(host, static_cast<uint16_t>(port));
                if (pause.count() > 0) {
                    // Leaves the radio and the CPU to the UI between probes.
                    std::this_thread::sleep_for(pause);
                }
                if (found) {
                    tok.defer("LanSweep::found", [this, host, port]() {
                        on_found(host, static_cast<uint16_t>(port));
                    });
                    break;
                }
            }
        }
        if (sweep->workers_left.fetch_sub(1) == 1) {
            tok.defer("LanSweep::finished", [this]() { on_finished(); });
        }
    };
    LanSweepWorkers started = start_lan_sweep_workers(workers, limits_, body);
    workers_ = std::move(started.keepalive);
    if (started.started < workers) {
        spdlog::warn("[LanSweep] Started {} of {} workers", started.started, workers);
        // The workers that never started will not count themselves down.
        const size_t missing = workers - started.started;
        if (sweep->workers_left.fetch_sub(missing) == missing) {
            tok.defer("LanSweep::finished", [this]() { on_finished(); });
        }
    }
}

void LanSweepDiscovery::on_found(const std::string& host, uint16_t port) {
    DiscoveredPrinter printer;
    printer.ip_address = host;
    printer.port = port;
    if (std::find(found_.begin(), found_.end(), printer) != found_.end()) {
        return;
    }
    spdlog::info("[LanSweep] Moonraker at {}:{}", host, port);
    found_.push_back(printer);
    if (on_update_) {
        on_update_(found_);
    }
}

void LanSweepDiscovery::on_finished() {
    discovering_ = false;
    spdlog::info("[LanSweep] Sweep finished: {} found", found_.size());
    if (on_update_) {
        on_update_(found_);
    }
}

void LanSweepDiscovery::stop_discovery() {
    lifetime_.invalidate();
    if (sweep_) {
        sweep_->cancelled.store(true);
        sweep_.reset();
    }
    // A probe in flight finishes on its own within its total timeout. It holds only the
    // sweep's shared state, so releasing the workers never waits for it.
    workers_.reset();
    discovering_ = false;
    on_update_ = nullptr;
}

bool LanSweepDiscovery::is_discovering() const {
    return discovering_;
}

std::vector<DiscoveredPrinter> LanSweepDiscovery::get_discovered_printers() const {
    return found_;
}

// ============================================================================
// MoonrakerDiscovery
// ============================================================================

MoonrakerDiscovery::MoonrakerDiscovery(std::unique_ptr<IMdnsDiscovery> mdns,
                                       std::unique_ptr<IMdnsDiscovery> sweep)
    : mdns_(std::move(mdns)), sweep_(std::move(sweep)) {}

MoonrakerDiscovery::~MoonrakerDiscovery() {
    stop_discovery();
}

void MoonrakerDiscovery::start_discovery(DiscoveryCallback on_update) {
    on_update_ = std::move(on_update);
    mdns_found_.clear();
    sweep_found_.clear();
    // Both sources call back on the main thread and stop calling when stopped, which
    // happens before this object goes away.
    if (mdns_) {
        mdns_->start_discovery([this](const std::vector<DiscoveredPrinter>& found) {
            mdns_found_ = found;
            publish();
        });
    }
    if (sweep_) {
        sweep_->start_discovery([this](const std::vector<DiscoveredPrinter>& found) {
            sweep_found_ = found;
            publish();
        });
    }
}

void MoonrakerDiscovery::stop_discovery() {
    if (mdns_) {
        mdns_->stop_discovery();
    }
    if (sweep_) {
        sweep_->stop_discovery();
    }
    on_update_ = nullptr;
}

bool MoonrakerDiscovery::is_discovering() const {
    if (sweep_) {
        return sweep_->is_discovering();
    }
    return mdns_ && mdns_->is_discovering();
}

std::vector<DiscoveredPrinter> MoonrakerDiscovery::get_discovered_printers() const {
    return merge_discovered_printers(mdns_found_, sweep_found_);
}

void MoonrakerDiscovery::publish() {
    if (on_update_) {
        on_update_(get_discovered_printers());
    }
}

} // namespace helix
