/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/RequestControlClient.h>
#include <LibWebView/Application.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

PrivacyPolicyCoordinator::PrivacyPolicyCoordinator(Privacy::NetworkMode default_network_mode)
    : m_default_network_mode(default_network_mode)
{
}

Privacy::NetworkMode PrivacyPolicyCoordinator::initial_network_mode_for_session(BrowsingSession const& session) const
{
    if (m_default_network_mode == Privacy::NetworkMode::Offline || session.identity().network_mode == Privacy::NetworkMode::Offline)
        return Privacy::NetworkMode::Offline;
    return Privacy::NetworkMode::Allow;
}

Privacy::PolicyInput PrivacyPolicyCoordinator::default_input_for_session(BrowsingSession const& session) const
{
    auto initial_mode = initial_network_mode_for_session(session);
    auto policy = initial_mode == Privacy::NetworkMode::Offline
        ? Privacy::NetworkPolicy::offline()
        : Privacy::NetworkPolicy::allow_default();
    bool has_configured_proxy = !Application::request_server_options().proxy_configuration.is_empty()
        || Application::web_content_options().proxy_autoconfig_url.has_value();
    return {
        .schema_version = Privacy::CURRENT_POLICY_SCHEMA_VERSION,
        .user_preference = policy,
        .has_configured_proxy = has_configured_proxy,
    };
}

PrivacyPolicyCoordinator::SessionState& PrivacyPolicyCoordinator::ensure_session_state(BrowsingSession const& session)
{
    return m_sessions.ensure(session.id(), [&] {
        auto input = default_input_for_session(session);
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
                .input = input,
                .committed_snapshot = closed,
                .admission_snapshot = closed,
                .last_diagnostics = { "Initial privacy policy rejected; defaulting closed."sv },
            };
        }
        auto result = compiled.release_value();
        return SessionState {
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
    return initial_network_mode_for_session(session);
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
    return initial_network_mode_for_session(session) == Privacy::NetworkMode::Allow;
}

bool PrivacyPolicyCoordinator::allows_dns_resolution(BrowsingSession const& session) const
{
    if (auto const* state = find_session_state(session))
        return state->admission_snapshot.network.allows_dns_resolution();
    return initial_network_mode_for_session(session) == Privacy::NetworkMode::Allow;
}

bool PrivacyPolicyCoordinator::allows_webrtc_network(BrowsingSession const& session) const
{
    if (auto const* state = find_session_state(session))
        return state->admission_snapshot.network.allows_webrtc_network();
    return initial_network_mode_for_session(session) == Privacy::NetworkMode::Allow;
}

bool PrivacyPolicyCoordinator::allows_background_services(BrowsingSession const& session) const
{
    if (auto const* state = find_session_state(session))
        return state->admission_snapshot.network.allow_background_services;
    return initial_network_mode_for_session(session) == Privacy::NetworkMode::Allow;
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
    auto next_input = state.input;
    next_input.user_preference = mode == Privacy::NetworkMode::Offline
        ? Privacy::NetworkPolicy::offline()
        : Privacy::NetworkPolicy::allow_default();
    next_input.has_configured_proxy = !Application::request_server_options().proxy_configuration.is_empty()
        || Application::web_content_options().proxy_autoconfig_url.has_value();
    return apply_policy_input(session, next_input);
}

PrivacyPolicyCoordinator::TransitionResult PrivacyPolicyCoordinator::apply_policy_input(BrowsingSession& session, Privacy::PolicyInput const& input)
{
    auto& state = ensure_session_state(session);
    auto next_epoch = allocate_epoch();
    auto compiled = Privacy::PolicyCompiler::compile(session.id(), next_epoch, input);
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
    state.input = input;
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
