/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPrivacy/Identity/NavigationHandoff.h>

namespace Privacy {

u8 NavigationHandoffEvaluator::route_restrictiveness_rank(IdentityDescriptor const& identity, NetworkPolicy const& policy)
{
    if (policy.mode == NetworkMode::Offline || identity.kind == IdentityKind::Offline)
        return 3;
    if (identity.kind == IdentityKind::TorRouted)
        return 2;
    if (!policy.allows_webrtc_network() || !policy.allow_background_services)
        return 1;
    return 0;
}

NavigationHandoffDecision NavigationHandoffEvaluator::evaluate(NavigationHandoffRequest const& request)
{
    auto displayed_url = request.destination_url.serialize();

    if (!request.source_identity.is_valid() || !request.target_identity.is_valid()) {
        return {
            .action = HandoffAction::Deny,
            .target_identity_id = {},
            .displayed_destination_url = move(displayed_url),
            .open_fresh_context = false,
            .drop_opener = true,
            .strip_referrer = true,
            .strip_credentials = true,
            .replay_post_body = false,
            .reason = "Invalid source or target identity"sv,
        };
    }

    if (request.source_identity.id == request.target_identity.id) {
        return {
            .action = HandoffAction::StayInCurrentIdentity,
            .target_identity_id = request.source_identity.id,
            .displayed_destination_url = move(displayed_url),
            .open_fresh_context = false,
            .drop_opener = false,
            .strip_referrer = false,
            .strip_credentials = false,
            .replay_post_body = request.has_post_body,
            .reason = {},
        };
    }

    // Crossing from a more restrictive route to a less restrictive one requires explicit confirmation
    // before any DNS lookup or network dispatch occurs.
    auto source_rank = route_restrictiveness_rank(request.source_identity, request.source_policy);
    auto target_rank = route_restrictiveness_rank(request.target_identity, request.target_policy);
    if (source_rank > target_rank && !request.user_confirmed) {
        return {
            .action = HandoffAction::RequiresConfirmation,
            .target_identity_id = request.target_identity.id,
            .displayed_destination_url = move(displayed_url),
            .open_fresh_context = true,
            .drop_opener = true,
            .strip_referrer = true,
            .strip_credentials = true,
            .replay_post_body = false,
            .reason = ByteString::formatted(
                "Navigation from '{}' to less restrictive identity '{}' requires user confirmation for {}",
                request.source_identity.id,
                request.target_identity.id,
                request.destination_url.serialize()),
        };
    }

    // Never replay a POST body across an identity boundary; always open a fresh context with opener,
    // referrer, and source credentials stripped.
    return {
        .action = HandoffAction::HandoffToNewContext,
        .target_identity_id = request.target_identity.id,
        .displayed_destination_url = move(displayed_url),
        .open_fresh_context = true,
        .drop_opener = true,
        .strip_referrer = true,
        .strip_credentials = true,
        .replay_post_body = false,
        .reason = request.has_post_body
            ? ByteString { "Cross-identity handoff opens a fresh context without replaying POST body" }
            : ByteString { "Cross-identity handoff opens a fresh context" },
    };
}

}
