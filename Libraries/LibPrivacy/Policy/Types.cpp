/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibPrivacy/Policy/Types.h>

namespace Privacy {

Optional<NetworkMode> network_mode_from_string(StringView value)
{
    if (value.equals_ignoring_ascii_case("allow"sv))
        return NetworkMode::Allow;
    if (value.equals_ignoring_ascii_case("offline"sv))
        return NetworkMode::Offline;
    if (value.equals_ignoring_ascii_case("proxy-only"sv))
        return NetworkMode::ProxyOnly;
    return {};
}

StringView network_mode_to_string(NetworkMode mode)
{
    switch (mode) {
    case NetworkMode::Allow:
        return "allow"sv;
    case NetworkMode::Offline:
        return "offline"sv;
    case NetworkMode::ProxyOnly:
        return "proxy-only"sv;
    }
    return "unknown"sv;
}

StringView policy_reason_to_string(PolicyReason reason)
{
    switch (reason) {
    case PolicyReason::None:
        return "none"sv;
    case PolicyReason::UninitializedClosed:
        return "uninitialized-closed"sv;
    case PolicyReason::CoordinatorLost:
        return "coordinator-lost"sv;
    case PolicyReason::StaleEpoch:
        return "stale-epoch"sv;
    case PolicyReason::UnsupportedSchemaVersion:
        return "unsupported-schema-version"sv;
    case PolicyReason::SessionMismatch:
        return "session-mismatch"sv;
    case PolicyReason::OfflineMode:
        return "offline-mode"sv;
    case PolicyReason::AdministrativeRestriction:
        return "administrative-restriction"sv;
    case PolicyReason::InvalidPolicy:
        return "invalid-policy"sv;
    case PolicyReason::NetworkDenied:
        return "network-denied"sv;
    case PolicyReason::MandatoryRouteUnavailable:
        return "mandatory-route-unavailable"sv;
    case PolicyReason::ConflictingRoutes:
        return "conflicting-routes"sv;
    }
    return "unknown"sv;
}

NetworkPolicy NetworkPolicy::allow_default()
{
    return {
        .mode = NetworkMode::Allow,
        .cache_policy = CachePolicy::Normal,
        .dns_policy = DNSRoutePolicy::Default,
        .webrtc_policy = WebRTCPolicy::Allowed,
        .allow_background_services = true,
        .allow_speculative_connections = true,
        .allow_websockets = true,
        .allow_aia_fetches = true,
    };
}

NetworkPolicy NetworkPolicy::offline()
{
    return {
        .mode = NetworkMode::Offline,
        .cache_policy = CachePolicy::FreshOnlyNoRevalidation,
        .dns_policy = DNSRoutePolicy::Denied,
        .webrtc_policy = WebRTCPolicy::Disabled,
        .allow_background_services = false,
        .allow_speculative_connections = false,
        .allow_websockets = false,
        .allow_aia_fetches = false,
    };
}

NetworkPolicy NetworkPolicy::deny_all()
{
    return {
        .mode = NetworkMode::Offline,
        .cache_policy = CachePolicy::Disabled,
        .dns_policy = DNSRoutePolicy::Denied,
        .webrtc_policy = WebRTCPolicy::Disabled,
        .allow_background_services = false,
        .allow_speculative_connections = false,
        .allow_websockets = false,
        .allow_aia_fetches = false,
    };
}

NetworkPolicy NetworkPolicy::proxy_only(ProxyRoute route)
{
    return {
        .mode = NetworkMode::ProxyOnly,
        .cache_policy = CachePolicy::Normal,
        .dns_policy = DNSRoutePolicy::Denied,
        .webrtc_policy = WebRTCPolicy::Disabled,
        // Browser services, WebSockets and AIA fetches all go through RequestServer, which sends them over the route.
        .allow_background_services = true,
        // Speculative work resolves and connects ahead of a request, which would bypass the route.
        .allow_speculative_connections = false,
        .allow_websockets = true,
        .allow_aia_fetches = true,
        .route = move(route),
    };
}

static CachePolicy intersect_cache_policy(CachePolicy a, CachePolicy b)
{
    if (a == CachePolicy::Disabled || b == CachePolicy::Disabled)
        return CachePolicy::Disabled;
    if (a == CachePolicy::FreshOnlyNoRevalidation || b == CachePolicy::FreshOnlyNoRevalidation)
        return CachePolicy::FreshOnlyNoRevalidation;
    return CachePolicy::Normal;
}

NetworkPolicy NetworkPolicy::intersect(NetworkPolicy const& a, NetworkPolicy const& b)
{
    // Two different mandatory routes have no common route, so their intersection allows no egress at all.
    bool conflicting_routes = a.route.has_value() && b.route.has_value() && *a.route != *b.route;

    auto mode = (a.allows_network_egress() && b.allows_network_egress() && !conflicting_routes)
        ? NetworkMode::Allow
        : NetworkMode::Offline;
    auto cache_policy = intersect_cache_policy(a.cache_policy, b.cache_policy);
    if (mode == NetworkMode::Offline && cache_policy == CachePolicy::Normal)
        cache_policy = CachePolicy::FreshOnlyNoRevalidation;

    if (mode == NetworkMode::Offline) {
        return {
            .mode = NetworkMode::Offline,
            .cache_policy = cache_policy,
            .dns_policy = DNSRoutePolicy::Denied,
            .webrtc_policy = WebRTCPolicy::Disabled,
            .allow_background_services = false,
            .allow_speculative_connections = false,
            .allow_websockets = false,
            .allow_aia_fetches = false,
        };
    }

    auto route = a.route.has_value() ? a.route : b.route;
    if (route.has_value()) {
        return {
            .mode = NetworkMode::ProxyOnly,
            .cache_policy = cache_policy,
            .dns_policy = DNSRoutePolicy::Denied,
            .webrtc_policy = WebRTCPolicy::Disabled,
            .allow_background_services = a.allow_background_services && b.allow_background_services,
            .allow_speculative_connections = false,
            .allow_websockets = a.allow_websockets && b.allow_websockets,
            .allow_aia_fetches = a.allow_aia_fetches && b.allow_aia_fetches,
            .route = move(route),
        };
    }

    return {
        .mode = NetworkMode::Allow,
        .cache_policy = cache_policy,
        .dns_policy = (a.dns_policy == DNSRoutePolicy::Default && b.dns_policy == DNSRoutePolicy::Default)
            ? DNSRoutePolicy::Default
            : DNSRoutePolicy::Denied,
        .webrtc_policy = (a.webrtc_policy == WebRTCPolicy::Allowed && b.webrtc_policy == WebRTCPolicy::Allowed)
            ? WebRTCPolicy::Allowed
            : WebRTCPolicy::Disabled,
        .allow_background_services = a.allow_background_services && b.allow_background_services,
        .allow_speculative_connections = a.allow_speculative_connections && b.allow_speculative_connections,
        .allow_websockets = a.allow_websockets && b.allow_websockets,
        .allow_aia_fetches = a.allow_aia_fetches && b.allow_aia_fetches,
    };
}

bool NetworkPolicy::is_consistent() const
{
    // A route exists exactly in ProxyOnly mode, and it must be one that needs no local name resolution to reach.
    if (route.has_value() != (mode == NetworkMode::ProxyOnly))
        return false;
    if (mode == NetworkMode::ProxyOnly) {
        if (!route->is_valid())
            return false;
        if (dns_policy != DNSRoutePolicy::Denied || webrtc_policy != WebRTCPolicy::Disabled || allow_speculative_connections)
            return false;
    }
    if (mode == NetworkMode::Offline) {
        if (cache_policy == CachePolicy::Normal)
            return false;
        if (dns_policy != DNSRoutePolicy::Denied)
            return false;
        if (webrtc_policy != WebRTCPolicy::Disabled)
            return false;
        if (allow_background_services || allow_speculative_connections || allow_websockets || allow_aia_fetches)
            return false;
    }
    return true;
}

bool NetworkPolicy::is_tightening_relative_to(NetworkPolicy const& previous) const
{
    // Any route change retires the connections, lookups and streams of the previous route, including a change from
    // direct egress to a mandatory route, a different endpoint, or a new isolation generation.
    if (route.has_value() && route != previous.route)
        return true;
    if (previous.allows_network_egress() && !allows_network_egress())
        return true;
    if (previous.allows_dns_resolution() && !allows_dns_resolution())
        return true;
    if (previous.allows_webrtc_network() && !allows_webrtc_network())
        return true;
    if (previous.allows_network_revalidation() && !allows_network_revalidation())
        return true;
    if (previous.allows_fresh_cache_without_revalidation() && !allows_fresh_cache_without_revalidation())
        return true;
    if (previous.allow_background_services && !allow_background_services)
        return true;
    if (previous.allow_speculative_connections && !allow_speculative_connections)
        return true;
    if (previous.allow_websockets && !allow_websockets)
        return true;
    if (previous.allow_aia_fetches && !allow_aia_fetches)
        return true;
    return false;
}

bool PolicySnapshot::is_valid() const
{
    return schema_version == CURRENT_POLICY_SCHEMA_VERSION
        && session_id != 0
        && epoch != 0
        && network.is_consistent();
}

bool PolicySnapshot::is_tightening_relative_to(PolicySnapshot const& previous) const
{
    return network.is_tightening_relative_to(previous.network);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Privacy::NetworkPolicy const& policy)
{
    TRY(encoder.encode(to_underlying(policy.mode)));
    TRY(encoder.encode(to_underlying(policy.cache_policy)));
    TRY(encoder.encode(to_underlying(policy.dns_policy)));
    TRY(encoder.encode(to_underlying(policy.webrtc_policy)));
    TRY(encoder.encode(policy.allow_background_services));
    TRY(encoder.encode(policy.allow_speculative_connections));
    TRY(encoder.encode(policy.allow_websockets));
    TRY(encoder.encode(policy.allow_aia_fetches));
    TRY(encoder.encode(policy.route));
    return {};
}

template<>
ErrorOr<Privacy::NetworkPolicy> decode(Decoder& decoder)
{
    auto raw_mode = TRY(decoder.decode<u8>());
    if (raw_mode > to_underlying(Privacy::NetworkMode::ProxyOnly))
        return Error::from_string_literal("IPC: Invalid Privacy::NetworkMode value");

    auto raw_cache_policy = TRY(decoder.decode<u8>());
    if (raw_cache_policy > to_underlying(Privacy::CachePolicy::Disabled))
        return Error::from_string_literal("IPC: Invalid Privacy::CachePolicy value");

    auto raw_dns_policy = TRY(decoder.decode<u8>());
    if (raw_dns_policy > to_underlying(Privacy::DNSRoutePolicy::Denied))
        return Error::from_string_literal("IPC: Invalid Privacy::DNSRoutePolicy value");

    auto raw_webrtc_policy = TRY(decoder.decode<u8>());
    if (raw_webrtc_policy > to_underlying(Privacy::WebRTCPolicy::Disabled))
        return Error::from_string_literal("IPC: Invalid Privacy::WebRTCPolicy value");

    auto allow_background_services = TRY(decoder.decode<bool>());
    auto allow_speculative_connections = TRY(decoder.decode<bool>());
    auto allow_websockets = TRY(decoder.decode<bool>());
    auto allow_aia_fetches = TRY(decoder.decode<bool>());
    auto route = TRY(decoder.decode<Optional<Privacy::ProxyRoute>>());

    return Privacy::NetworkPolicy {
        .mode = static_cast<Privacy::NetworkMode>(raw_mode),
        .cache_policy = static_cast<Privacy::CachePolicy>(raw_cache_policy),
        .dns_policy = static_cast<Privacy::DNSRoutePolicy>(raw_dns_policy),
        .webrtc_policy = static_cast<Privacy::WebRTCPolicy>(raw_webrtc_policy),
        .allow_background_services = allow_background_services,
        .allow_speculative_connections = allow_speculative_connections,
        .allow_websockets = allow_websockets,
        .allow_aia_fetches = allow_aia_fetches,
        .route = move(route),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Privacy::PolicySnapshot const& snapshot)
{
    TRY(encoder.encode(snapshot.schema_version));
    TRY(encoder.encode(snapshot.session_id));
    TRY(encoder.encode(snapshot.epoch));
    TRY(encoder.encode(snapshot.network));
    return {};
}

template<>
ErrorOr<Privacy::PolicySnapshot> decode(Decoder& decoder)
{
    auto schema_version = TRY(decoder.decode<u32>());
    auto session_id = TRY(decoder.decode<u64>());
    auto epoch = TRY(decoder.decode<u64>());
    auto network = TRY(decoder.decode<Privacy::NetworkPolicy>());
    return Privacy::PolicySnapshot {
        .schema_version = schema_version,
        .session_id = session_id,
        .epoch = epoch,
        .network = move(network),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Privacy::PolicyInstallAck const& ack)
{
    TRY(encoder.encode(ack.accepted));
    TRY(encoder.encode(ack.committed_epoch));
    TRY(encoder.encode(to_underlying(ack.rejection_reason)));
    return {};
}

template<>
ErrorOr<Privacy::PolicyInstallAck> decode(Decoder& decoder)
{
    auto accepted = TRY(decoder.decode<bool>());
    auto committed_epoch = TRY(decoder.decode<u64>());
    auto raw_reason = TRY(decoder.decode<u8>());
    if (raw_reason > to_underlying(Privacy::PolicyReason::ConflictingRoutes))
        return Error::from_string_literal("IPC: Invalid Privacy::PolicyReason value");

    return Privacy::PolicyInstallAck {
        .accepted = accepted,
        .committed_epoch = committed_epoch,
        .rejection_reason = static_cast<Privacy::PolicyReason>(raw_reason),
    };
}

}
