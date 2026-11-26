/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPrivacy/Policy/Compiler.h>

namespace Privacy {

ErrorOr<PolicyCompilationResult, PolicyReason> PolicyCompiler::compile(u64 session_id, u64 next_epoch, PolicyInput const& input)
{
    if (input.schema_version != CURRENT_POLICY_SCHEMA_VERSION)
        return PolicyReason::UnsupportedSchemaVersion;

    if (session_id == 0 || next_epoch == 0)
        return PolicyReason::InvalidPolicy;

    if (!input.user_preference.is_consistent())
        return PolicyReason::InvalidPolicy;

    if (input.administrative_ceiling.has_value() && !input.administrative_ceiling->is_consistent())
        return PolicyReason::InvalidPolicy;

    if (input.identity_requirement.has_value() && !input.identity_requirement->is_consistent())
        return PolicyReason::InvalidPolicy;

    Vector<ByteString> diagnostics;

    // Every mandatory route among the inputs must be the same route; otherwise no route satisfies all of them and the
    // intersection below closes egress.
    Optional<ProxyRoute> required_route;
    bool has_conflicting_routes = false;
    for (auto const* policy : { input.administrative_ceiling.has_value() ? &*input.administrative_ceiling : nullptr,
             input.identity_requirement.has_value() ? &*input.identity_requirement : nullptr,
             &input.user_preference }) {
        if (!policy || !policy->route.has_value())
            continue;
        if (required_route.has_value() && *required_route != *policy->route)
            has_conflicting_routes = true;
        else
            required_route = policy->route;
    }
    if (has_conflicting_routes)
        diagnostics.append("Conflicting mandatory routes; network egress remains closed."sv);

    // Intersect mandatory administrative ceiling and identity requirements first; an empty
    // intersection always resolves to closed networking.
    auto effective = NetworkPolicy::allow_default();
    if (input.administrative_ceiling.has_value())
        effective = NetworkPolicy::intersect(effective, *input.administrative_ceiling);

    if (input.identity_requirement.has_value())
        effective = NetworkPolicy::intersect(effective, *input.identity_requirement);

    auto ceiling = effective;
    effective = NetworkPolicy::intersect(ceiling, input.user_preference);

    if (effective.requires_mandatory_route() && input.has_configured_proxy)
        diagnostics.append("The mandatory route replaces the configured proxy settings, including their bypass list."sv);

    if (!effective.allows_network_egress() && input.has_configured_proxy) {
        diagnostics.append("Offline policy overrides configured proxy settings; network egress remains closed."sv);
    }

    if (input.administrative_ceiling.has_value()
        && input.user_preference.allows_network_egress()
        && !input.administrative_ceiling->allows_network_egress()) {
        diagnostics.append("Administrative restriction keeps network egress closed despite user preference."sv);
    }

    PolicySnapshot snapshot {
        .schema_version = input.schema_version,
        .session_id = session_id,
        .epoch = next_epoch,
        .network = effective,
    };

    if (!snapshot.is_valid())
        return PolicyReason::InvalidPolicy;

    return PolicyCompilationResult {
        .snapshot = move(snapshot),
        .diagnostics = move(diagnostics),
    };
}

}
