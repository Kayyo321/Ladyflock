/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibIPC/Forward.h>
#include <LibPrivacy/Export.h>

namespace Privacy {

constexpr inline u32 CURRENT_POLICY_SCHEMA_VERSION = 1;

enum class NetworkMode : u8 {
    Allow,
    Offline,
};

enum class CachePolicy : u8 {
    Normal,
    FreshOnlyNoRevalidation,
    Disabled,
};

enum class DNSRoutePolicy : u8 {
    Default,
    Denied,
};

enum class WebRTCPolicy : u8 {
    Allowed,
    Disabled,
};

enum class PolicyDecision : u8 {
    Allow,
    Deny,
};

enum class PolicyReason : u8 {
    None,
    UninitializedClosed,
    CoordinatorLost,
    StaleEpoch,
    UnsupportedSchemaVersion,
    SessionMismatch,
    OfflineMode,
    AdministrativeRestriction,
    InvalidPolicy,
    NetworkDenied,
};

PRIVACY_API Optional<NetworkMode> network_mode_from_string(StringView);
PRIVACY_API StringView network_mode_to_string(NetworkMode);
PRIVACY_API StringView policy_reason_to_string(PolicyReason);

// Typed network capabilities carried in immutable policy snapshots.
// Default-constructed state is fail-closed.
struct PRIVACY_API NetworkPolicy {
    NetworkMode mode { NetworkMode::Offline };
    CachePolicy cache_policy { CachePolicy::Disabled };
    DNSRoutePolicy dns_policy { DNSRoutePolicy::Denied };
    WebRTCPolicy webrtc_policy { WebRTCPolicy::Disabled };
    bool allow_background_services { false };
    bool allow_speculative_connections { false };
    bool allow_websockets { false };
    bool allow_aia_fetches { false };

    static NetworkPolicy allow_default();
    static NetworkPolicy offline();
    static NetworkPolicy deny_all();

    static NetworkPolicy intersect(NetworkPolicy const&, NetworkPolicy const&);

    bool allows_network_egress() const { return mode == NetworkMode::Allow; }
    bool allows_dns_resolution() const { return allows_network_egress() && dns_policy == DNSRoutePolicy::Default; }
    bool allows_webrtc_network() const { return allows_network_egress() && webrtc_policy == WebRTCPolicy::Allowed; }
    bool allows_fresh_cache_without_revalidation() const { return cache_policy == CachePolicy::Normal || cache_policy == CachePolicy::FreshOnlyNoRevalidation; }
    bool allows_network_revalidation() const { return allows_network_egress() && cache_policy == CachePolicy::Normal; }

    // Rejects contradictory combinations (such as Offline claiming open DNS or WebSockets).
    bool is_consistent() const;

    // True whenever this policy narrows any capability that was permitted by `previous`.
    bool is_tightening_relative_to(NetworkPolicy const& previous) const;

    bool operator==(NetworkPolicy const&) const = default;
};

// Immutable, session-bound, versioned snapshot pushed by Hub to enforcing processes.
struct PRIVACY_API PolicySnapshot {
    u32 schema_version { CURRENT_POLICY_SCHEMA_VERSION };
    u64 session_id { 0 };
    u64 epoch { 0 };
    NetworkPolicy network {};

    bool is_valid() const;
    bool is_tightening_relative_to(PolicySnapshot const& previous) const;

    bool operator==(PolicySnapshot const&) const = default;
};

// Synchronous acknowledgement returned by an enforcing helper when installing a PolicySnapshot.
struct PRIVACY_API PolicyInstallAck {
    bool accepted { false };
    u64 committed_epoch { 0 };
    PolicyReason rejection_reason { PolicyReason::UninitializedClosed };

    bool operator==(PolicyInstallAck const&) const = default;
};

}

namespace IPC {

template<>
PRIVACY_API ErrorOr<void> encode(Encoder&, Privacy::NetworkPolicy const&);

template<>
PRIVACY_API ErrorOr<Privacy::NetworkPolicy> decode(Decoder&);

template<>
PRIVACY_API ErrorOr<void> encode(Encoder&, Privacy::PolicySnapshot const&);

template<>
PRIVACY_API ErrorOr<Privacy::PolicySnapshot> decode(Decoder&);

template<>
PRIVACY_API ErrorOr<void> encode(Encoder&, Privacy::PolicyInstallAck const&);

template<>
PRIVACY_API ErrorOr<Privacy::PolicyInstallAck> decode(Decoder&);

}
