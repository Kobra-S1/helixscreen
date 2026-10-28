// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_auth.h"

#include "text_io.h"

#include <mutex>
#include <utility>

namespace helix::moonraker_auth {

namespace {

std::mutex& credential_mutex() {
    static std::mutex m;
    return m;
}

Credential& stored_credential() {
    static Credential c;
    return c;
}

} // namespace

Credential credential_from_printer(const nlohmann::json& printer) {
    Credential c;
    if (!printer.is_object()) {
        return c;
    }
    const auto host = printer.find("moonraker_host");
    if (host != printer.end() && host->is_string()) {
        c.host = host->get<std::string>();
    }
    const auto port = printer.find("moonraker_port");
    if (port != printer.end() && port->is_number_integer()) {
        c.port = port->get<int>();
    }
    const auto key = printer.find("moonraker_api_key");
    if (key != printer.end() && key->is_string()) {
        c.api_key = std::string(text_io::trim(key->get<std::string>()));
    }
    return c;
}

void set_credential(Credential credential) {
    std::lock_guard<std::mutex> lock(credential_mutex());
    stored_credential() = std::move(credential);
}

std::string key_for(std::string_view host, int port) {
    std::lock_guard<std::mutex> lock(credential_mutex());
    const Credential& c = stored_credential();
    if (c.api_key.empty() || c.port != port || !text_io::equals_ignore_case(c.host, host)) {
        return {};
    }
    return c.api_key;
}

std::string key_for_url(std::string_view url) {
    const size_t scheme = url.find("://");
    std::string_view rest = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));

    std::string_view host;
    std::string_view port;
    if (!rest.empty() && rest.front() == '[') {
        const size_t close = rest.find(']');
        if (close == std::string_view::npos || close + 1 >= rest.size() || rest[close + 1] != ':') {
            return {};
        }
        host = rest.substr(1, close - 1);
        port = rest.substr(close + 2);
    } else {
        const size_t colon = rest.rfind(':');
        if (colon == std::string_view::npos || rest.find(':') != colon) {
            return {};
        }
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
    }

    int port_number = 0;
    for (const char ch : port) {
        if (ch < '0' || ch > '9' || port_number > 65535) {
            return {};
        }
        port_number = port_number * 10 + (ch - '0');
    }
    if (port.empty() || port_number == 0 || port_number > 65535) {
        return {};
    }
    return key_for(host, port_number);
}

HeaderAction header_action(std::string_view host, int port, std::string_view current) {
    const std::string key = key_for(host, port);
    if (!key.empty()) {
        return current == key ? HeaderAction::Leave : HeaderAction::Set;
    }
    if (current.empty()) {
        return HeaderAction::Leave;
    }
    // Another service's key is that caller's own business; the recorded key
    // reaching a host it does not belong to (a redirect) is ours to take back.
    std::lock_guard<std::mutex> lock(credential_mutex());
    const std::string& recorded = stored_credential().api_key;
    return !recorded.empty() && current == recorded ? HeaderAction::Remove : HeaderAction::Leave;
}

} // namespace helix::moonraker_auth
