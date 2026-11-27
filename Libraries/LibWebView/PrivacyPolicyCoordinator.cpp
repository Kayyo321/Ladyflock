/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <AK/Random.h>
#include <LibRequests/RequestControlClient.h>
#include <LibWebView/Application.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

PrivacyPolicyCoordinator::PrivacyPolicyCoordinator(Configuration configuration)
    : m_configuration(move(configuration))
{
}

static bool has_configured_proxy()
{
    return !Application::request_server_options().proxy_configuration.is_empty()
        || Application::web_content_options().proxy_autoconfig_url.has_value();
}

static ByteString generate_isolation_secret()
{
    Array<u8, 16> secret;
    fill_with_random(secret);
    return encode_hex(secret);
}

Privacy::ProxyRoute PrivacyPolicyCoordinator::route_for_session(Privacy::ProxyRoute route, bool isolate, ByteString const& isolation_secret, u64 isolation_generation)
{
    if (isolate) {
        route.isolation = Privacy::StreamIsolation::PerIdentityAndSite;
        route.isolation_secret = isolation_secret;
        route.isolation_generation = isolation_generation;
    }
    return route;
}

Privacy::NetworkPolicy PrivacyPolicyCoordinator::preference_for_mode(Privacy::NetworkMode mode, ByteString const& isolation_secret, u64 isolation_generation) const
{
    switch (mode) {
    case Privacy::NetworkMode::Allow:
        return Privacy::NetworkPolicy::allow_default();
    case Privacy::NetworkMode::Offline:
        return Privacy::NetworkPolicy::offline();
    case Privacy::NetworkMode::ProxyOnly:
        // Without a route to take, a proxy-only session takes none.
        if (!m_configuration.proxy_route.has_value())
            return Privacy::NetworkPolicy::deny_all();
        return Privacy::NetworkPolicy::proxy_only(route_for_session(*m_configuration.proxy_route, m_configuration.isolate_proxy_streams, isolation_secret, isolation_generation));
    }
    return Privacy::NetworkPolicy::deny_all();
}

Optional<Privacy::NetworkPolicy> PrivacyPolicyCoordinator::identity_requirement_for_session(BrowsingSession const& session, ByteString const& isolation_secret, u64 isolation_generation) const
{
    auto const& identity = session.identity();
    if (identity.network_mode == Privacy::NetworkMode::Offline || identity.kind == Privacy::IdentityKind::Offline)
        return Privacy::NetworkPolicy::offline();
    if (identity.kind == Privacy::IdentityKind::TorRouted) {
        // A Tor-routed identity never connects directly: without a Tor endpoint, it has no network at all.
        if (!m_configuration.tor_route.has_value())
            return Privacy::NetworkPolicy::deny_all();
        return Privacy::NetworkPolicy::proxy_only(route_for_session(*m_configuration.tor_route, true, isolation_secret, isolation_generation));
    }
    return {};
}

Privacy::PolicyInput PrivacyPolicyCoordinator::default_input_for_session(BrowsingSession const& session, ByteString const& isolation_secret, u64 isolation_generation) const
{
    auto preference = preference_for_mode(m_configuration.default_network_mode, isolation_secret, isolation_generation);
    auto requirement = identity_requirement_for_session(session, isolation_secret, isolation_generation);

    // An identity's own route takes the place of the browser-wide proxy route: a Tor-routed identity keeps to Tor even
    // when the browser's default mode names a different proxy.
    if (requirement.has_value() && requirement->route.has_value() && preference.route.has_value())
        preference = Privacy::NetworkPolicy::allow_default();

    return {
        .schema_version = Privacy::CURRENT_POLICY_SCHEMA_VERSION,
        .administrative_ceiling = m_configuration.administrative_ceiling,
        .identity_requirement = move(requirement),
        .user_preference = move(preference),
        .has_configured_proxy = has_configured_proxy(),
    };
}

Privacy::NetworkPolicy PrivacyPolicyCoordinator::preview_policy(BrowsingSession const& session) const
{
    ByteString const placeholder_secret = "00000000000000000000000000000000"sv;
    auto compiled = Privacy::PolicyCompiler::compile(session.id() ? session.id() : 1, 1, default_input_for_session(session, placeholder_secret, 1));
    if (compiled.is_error())
        return Privacy::NetworkPolicy::deny_all();
    return compiled.value().snapshot.network;
}

PrivacyPolicyCoordinator::SessionState& PrivacyPolicyCoordinator::ensure_session_state(BrowsingSession const& session)
{
    return m_sessions.ensure(session.id(), [&] {
        auto isolation_secret = generate_isolation_secret();
        u64 isolation_generation = 1;
        auto input = default_input_for_session(session, isolation_secret, isolation_generation);
        auto compiled = Privacy::PolicyCompiler::compile(session.id(), allocate_epoch(), input);
        if (compiled.is_error()) {
            // Fail closed if initial compilation ever rejects the input.
            Privacy::PolicySnapshot closed {
                .schema_version = Privacy::CURRENT_POLICY_SCHEMA_VERSION,
                .session_id = session.id(),
                .epoch = allocate_epoch(),
                .network = Privacy::NetworkPolicy::offline(),
            };
            return SessionState {
                .isolation_secret = move(isolation_secret),
                .isolation_generation = isolation_generation,
                .input = input,
                .committed_snapshot = closed,
                .admission_snapshot = closed,
                .last_diagnostics = { "Initial privacy policy rejected; defaulting closed."sv },
            };
        }
        auto result = compiled.release_value();
        for (auto const& diagnostic : result.diagnostics)
            dbgln("Privacy policy for session {}: {}", session.id(), diagnostic);
        return SessionState {
            .isolation_secret = move(isolation_secret),
            .isolation_generation = isolation_generation,
            .input = input,
            .committed_snapshot = result.snapshot,
            .admission_snapshot = result.snapshot,
            .last_diagnostics = move(result.diagnostics),
        };
    });
}

PrivacyPolicyCoordinator::SessionState const* PrivacyPolicyCoordinator::find_session_state(BrowsingSession const& session) const
{
    if (auto it = m_sessions.find(session.id()); it != m_sessions.end())
        return &it->value;
    return nullptr;
}

Privacy::PolicySnapshot const& PrivacyPolicyCoordinator::snapshot_for_session(BrowsingSession const& session)
{
    return ensure_session_state(session).committed_snapshot;
}

Privacy::NetworkMode PrivacyPolicyCoordinator::network_mode(BrowsingSession const& session) const
{
    if (auto const* state = find_session_state(session))
        return state->admission_snapshot.network.mode;
    return preview_policy(session).mode;
}

u64 PrivacyPolicyCoordinator::committed_epoch(BrowsingSession const& session) const
{
    if (auto const* state = find_session_state(session))
        return state->committed_snapshot.epoch;
    return 0;
}

bool PrivacyPolicyCoordinator::allows_network_egress(BrowsingSession const& session) const
{
    if (auto const* state = find_session_state(session))
        return state->admission_snapshot.network.allows_network_egress();
    return preview_policy(session).allows_network_egress();
}

bool PrivacyPolicyCoordinator::allows_dns_resolution(BrowsingSession const& session) const
{
    if (auto const* state = find_session_state(session))
        return state->admission_snapshot.network.allows_dns_resolution();
    return preview_policy(session).allows_dns_resolution();
}

bool PrivacyPolicyCoordinator::allows_webrtc_network(BrowsingSession const& session) const
{
    if (auto const* state = find_session_state(session))
        return state->admission_snapshot.network.allows_webrtc_network();
    return preview_policy(session).allows_webrtc_network();
}

bool PrivacyPolicyCoordinator::allows_background_services(BrowsingSession const& session) const
{
    if (auto const* state = find_session_state(session))
        return state->admission_snapshot.network.allows_network_egress() && state->admission_snapshot.network.allow_background_services;
    auto policy = preview_policy(session);
    return policy.allows_network_egress() && policy.allow_background_services;
}

ErrorOr<void> PrivacyPolicyCoordinator::install_policy_on_request_server(BrowsingSession const& session, Requests::RequestControlClient& control_client)
{
    auto snapshot = snapshot_for_session(session);
    auto response = send_sync_to_helper<Messages::RequestServerControl::InstallPrivacyPolicy>(control_client, snapshot);
    if (!response)
        return Error::from_string_literal("RequestServer failed to acknowledge privacy policy installation");

    auto const& ack = response->ack();
    if (!ack.accepted || ack.committed_epoch != snapshot.epoch) {
        warnln("RequestServer rejected privacy policy epoch {}: {}", snapshot.epoch, Privacy::policy_reason_to_string(ack.rejection_reason));
        return Error::from_string_literal("RequestServer rejected privacy policy snapshot");
    }

    return {};
}

PrivacyPolicyCoordinator::TransitionResult PrivacyPolicyCoordinator::set_network_mode(BrowsingSession& session, Privacy::NetworkMode mode)
{
    auto& state = ensure_session_state(session);
    if (mode == Privacy::NetworkMode::ProxyOnly && !m_configuration.proxy_route.has_value()) {
        return {
            .committed = false,
            .epoch = state.committed_snapshot.epoch,
            .reason = Privacy::PolicyReason::MandatoryRouteUnavailable,
            .diagnostics = { "Proxy-only mode needs a proxy route; the current policy stays."sv },
        };
    }
    auto next_input = state.input;
    next_input.user_preference = preference_for_mode(mode, state.isolation_secret, state.isolation_generation);
    next_input.has_configured_proxy = has_configured_proxy();
    return apply_policy_input(session, next_input);
}

PrivacyPolicyCoordinator::TransitionResult PrivacyPolicyCoordinator::renew_route_isolation(BrowsingSession& session)
{
    auto& state = ensure_session_state(session);
    ++state.isolation_generation;

    auto renew = [&](Optional<Privacy::NetworkPolicy>& policy) {
        if (policy.has_value() && policy->route.has_value() && policy->route->isolation == Privacy::StreamIsolation::PerIdentityAndSite)
            policy->route->isolation_generation = state.isolation_generation;
    };
    auto next_input = state.input;
    renew(next_input.administrative_ceiling);
    renew(next_input.identity_requirement);
    Optional<Privacy::NetworkPolicy> preference = next_input.user_preference;
    renew(preference);
    next_input.user_preference = preference.release_value();
    return apply_policy_input(session, next_input);
}

PrivacyPolicyCoordinator::TransitionResult PrivacyPolicyCoordinator::apply_policy_input(BrowsingSession& session, Privacy::PolicyInput const& input)
{
    auto& state = ensure_session_state(session);

    // An input from anywhere, including one with its own ceiling, stays within the administrator's.
    auto bounded_input = input;
    if (m_configuration.administrative_ceiling.has_value()) {
        bounded_input.administrative_ceiling = input.administrative_ceiling.has_value()
            ? Privacy::NetworkPolicy::intersect(*m_configuration.administrative_ceiling, *input.administrative_ceiling)
            : *m_configuration.administrative_ceiling;
    }

    auto next_epoch = allocate_epoch();
    auto compiled = Privacy::PolicyCompiler::compile(session.id(), next_epoch, bounded_input);
    if (compiled.is_error()) {
        // Retain the last accepted policy on malformed reload and report the rejection.
        return {
            .committed = false,
            .epoch = state.committed_snapshot.epoch,
            .reason = compiled.error(),
            .diagnostics = { ByteString::formatted("Rejected privacy policy update: {}", Privacy::policy_reason_to_string(compiled.error())) },
        };
    }

    auto result = compiled.release_value();
    auto const& target_snapshot = result.snapshot;
    bool is_tightening = target_snapshot.is_tightening_relative_to(state.committed_snapshot);

    if (is_tightening) {
        // Phase 1: Pause Hub-side admissions immediately before notifying helpers.
        state.admission_snapshot = target_snapshot;

        // Cancel Hub-initiated background work for the default session when tightening.
        if (&session == &Application::default_session())
            Application::the().cancel_background_networking_for_privacy_policy();

        // Retire any live WebRTCClient helpers in this session whose permissions exceed the new policy.
        if (!target_snapshot.network.allows_webrtc_network())
            Application::the().retire_unrestricted_webrtc_helpers_for_session(session);
    }

    // Phase 2: Synchronously install the new policy on every RequestServer instance of this session
    // and verify acknowledgements. Any helper that fails to acknowledge is forcefully terminated.
    Vector<NonnullRefPtr<RequestServerInstance>> instances;
    RequestServerManager::the().for_each_instance([&](RequestServerInstance& instance) {
        if (&instance.session() == &session)
            instances.append(instance);
        return IterationDecision::Continue;
    });

    bool all_acknowledged = true;
    for (auto& instance : instances) {
        auto response = send_sync_to_helper<Messages::RequestServerControl::InstallPrivacyPolicy>(instance->control_client(), target_snapshot);
        if (!response || !response->ack().accepted || response->ack().committed_epoch != target_snapshot.epoch) {
            all_acknowledged = false;
            warnln("RequestServer failed to acknowledge privacy policy epoch {}; terminating helper", target_snapshot.epoch);
            end_unresponsive_helper(instance->control_client());
        }
    }

    // Phase 3: Commit the new policy snapshot.
    state.input = bounded_input;
    state.committed_snapshot = target_snapshot;
    state.admission_snapshot = target_snapshot;
    state.last_diagnostics = result.diagnostics;

    // If relaxing from Offline -> Allow, restart any RequestServer instance that was launched
    // with an OS-level --disable-network sandbox so it regains a network-capable sandbox.
    if (target_snapshot.network.allows_network_egress())
        RequestServerManager::the().restart_network_sandboxed_instances_for_session(session);

    if (on_policy_committed)
        on_policy_committed(session, state.committed_snapshot);

    return {
        .committed = all_acknowledged,
        .epoch = state.committed_snapshot.epoch,
        .reason = all_acknowledged ? Privacy::PolicyReason::None : Privacy::PolicyReason::NetworkDenied,
        .diagnostics = move(result.diagnostics),
    };
}

}
