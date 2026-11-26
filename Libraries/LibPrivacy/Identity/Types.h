/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibIPC/Forward.h>
#include <LibPrivacy/Export.h>
#include <LibPrivacy/Policy/Types.h>

namespace Privacy {

constexpr inline StringView DEFAULT_IDENTITY_ID = "default"sv;
constexpr inline StringView PRIVATE_IDENTITY_ID = "private"sv;

enum class IdentityKind : u8 {
    Persistent,
    Temporary,
    Disposable,
    Offline,
    TorRouted,
    DirectNetwork,
};

PRIVACY_API bool is_valid_identity_id(StringView id);
PRIVACY_API StringView identity_kind_to_string(IdentityKind);

// Declarative identity specification owned by Hub and mapped to a BrowsingSession.
struct PRIVACY_API IdentityDescriptor {
    String id;
    String display_name;
    IdentityKind kind { IdentityKind::Persistent };
    NetworkMode network_mode { NetworkMode::Allow };
    bool ephemeral_storage { false };
    bool persist_history { true };
    bool inherit_ordinary_permission_blocks { false };

    static IdentityDescriptor default_persistent();
    static IdentityDescriptor private_ephemeral();
    static IdentityDescriptor persistent(String id, String display_name);
    static IdentityDescriptor temporary(String id, String display_name);
    static IdentityDescriptor disposable(String id, String display_name);
    static IdentityDescriptor offline(String id, String display_name, bool ephemeral = true);
    static IdentityDescriptor tor_routed(String id, String display_name);

    bool is_valid() const;
    bool is_default_identity() const { return id == DEFAULT_IDENTITY_ID; }
    bool is_ephemeral() const { return ephemeral_storage || kind == IdentityKind::Temporary || kind == IdentityKind::Disposable; }

    bool operator==(IdentityDescriptor const&) const = default;
};

// Domain-to-identity assignment rule evaluated in Hub before navigation dispatch.
// Supports exact hosts ("work.example") and wildcard subdomains ("*.work.example").
struct PRIVACY_API DomainAssignmentRule {
    String pattern;
    String target_identity_id;

    bool is_valid() const;
    bool matches_host(StringView host) const;

    bool operator==(DomainAssignmentRule const&) const = default;
};

}

namespace IPC {

template<>
PRIVACY_API ErrorOr<void> encode(Encoder&, Privacy::IdentityDescriptor const&);

template<>
PRIVACY_API ErrorOr<Privacy::IdentityDescriptor> decode(Decoder&);

template<>
PRIVACY_API ErrorOr<void> encode(Encoder&, Privacy::DomainAssignmentRule const&);

template<>
PRIVACY_API ErrorOr<Privacy::DomainAssignmentRule> decode(Decoder&);

}
