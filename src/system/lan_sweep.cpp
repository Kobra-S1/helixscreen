// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lan_sweep.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_events.h"
#include "platform_info.h"
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
#include <netdb.h>
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
    // Our own address stays in: on a printer that runs HelixScreen, its Moonraker is a
    // printer to list like any other.
    for (uint32_t a = network + 1; a < broadcast; ++a) {
        hosts.push_back(dotted(a));
    }
    return hosts;
}

// ============================================================================
// The Moonraker check
// ============================================================================

namespace {

size_t skip_json_space(const std::string& s, size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    return i;
}

/// Where the value of @p key starts in the JSON object whose '{' is at @p open, or npos.
/// Only that object's own keys count: nothing nested deeper, and no string values.
size_t json_member(const std::string& s, size_t open, const char* key) {
    const size_t key_len = std::strlen(key);
    bool expect_key = true;
    int depth = 0;
    for (size_t i = open + 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {
            const size_t start = i + 1;
            size_t end = start;
            while (end < s.size() && s[end] != '"') {
                end += (s[end] == '\\') ? 2 : 1;
            }
            if (end >= s.size()) {
                return std::string::npos;
            }
            i = end;
            // expect_key is only ever set at this object's own level.
            if (expect_key) {
                const size_t colon = skip_json_space(s, end + 1);
                if (colon < s.size() && s[colon] == ':') {
                    if (end - start == key_len && s.compare(start, key_len, key) == 0) {
                        return skip_json_space(s, colon + 1);
                    }
                    expect_key = false;
                    i = colon;
                }
            }
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (depth == 0) {
                return std::string::npos;
            }
            --depth;
        } else if (c == ',' && depth == 0) {
            expect_key = true;
        }
    }
    return std::string::npos;
}

/// The object value of @p key in the object at @p open: the index of its '{', or npos.
size_t json_object_member(const std::string& s, size_t open, const char* key) {
    const size_t v = json_member(s, open, key);
    return (v < s.size() && s[v] == '{') ? v : std::string::npos;
}

} // namespace

MoonrakerProbe read_server_info_response(const std::string& http_response) {
    const std::string& r = http_response;
    // "HTTP/1.1 200 OK"
    if (r.compare(0, 5, "HTTP/") != 0) {
        return {};
    }
    const size_t space = r.find(' ');
    if (space == std::string::npos || space + 3 >= r.size()) {
        return {};
    }
    const bool ok = r[space + 1] == '2';
    // An auth-protected Moonraker answers 401 or 403 with its own error object; listing it
    // lets the pick fill the address, and the connection deals with the login.
    const bool refused = r.compare(space + 1, 3, "401") == 0 || r.compare(space + 1, 3, "403") == 0;
    if (!ok && !refused) {
        return {};
    }
    const size_t body_start = r.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        return {};
    }
    const size_t top = skip_json_space(r, body_start + 4);
    if (top >= r.size() || r[top] != '{') {
        return {};
    }
    // A scan of the top-level and member keys, which is all this needs, rather than a
    // full parse, keeps the check small enough for the firmware image.
    if (refused) {
        const size_t error = json_object_member(r, top, "error");
        return error != std::string::npos && json_member(r, error, "code") != std::string::npos &&
               json_member(r, error, "message") != std::string::npos;
    }
    const size_t result = json_object_member(r, top, "result");
    if (result == std::string::npos) {
        return {};
    }
    const size_t klippy = json_member(r, result, "klippy_state");
    MoonrakerProbe probe(
        json_member(r, result, "moonraker_version") != std::string::npos ||
        (klippy != std::string::npos && json_member(r, result, "components") != std::string::npos));
    if (probe.moonraker && klippy < r.size() && r[klippy] == '"') {
        const size_t end = r.find('"', klippy + 1);
        if (end != std::string::npos) {
            probe.klippy_state = r.substr(klippy + 1, end - klippy - 1);
        }
    }
    return probe;
}

bool is_moonraker_server_info_response(const std::string& http_response) {
    return read_server_info_response(http_response).moonraker;
}

MoonrakerProbe probe_moonraker_http(const std::string& host, uint16_t port,
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
    MoonrakerProbe failed;
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        // A name: Test Connection takes one; the sweep only ever passes addresses.
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) {
            failed.failure = ConnectFailureKind::Timeout;
            return failed;
        }
        addr.sin_addr = reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr;
        ::freeaddrinfo(found);
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return failed;
    }
    struct Closer {
        int fd;
        ~Closer() {
            ::close(fd);
        }
    } closer{fd};

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return failed;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno != EINPROGRESS) {
            failed.failure = classify_connect_failure(errno, 0);
            return failed;
        }
        pollfd pfd{fd, POLLOUT, 0};
        const int wait = std::min(static_cast<int>(connect_timeout.count()), remaining_ms());
        if (::poll(&pfd, 1, wait) != 1) {
            failed.failure = ConnectFailureKind::Timeout;
            return failed;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            failed.failure = classify_connect_failure(err, 0);
            return failed;
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
                return failed;
            }
            continue;
        }
        return failed;
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
    return read_server_info_response(response);
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

LanSweepDiscovery::LanSweepDiscovery(LanSweepLimits limits, LocalNetFn local_net, ProbeFn probe,
                                     StartWorkersFn start_workers)
    : limits_(limits), local_net_(std::move(local_net)), probe_(std::move(probe)),
      start_workers_(std::move(start_workers)) {}

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
                const bool found = sweep->probe(host, static_cast<uint16_t>(port)).moonraker;
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
    LanSweepWorkers started = start_workers_(workers, limits_, body);
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

// ============================================================================
// MoonrakerConnectionTest
// ============================================================================

namespace {

struct ConnectionTestSeams {
    MoonrakerProbeFn probe;
    StartWorkersFn start_workers;
};

ConnectionTestSeams& connection_test_seams() {
    static ConnectionTestSeams seams;
    return seams;
}

/// Longer than a sweep's: this is one address the user named, maybe behind a slow link.
LanSweepLimits connection_test_limits() {
    LanSweepLimits limits = lan_sweep_default_limits();
    limits.max_concurrent = 1;
    limits.connect_timeout = std::chrono::milliseconds(3000);
    limits.total_timeout = std::chrono::milliseconds(5000);
    return limits;
}

} // namespace

void set_connection_test_seams_for_test(MoonrakerProbeFn probe, StartWorkersFn start_workers) {
    connection_test_seams() = {std::move(probe), std::move(start_workers)};
}

MoonrakerConnectionTest::MoonrakerConnectionTest() : limits_(connection_test_limits()) {
    const LanSweepLimits limits = limits_;
    probe_ = [limits](const std::string& host, uint16_t port) {
        return probe_moonraker_http(host, port, limits.connect_timeout, limits.total_timeout);
    };
    start_workers_ = start_lan_sweep_workers;
}

MoonrakerConnectionTest::~MoonrakerConnectionTest() {
    cancel();
}

void MoonrakerConnectionTest::start(const std::string& host, std::vector<int> ports,
                                    DoneFn on_done) {
    cancel();
    on_done_ = std::move(on_done);
    running_ = true;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    cancelled_ = cancelled;
    const auto tok = lifetime_.token();
    // Stand-ins are read per start, so a screen built before a test installs them uses them.
    const ConnectionTestSeams& seams = connection_test_seams();
    auto probe = seams.probe ? seams.probe : probe_;
    const StartWorkersFn& start_workers =
        seams.start_workers ? seams.start_workers : start_workers_;
    // Runs on the worker; reaches `this` only through tok.defer, on the main thread.
    auto body = [this, tok, probe, cancelled, host, ports]() {
        Result result;
        for (int port : ports) {
            if (cancelled->load()) {
                return;
            }
            result.tried.push_back(port);
            const MoonrakerProbe answer = probe(host, static_cast<uint16_t>(port));
            if (answer.moonraker) {
                result.reached = true;
                result.port = port;
                result.klippy_state = answer.klippy_state;
                break;
            }
            result.failure = answer.failure;
        }
        tok.defer("MoonrakerConnectionTest::done", [this, result]() { finish(result); });
    };
    LanSweepWorkers worker = start_workers(1, limits_, body);
    worker_ = std::move(worker.keepalive);
    if (worker.started == 0) {
        Result none;
        none.tried = ports;
        tok.defer("MoonrakerConnectionTest::no_worker", [this, none]() { finish(none); });
    }
}

void MoonrakerConnectionTest::cancel() {
    lifetime_.invalidate();
    if (cancelled_) {
        cancelled_->store(true);
        cancelled_.reset();
    }
    worker_.reset();
    running_ = false;
    on_done_ = nullptr;
}

void MoonrakerConnectionTest::finish(const Result& result) {
    running_ = false;
    cancelled_.reset();
    worker_.reset();
    // The callback may start the next test, which replaces on_done_.
    DoneFn done = std::move(on_done_);
    on_done_ = nullptr;
    if (done) {
        done(result);
    }
}

std::string connect_failure_status(ConnectFailureKind kind, const std::vector<int>& ports_tried) {
    std::string text = lv_tr(connect_failure_hint(kind, is_android_platform()));
    if (ports_tried.size() > 1) {
        // "7125, 8080" and "80": every port but the last, then the last.
        std::string leading;
        for (size_t i = 0; i + 1 < ports_tried.size(); ++i) {
            leading += (i ? ", " : "") + std::to_string(ports_tried[i]);
        }
        text += " ";
        text += fill_placeholders(lv_tr("Tried ports {} and {}."),
                                  {leading, std::to_string(ports_tried.back())});
    }
    return text;
}

std::string connection_test_status(const std::string& host,
                                   const MoonrakerConnectionTest::Result& result) {
    std::string text = fill_placeholders(lv_tr("Moonraker answered at {}."),
                                         {host + ":" + std::to_string(result.port)});
    if (!result.klippy_state.empty()) {
        text += " ";
        text += fill_placeholders(lv_tr("Klipper: {}"), {result.klippy_state});
    }
    return text;
}

} // namespace helix
