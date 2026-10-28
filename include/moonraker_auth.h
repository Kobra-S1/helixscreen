// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

#include "hv/json.hpp"

/// The Moonraker API key, and the one printer it belongs to.
///
/// A Moonraker with `force_logins` refuses every request that carries no
/// credential, trusted address or not, and accepts its API key in an
/// `X-Api-Key` header. A wrong key is refused just as surely, even from a
/// trusted client, so the key goes only to the host and port it was configured
/// for: never to another printer, a webcam or any other server.
namespace helix::moonraker_auth {

inline constexpr const char* kApiKeyHeader = "X-Api-Key";

struct Credential {
    std::string host;
    int port = 0;
    std::string api_key; ///< Empty: this printer takes no key.
};

/// The credential a printer entry in settings carries: its `moonraker_host`,
/// `moonraker_port` and `moonraker_api_key`. A key that is `false`, absent or
/// blank means none.
[[nodiscard]] Credential credential_from_printer(const nlohmann::json& printer);

/// Records the active printer's credential, replacing the previous one.
void set_credential(Credential credential);

/// The key to send to @p host:@p port: the recorded key when that is the
/// printer it belongs to, else empty. Hosts compare case-insensitively.
[[nodiscard]] std::string key_for(std::string_view host, int port);

/// key_for() of the host and port in @p url ("ws://host:port/websocket",
/// "http://[::1]:7125/server/info"). Empty for a URL with no explicit port.
[[nodiscard]] std::string key_for_url(std::string_view url);

/// What a request to @p host:@p port does with its `X-Api-Key` header.
enum class HeaderAction {
    Leave,  ///< Not ours to touch: no key here, or another service's key.
    Set,    ///< Send the recorded key.
    Remove, ///< Carries the recorded key to a host it does not belong to.
};

/// @p current is the header's present value, empty when the request has none.
[[nodiscard]] HeaderAction header_action(std::string_view host, int port, std::string_view current);

} // namespace helix::moonraker_auth
