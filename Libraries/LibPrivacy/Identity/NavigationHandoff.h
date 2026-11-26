/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/String.h>
#include <LibPrivacy/Export.h>
#include <LibPrivacy/Identity/Types.h>
#include <LibPrivacy/Policy/Types.h>
#include <LibURL/URL.h>

namespace Privacy {

enum class HandoffAction : u8 {
    StayInCurrentIdentity,
    RequiresConfirmation,
    HandoffToNewContext,
    Deny,
};

struct PRIVACY_API NavigationHandoffRequest {
    IdentityDescriptor source_identity;
    NetworkPolicy source_policy {};
    IdentityDescriptor target_identity;
    NetworkPolicy target_policy {};
    URL::URL destination_url;
    ByteString http_method { "GET" };
    bool has_post_body { false };
    bool has_opener { false };
    bool has_referrer { false };
    bool has_credentials { false };
    bool user_confirmed { false };
};

// Pure decision produced before DNS or dispatch in the source identity.
struct PRIVACY_API NavigationHandoffDecision {
    HandoffAction action { HandoffAction::Deny };
    String target_identity_id;
    // Always shown to the user on confirmation/handoff because URL-embedded identifiers can survive a handoff.
    String displayed_destination_url;
    bool open_fresh_context { false };
    bool drop_opener { false };
    bool strip_referrer { false };
    bool strip_credentials { false };
    bool replay_post_body { false };
    ByteString reason;

    bool is_cross_identity() const { return action == HandoffAction::HandoffToNewContext; }
};

class PRIVACY_API NavigationHandoffEvaluator {
public:
    // Numeric restrictiveness tier: higher means more restrictive route.
    // Crossing from a higher tier to a lower tier requires explicit user confirmation.
    static u8 route_restrictiveness_rank(IdentityDescriptor const&, NetworkPolicy const&);

    static NavigationHandoffDecision evaluate(NavigationHandoffRequest const&);
};

}
