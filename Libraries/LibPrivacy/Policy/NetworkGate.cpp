/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPrivacy/Policy/NetworkGate.h>
#include <LibURL/URL.h>

namespace Privacy {

InstallOutcome NetworkGate::install(PolicySnapshot const& snapshot)
{
    if (m_state == State::CoordinatorLost) {
        return {
            .ack = { .accepted = false, .committed_epoch = m_committed_epoch, .rejection_reason = PolicyReason::CoordinatorLost },
            .requires_quiesce = false,
        };
    }

    if (snapshot.schema_version != CURRENT_POLICY_SCHEMA_VERSION) {
        return {
            .ack = { .accepted = false, .committed_epoch = m_committed_epoch, .rejection_reason = PolicyReason::UnsupportedSchemaVersion },
            .requires_quiesce = false,
        };
    }

    if (snapshot.session_id == 0 || snapshot.epoch == 0 || !snapshot.network.is_consistent()) {
        return {
            .ack = { .accepted = false, .committed_epoch = m_committed_epoch, .rejection_reason = PolicyReason::InvalidPolicy },
            .requires_quiesce = false,
        };
    }

    if (m_bound_session_id.has_value() && *m_bound_session_id != snapshot.session_id) {
        return {
            .ack = { .accepted = false, .committed_epoch = m_committed_epoch, .rejection_reason = PolicyReason::SessionMismatch },
            .requires_quiesce = false,
        };
    }

    if (snapshot.epoch <= m_committed_epoch) {
        return {
            .ack = { .accepted = false, .committed_epoch = m_committed_epoch, .rejection_reason = PolicyReason::StaleEpoch },
            .requires_quiesce = false,
        };
    }

    bool requires_quiesce = false;
    if (m_current_snapshot.has_value())
        requires_quiesce = snapshot.is_tightening_relative_to(*m_current_snapshot);
    else if (!snapshot.network.allows_network_egress())
        requires_quiesce = true;

    m_bound_session_id = snapshot.session_id;
    m_committed_epoch = snapshot.epoch;
    m_current_snapshot = snapshot;
    m_state = State::Active;

    return {
        .ack = { .accepted = true, .committed_epoch = m_committed_epoch, .rejection_reason = PolicyReason::None },
        .requires_quiesce = requires_quiesce,
    };
}

void NetworkGate::mark_coordinator_lost()
{
    m_state = State::CoordinatorLost;
    m_current_snapshot.clear();
}

Optional<ProxyRoute const&> NetworkGate::mandatory_route() const
{
    if (m_state != State::Active || !m_current_snapshot.has_value() || !m_current_snapshot->network.route.has_value())
        return {};
    return *m_current_snapshot->network.route;
}

bool NetworkGate::is_epoch_current(u64 captured_epoch) const
{
    return m_state == State::Active
        && m_current_snapshot.has_value()
        && captured_epoch != 0
        && captured_epoch == m_committed_epoch;
}

Optional<EvaluationResult> NetworkGate::check_ready() const
{
    switch (m_state) {
    case State::Uninitialized:
        return EvaluationResult { .decision = PolicyDecision::Deny, .reason = PolicyReason::UninitializedClosed, .epoch = m_committed_epoch };
    case State::CoordinatorLost:
        return EvaluationResult { .decision = PolicyDecision::Deny, .reason = PolicyReason::CoordinatorLost, .epoch = m_committed_epoch };
    case State::Active:
        if (!m_current_snapshot.has_value() || !m_current_snapshot->is_valid())
            return EvaluationResult { .decision = PolicyDecision::Deny, .reason = PolicyReason::InvalidPolicy, .epoch = m_committed_epoch };
        return {};
    }
    return EvaluationResult { .decision = PolicyDecision::Deny, .reason = PolicyReason::InvalidPolicy, .epoch = m_committed_epoch };
}

PolicyReason NetworkGate::denial_reason_for_current_policy() const
{
    if (!m_current_snapshot.has_value())
        return PolicyReason::UninitializedClosed;
    if (m_current_snapshot->network.mode == NetworkMode::Offline)
        return PolicyReason::OfflineMode;
    if (m_current_snapshot->network.mode == NetworkMode::ProxyOnly)
        return PolicyReason::MandatoryRouteUnavailable;
    return PolicyReason::NetworkDenied;
}

EvaluationResult NetworkGate::evaluate_http_request(URL::URL const& url, bool is_revalidation, bool is_speculative) const
{
    if (auto blocked = check_ready(); blocked.has_value())
        return *blocked;

    auto const& policy = m_current_snapshot->network;
    if (!url.scheme().is_one_of("http"sv, "https"sv))
        return { .decision = PolicyDecision::Deny, .reason = PolicyReason::NetworkDenied, .epoch = m_committed_epoch };

    if (!policy.allows_network_egress())
        return { .decision = PolicyDecision::Deny, .reason = denial_reason_for_current_policy(), .epoch = m_committed_epoch };

    if (is_revalidation && !policy.allows_network_revalidation())
        return { .decision = PolicyDecision::Deny, .reason = denial_reason_for_current_policy(), .epoch = m_committed_epoch };

    if (is_speculative && !policy.allow_speculative_connections)
        return { .decision = PolicyDecision::Deny, .reason = denial_reason_for_current_policy(), .epoch = m_committed_epoch };

    return { .decision = PolicyDecision::Allow, .reason = PolicyReason::None, .epoch = m_committed_epoch };
}

EvaluationResult NetworkGate::evaluate_fresh_cache_read() const
{
    if (auto blocked = check_ready(); blocked.has_value())
        return *blocked;

    auto const& policy = m_current_snapshot->network;
    if (!policy.allows_fresh_cache_without_revalidation())
        return { .decision = PolicyDecision::Deny, .reason = denial_reason_for_current_policy(), .epoch = m_committed_epoch };

    return { .decision = PolicyDecision::Allow, .reason = PolicyReason::None, .epoch = m_committed_epoch };
}

EvaluationResult NetworkGate::evaluate_dns_lookup(StringView host, bool) const
{
    if (auto blocked = check_ready(); blocked.has_value())
        return *blocked;

    if (host.is_empty())
        return { .decision = PolicyDecision::Deny, .reason = PolicyReason::NetworkDenied, .epoch = m_committed_epoch };

    auto const& policy = m_current_snapshot->network;
    if (!policy.allows_dns_resolution())
        return { .decision = PolicyDecision::Deny, .reason = denial_reason_for_current_policy(), .epoch = m_committed_epoch };

    return { .decision = PolicyDecision::Allow, .reason = PolicyReason::None, .epoch = m_committed_epoch };
}

EvaluationResult NetworkGate::evaluate_websocket(URL::URL const& url) const
{
    if (auto blocked = check_ready(); blocked.has_value())
        return *blocked;

    if (!url.scheme().is_one_of("ws"sv, "wss"sv))
        return { .decision = PolicyDecision::Deny, .reason = PolicyReason::NetworkDenied, .epoch = m_committed_epoch };

    auto const& policy = m_current_snapshot->network;
    if (!policy.allows_network_egress() || !policy.allow_websockets)
        return { .decision = PolicyDecision::Deny, .reason = denial_reason_for_current_policy(), .epoch = m_committed_epoch };

    return { .decision = PolicyDecision::Allow, .reason = PolicyReason::None, .epoch = m_committed_epoch };
}

EvaluationResult NetworkGate::evaluate_aia_fetch(URL::URL const& url) const
{
    if (auto blocked = check_ready(); blocked.has_value())
        return *blocked;

    if (!url.scheme().is_one_of("http"sv, "https"sv))
        return { .decision = PolicyDecision::Deny, .reason = PolicyReason::NetworkDenied, .epoch = m_committed_epoch };

    auto const& policy = m_current_snapshot->network;
    if (!policy.allows_network_egress() || !policy.allow_aia_fetches)
        return { .decision = PolicyDecision::Deny, .reason = denial_reason_for_current_policy(), .epoch = m_committed_epoch };

    return { .decision = PolicyDecision::Allow, .reason = PolicyReason::None, .epoch = m_committed_epoch };
}

EvaluationResult NetworkGate::evaluate_webrtc_network() const
{
    if (auto blocked = check_ready(); blocked.has_value())
        return *blocked;

    auto const& policy = m_current_snapshot->network;
    if (!policy.allows_webrtc_network())
        return { .decision = PolicyDecision::Deny, .reason = denial_reason_for_current_policy(), .epoch = m_committed_epoch };

    return { .decision = PolicyDecision::Allow, .reason = PolicyReason::None, .epoch = m_committed_epoch };
}

EvaluationResult NetworkGate::evaluate_background_service(StringView) const
{
    if (auto blocked = check_ready(); blocked.has_value())
        return *blocked;

    auto const& policy = m_current_snapshot->network;
    if (!policy.allows_network_egress() || !policy.allow_background_services)
        return { .decision = PolicyDecision::Deny, .reason = denial_reason_for_current_policy(), .epoch = m_committed_epoch };

    return { .decision = PolicyDecision::Allow, .reason = PolicyReason::None, .epoch = m_committed_epoch };
}

}
