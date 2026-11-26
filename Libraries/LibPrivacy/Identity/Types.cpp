/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibPrivacy/Identity/Types.h>

namespace Privacy {

bool is_valid_identity_id(StringView id)
{
    if (id.is_empty() || id.length() > 64)
        return false;
    if (!is_ascii_lower_alpha(id[0]) && !is_ascii_digit(id[0]))
        return false;
    return all_of(id, [](char c) {
        return is_ascii_lower_alpha(c) || is_ascii_digit(c) || c == '-' || c == '_';
    });
}

StringView identity_kind_to_string(IdentityKind kind)
{
    switch (kind) {
    case IdentityKind::Persistent:
        return "persistent"sv;
    case IdentityKind::Temporary:
        return "temporary"sv;
    case IdentityKind::Disposable:
        return "disposable"sv;
    case IdentityKind::Offline:
        return "offline"sv;
    case IdentityKind::TorRouted:
        return "tor-routed"sv;
    case IdentityKind::DirectNetwork:
        return "direct-network"sv;
    }
    return "unknown"sv;
}

IdentityDescriptor IdentityDescriptor::default_persistent()
{
    return {
        .id = MUST(String::from_utf8(DEFAULT_IDENTITY_ID)),
        .display_name = "Default"_string,
        .kind = IdentityKind::Persistent,
        .network_mode = NetworkMode::Allow,
        .ephemeral_storage = false,
        .persist_history = true,
        .inherit_ordinary_permission_blocks = false,
    };
}

IdentityDescriptor IdentityDescriptor::private_ephemeral()
{
    return {
        .id = MUST(String::from_utf8(PRIVATE_IDENTITY_ID)),
        .display_name = "Private"_string,
        .kind = IdentityKind::Temporary,
        .network_mode = NetworkMode::Allow,
        .ephemeral_storage = true,
        .persist_history = false,
        .inherit_ordinary_permission_blocks = true,
    };
}

IdentityDescriptor IdentityDescriptor::persistent(String id, String display_name)
{
    return {
        .id = move(id),
        .display_name = move(display_name),
        .kind = IdentityKind::Persistent,
        .network_mode = NetworkMode::Allow,
        .ephemeral_storage = false,
        .persist_history = true,
        .inherit_ordinary_permission_blocks = false,
    };
}

IdentityDescriptor IdentityDescriptor::temporary(String id, String display_name)
{
    return {
        .id = move(id),
        .display_name = move(display_name),
        .kind = IdentityKind::Temporary,
        .network_mode = NetworkMode::Allow,
        .ephemeral_storage = true,
        .persist_history = false,
        .inherit_ordinary_permission_blocks = true,
    };
}

IdentityDescriptor IdentityDescriptor::disposable(String id, String display_name)
{
    return {
        .id = move(id),
        .display_name = move(display_name),
        .kind = IdentityKind::Disposable,
        .network_mode = NetworkMode::Allow,
        .ephemeral_storage = true,
        .persist_history = false,
        .inherit_ordinary_permission_blocks = true,
    };
}

IdentityDescriptor IdentityDescriptor::offline(String id, String display_name, bool ephemeral)
{
    return {
        .id = move(id),
        .display_name = move(display_name),
        .kind = IdentityKind::Offline,
        .network_mode = NetworkMode::Offline,
        .ephemeral_storage = ephemeral,
        .persist_history = !ephemeral,
        .inherit_ordinary_permission_blocks = ephemeral,
    };
}

IdentityDescriptor IdentityDescriptor::tor_routed(String id, String display_name)
{
    return {
        .id = move(id),
        .display_name = move(display_name),
        .kind = IdentityKind::TorRouted,
        .network_mode = NetworkMode::Allow,
        .ephemeral_storage = true,
        .persist_history = false,
        .inherit_ordinary_permission_blocks = true,
    };
}

bool IdentityDescriptor::is_valid() const
{
    if (!is_valid_identity_id(id.bytes_as_string_view()))
        return false;
    if (display_name.is_empty())
        return false;
    if (kind == IdentityKind::Offline && network_mode != NetworkMode::Offline)
        return false;
    if (is_ephemeral() && persist_history)
        return false;
    return true;
}

bool DomainAssignmentRule::is_valid() const
{
    if (!is_valid_identity_id(target_identity_id.bytes_as_string_view()))
        return false;
    auto view = pattern.bytes_as_string_view();
    if (view.is_empty() || view.length() > 253)
        return false;
    if (view.starts_with("*."sv))
        view = view.substring_view(2);
    if (view.is_empty() || view.starts_with('.') || view.ends_with('.') || view.contains('*'))
        return false;
    return all_of(view, [](char c) {
        return is_ascii_alphanumeric(c) || c == '-' || c == '.';
    });
}

bool DomainAssignmentRule::matches_host(StringView host) const
{
    if (!is_valid() || host.is_empty())
        return false;

    while (host.ends_with('.'))
        host = host.substring_view(0, host.length() - 1);
    if (host.is_empty())
        return false;

    auto pat = pattern.bytes_as_string_view();
    if (pat.starts_with("*."sv)) {
        auto suffix = pat.substring_view(2);
        if (host.equals_ignoring_ascii_case(suffix))
            return true;
        return host.length() > suffix.length()
            && host[host.length() - suffix.length() - 1] == '.'
            && host.ends_with(suffix, CaseSensitivity::CaseInsensitive);
    }

    return host.equals_ignoring_ascii_case(pat);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Privacy::IdentityDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.id));
    TRY(encoder.encode(descriptor.display_name));
    TRY(encoder.encode(descriptor.kind));
    TRY(encoder.encode(descriptor.network_mode));
    TRY(encoder.encode(descriptor.ephemeral_storage));
    TRY(encoder.encode(descriptor.persist_history));
    TRY(encoder.encode(descriptor.inherit_ordinary_permission_blocks));
    return {};
}

template<>
ErrorOr<Privacy::IdentityDescriptor> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<String>());
    auto display_name = TRY(decoder.decode<String>());
    auto kind = TRY(decoder.decode<Privacy::IdentityKind>());
    if (to_underlying(kind) > to_underlying(Privacy::IdentityKind::DirectNetwork))
        return Error::from_string_literal("Invalid Privacy::IdentityKind");
    auto network_mode = TRY(decoder.decode<Privacy::NetworkMode>());
    if (to_underlying(network_mode) > to_underlying(Privacy::NetworkMode::Offline))
        return Error::from_string_literal("Invalid Privacy::NetworkMode");
    auto ephemeral_storage = TRY(decoder.decode<bool>());
    auto persist_history = TRY(decoder.decode<bool>());
    auto inherit_ordinary_permission_blocks = TRY(decoder.decode<bool>());

    Privacy::IdentityDescriptor descriptor {
        .id = move(id),
        .display_name = move(display_name),
        .kind = kind,
        .network_mode = network_mode,
        .ephemeral_storage = ephemeral_storage,
        .persist_history = persist_history,
        .inherit_ordinary_permission_blocks = inherit_ordinary_permission_blocks,
    };
    if (!descriptor.is_valid())
        return Error::from_string_literal("Invalid Privacy::IdentityDescriptor");
    return descriptor;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Privacy::DomainAssignmentRule const& rule)
{
    TRY(encoder.encode(rule.pattern));
    TRY(encoder.encode(rule.target_identity_id));
    return {};
}

template<>
ErrorOr<Privacy::DomainAssignmentRule> decode(Decoder& decoder)
{
    auto pattern = TRY(decoder.decode<String>());
    auto target_identity_id = TRY(decoder.decode<String>());
    Privacy::DomainAssignmentRule rule { .pattern = move(pattern), .target_identity_id = move(target_identity_id) };
    if (!rule.is_valid())
        return Error::from_string_literal("Invalid Privacy::DomainAssignmentRule");
    return rule;
}

}
