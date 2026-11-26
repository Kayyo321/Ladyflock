/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibIPC/Forward.h>
#include <LibPrivacy/Export.h>

namespace Privacy {

// Proxy types a mandatory route may use. Each one resolves destination names at the proxy, so a routed request never
// needs a local DNS lookup. SOCKS4 and SOCKS5 without remote resolution are not representable.
enum class RouteProxyType : u8 {
    HTTP,
    HTTPS,
    SOCKS4A,
    SOCKS5Hostname,
};

enum class StreamIsolation : u8 {
    // Every stream uses the route's own credentials, if any.
    None,
    // Each identity and top-level site gets its own SOCKS5 username and password, which Tor's default IsolateSOCKSAuth
    // keeps on separate circuits.
    PerIdentityAndSite,
};

// The one network route that every eligible request of a session must take. The proxy endpoint is pinned to an IP
// literal so that reaching it needs no name resolution. Only network owners receive a route: it can carry proxy
// credentials and the isolation secret.
struct PRIVACY_API ProxyRoute {
    RouteProxyType type { RouteProxyType::SOCKS5Hostname };
    // An IPv4 or IPv6 literal, without brackets.
    ByteString host;
    u16 port { 0 };
    // Optional static userinfo ("user:password") for the proxy. Not allowed together with stream isolation.
    ByteString credentials;
    StreamIsolation isolation { StreamIsolation::None };
    // Secret the isolation credentials are derived from; required with isolation, empty otherwise.
    ByteString isolation_secret;
    // Changing the generation changes every isolation credential, so no later stream shares a circuit with an earlier one.
    u64 isolation_generation { 0 };

    // Parses a proxy URL such as "socks5h://127.0.0.1:9050" or "http://[::1]:8080". Rejects proxy types that resolve
    // names locally, host names, and anything after the authority.
    static Optional<ProxyRoute> parse(StringView);

    bool is_valid() const;

    // The route without credentials or secrets, for diagnostics and the UI.
    ByteString redacted_description() const;

    bool operator==(ProxyRoute const&) const = default;
};

struct PRIVACY_API IsolationCredentials {
    ByteString username;
    ByteString password;

    bool operator==(IsolationCredentials const&) const = default;
};

// Derives the SOCKS5 credentials for one stream. The username separates identities (each identity's session has its own
// secret) and the password separates top-level sites. An empty site names browser-initiated work. Returns nothing when
// the route does not isolate streams.
PRIVACY_API Optional<IsolationCredentials> isolation_credentials_for(ProxyRoute const&, StringView top_level_site);

PRIVACY_API StringView route_proxy_type_to_string(RouteProxyType);

}

namespace IPC {

template<>
PRIVACY_API ErrorOr<void> encode(Encoder&, Privacy::ProxyRoute const&);

template<>
PRIVACY_API ErrorOr<Privacy::ProxyRoute> decode(Decoder&);

}
