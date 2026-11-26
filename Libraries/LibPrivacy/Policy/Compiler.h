/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibPrivacy/Export.h>
#include <LibPrivacy/Policy/Types.h>

namespace Privacy {

struct PRIVACY_API PolicyInput {
    u32 schema_version { CURRENT_POLICY_SCHEMA_VERSION };
    // Optional locked administrative ceiling. If set, the effective policy can never be wider.
    Optional<NetworkPolicy> administrative_ceiling {};
    // Optional identity-bound requirement (e.g. Offline identity). Intersected with administrative_ceiling.
    Optional<NetworkPolicy> identity_requirement {};
    // Configurable preference resolved within the mandatory ceiling.
    NetworkPolicy user_preference { NetworkPolicy::allow_default() };
    // Records whether an external/administrative proxy setting was supplied alongside an Offline requirement
    // so the compiler keeps networking closed and emits an explicit diagnostic.
    bool has_configured_proxy { false };
};

struct PRIVACY_API PolicyCompilationResult {
    PolicySnapshot snapshot;
    Vector<ByteString> diagnostics;
};

class PRIVACY_API PolicyCompiler {
public:
    static ErrorOr<PolicyCompilationResult, PolicyReason> compile(u64 session_id, u64 next_epoch, PolicyInput const&);
};

}
