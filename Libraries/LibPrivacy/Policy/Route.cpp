/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <AK/StringBuilder.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibPrivacy/Policy/Route.h>

namespace Privacy {

static bool is_ip_literal(StringView host)
{
    return IPv4Address::from_string(host).has_value() || IPv6Address::from_string(host).has_value();
}

Optional<ProxyRoute> ProxyRoute::parse(StringView string)
{
    string = string.trim_whitespace();

    auto scheme_end = string.find("://"sv);
    if (!scheme_end.has_value())
        return {};

    ProxyRoute route;
    auto scheme = string.substring_view(0, *scheme_end);
    if (scheme.equals_ignoring_ascii_case("http"sv))
        route.type = RouteProxyType::HTTP;
    else if (scheme.equals_ignoring_ascii_case("https"sv))
        route.type = RouteProxyType::HTTPS;
    else if (scheme.equals_ignoring_ascii_case("socks4a"sv))
        route.type = RouteProxyType::SOCKS4A;
    else if (scheme.equals_ignoring_ascii_case("socks5h"sv))
        route.type = RouteProxyType::SOCKS5Hostname;
    else
        return {};

    auto authority = string.substring_view(*scheme_end + 3);
    if (authority.ends_with('/'))
        authority = authority.substring_view(0, authority.length() - 1);
    if (authority.contains('/') || authority.contains('?') || authority.contains('#'))
        return {};

    if (auto userinfo_end = authority.find_last('@'); userinfo_end.has_value()) {
        route.credentials = authority.substring_view(0, *userinfo_end);
        authority = authority.substring_view(*userinfo_end + 1);
    }

    StringView port;
    if (authority.starts_with('[')) {
        auto host_end = authority.find(']');
        if (!host_end.has_value())
            return {};
        route.host = authority.substring_view(1, *host_end - 1);
        auto rest = authority.substring_view(*host_end + 1);
        if (!rest.starts_with(':'))
            return {};
        port = rest.substring_view(1);
    } else {
        auto port_start = authority.find_last(':');
        if (!port_start.has_value())
            return {};
        route.host = authority.substring_view(0, *port_start);
        port = authority.substring_view(*port_start + 1);
    }

    auto port_number = port.to_number<u16>();
    if (!port_number.has_value())
        return {};
    route.port = *port_number;

    if (!route.is_valid())
        return {};
    return route;
}

bool ProxyRoute::is_valid() const
{
    if (host.is_empty() || port == 0 || !is_ip_literal(host))
        return false;
    // A zone identifier names a local interface, which a pinned route has no use for.
    if (host.contains('%'))
        return false;
    switch (isolation) {
    case StreamIsolation::None:
        return isolation_secret.is_empty();
    case StreamIsolation::PerIdentityAndSite:
        // Isolation replaces the SOCKS5 credentials, so it cannot be combined with static ones.
        return type == RouteProxyType::SOCKS5Hostname && credentials.is_empty() && isolation_secret.length() >= 16;
    }
    return false;
}

StringView route_proxy_type_to_string(RouteProxyType type)
{
    switch (type) {
    case RouteProxyType::HTTP:
        return "http"sv;
    case RouteProxyType::HTTPS:
        return "https"sv;
    case RouteProxyType::SOCKS4A:
        return "socks4a"sv;
    case RouteProxyType::SOCKS5Hostname:
        return "socks5h"sv;
    }
    return "unknown"sv;
}

ByteString ProxyRoute::redacted_description() const
{
    StringBuilder builder;
    builder.appendff("{}://", route_proxy_type_to_string(type));
    if (host.contains(':'))
        builder.appendff("[{}]", host);
    else
        builder.append(host);
    builder.appendff(":{}", port);
    if (isolation == StreamIsolation::PerIdentityAndSite)
        builder.append(" (isolated per identity and site)"sv);
    return builder.to_byte_string();
}

static ByteString derive_token(ProxyRoute const& route, u8 purpose, StringView site)
{
    Crypto::Hash::Hasher hasher { Crypto::Hash::HashKind::SHA256 };
    hasher.update("ladyflock-route-isolation-v1"sv);
    hasher.update(ReadonlyBytes { &purpose, 1 });
    hasher.update(route.isolation_secret.view());
    auto generation = route.isolation_generation;
    hasher.update(ReadonlyBytes { reinterpret_cast<u8 const*>(&generation), sizeof(generation) });
    // Length-prefix the site so that no two sites hash the same input.
    u64 site_length = site.length();
    hasher.update(ReadonlyBytes { reinterpret_cast<u8 const*>(&site_length), sizeof(site_length) });
    hasher.update(site);
    auto digest = hasher.finish();
    // 128 bits is plenty to keep tokens apart and stays well inside SOCKS5's 255-byte limits.
    return encode_hex(digest.bytes().trim(16));
}

Optional<IsolationCredentials> isolation_credentials_for(ProxyRoute const& route, StringView top_level_site)
{
    if (route.isolation != StreamIsolation::PerIdentityAndSite || !route.is_valid())
        return {};
    return IsolationCredentials {
        .username = derive_token(route, 0, {}),
        .password = derive_token(route, 1, top_level_site),
    };
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Privacy::ProxyRoute const& route)
{
    TRY(encoder.encode(to_underlying(route.type)));
    TRY(encoder.encode(route.host));
    TRY(encoder.encode(route.port));
    TRY(encoder.encode(route.credentials));
    TRY(encoder.encode(to_underlying(route.isolation)));
    TRY(encoder.encode(route.isolation_secret));
    TRY(encoder.encode(route.isolation_generation));
    return {};
}

template<>
ErrorOr<Privacy::ProxyRoute> decode(Decoder& decoder)
{
    auto raw_type = TRY(decoder.decode<u8>());
    if (raw_type > to_underlying(Privacy::RouteProxyType::SOCKS5Hostname))
        return Error::from_string_literal("IPC: Invalid Privacy::RouteProxyType value");
    auto host = TRY(decoder.decode<ByteString>());
    auto port = TRY(decoder.decode<u16>());
    auto credentials = TRY(decoder.decode<ByteString>());
    auto raw_isolation = TRY(decoder.decode<u8>());
    if (raw_isolation > to_underlying(Privacy::StreamIsolation::PerIdentityAndSite))
        return Error::from_string_literal("IPC: Invalid Privacy::StreamIsolation value");
    auto isolation_secret = TRY(decoder.decode<ByteString>());
    auto isolation_generation = TRY(decoder.decode<u64>());

    Privacy::ProxyRoute route {
        .type = static_cast<Privacy::RouteProxyType>(raw_type),
        .host = move(host),
        .port = port,
        .credentials = move(credentials),
        .isolation = static_cast<Privacy::StreamIsolation>(raw_isolation),
        .isolation_secret = move(isolation_secret),
        .isolation_generation = isolation_generation,
    };
    if (!route.is_valid())
        return Error::from_string_literal("IPC: Invalid Privacy::ProxyRoute");
    return route;
}

}
