// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lan_sweep.h"

#include "http_executor.h"
#include "printer_discovery_list.h"
#include "utils/network_validation.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hv/json.hpp"

namespace helix {

// ============================================================================
// Address range
// ============================================================================

namespace {

bool is_private_ipv4(uint32_t a) {
    return (a >> 24) == 10 || (a >> 20) == ((172u << 4) | 1u) || (a >> 16) == ((192u << 8) | 168u);
}

std::string dotted(uint32_t a) {
    return std::to_string(a >> 24) + "." + std::to_string((a >> 16) & 0xff) + "." +
           std::to_string((a >> 8) & 0xff) + "." + std::to_string(a & 0xff);
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

std::optional<LocalIpv4> active_local_ipv4() {
    // A connected UDP socket sends nothing, but makes the kernel pick the source address
    // its route would use.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return std::nullopt;
    }
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    probe.sin_addr.s_addr = htonl(0x08080808u);
    sockaddr_in self{};
    socklen_t len = sizeof(self);
    const bool routed = ::connect(fd, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0 &&
                        ::getsockname(fd, reinterpret_cast<sockaddr*>(&self), &len) == 0;
    ::close(fd);
    if (!routed || self.sin_addr.s_addr == 0) {
        return std::nullopt;
    }

    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return std::nullopt;
    }
    std::optional<LocalIpv4> found;
    for (ifaddrs* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || !it->ifa_netmask || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if (addr->sin_addr.s_addr == self.sin_addr.s_addr) {
            const auto* mask = reinterpret_cast<const sockaddr_in*>(it->ifa_netmask);
            found = LocalIpv4{ntohl(addr->sin_addr.s_addr), ntohl(mask->sin_addr.s_addr)};
            break;
        }
    }
    ::freeifaddrs(list);
    return found;
}

// ============================================================================
// The Moonraker check
// ============================================================================

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
    const auto doc = nlohmann::json::parse(http_response.begin() + body_start + 4,
                                           http_response.end(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return false;
    }
    const auto result = doc.find("result");
    if (result == doc.end() || !result->is_object()) {
        return false;
    }
    return result->contains("moonraker_version") ||
           (result->contains("klippy_state") && result->contains("components"));
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

    pool_ = std::make_unique<http::HttpExecutor>("lan-sweep", workers);
    pool_->start();
    const auto tok = lifetime_.token();
    const std::vector<int> ports = moonraker_ports_to_try("", false);
    for (size_t w = 0; w < workers; ++w) {
        // Each worker takes the next address until none are left. Results and the
        // finish reach `this` only through tok.defer, on the main thread.
        pool_->submit([this, sweep, tok, ports]() {
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
                    if (sweep->probe(host, static_cast<uint16_t>(port))) {
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
        });
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
    if (pool_) {
        // A probe in flight finishes on its own within its total timeout; it holds only
        // the sweep's shared state, so the pool lets it go rather than hold the UI.
        pool_->stop(std::chrono::milliseconds(20));
        pool_.reset();
    }
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

std::unique_ptr<IMdnsDiscovery> make_moonraker_discovery() {
    return std::make_unique<MoonrakerDiscovery>(std::make_unique<MdnsDiscovery>(),
                                                std::make_unique<LanSweepDiscovery>());
}

} // namespace helix
