/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibPrivacy/Export.h>
#include <LibPrivacy/Policy/Types.h>
#include <LibURL/Forward.h>

namespace Privacy {

struct PRIVACY_API EvaluationResult {
    PolicyDecision decision { PolicyDecision::Deny };
    PolicyReason reason { PolicyReason::UninitializedClosed };
    u64 epoch { 0 };

    bool is_allowed() const { return decision == PolicyDecision::Allow; }
    bool is_denied() const { return decision != PolicyDecision::Allow; }

    bool operator==(EvaluationResult const&) const = default;
};

struct PRIVACY_API InstallOutcome {
    PolicyInstallAck ack;
    // True when installing the snapshot narrowed permissions and the owner must quiesce
    // existing requests, DNS lookups, WebSockets, and pooled connections.
    bool requires_quiesce { false };
};

// Default-closed enforcement state machine owned by network-capable helpers (e.g. RequestServer)
// and Hub-side network boundaries (e.g. delegated system DNS).
class PRIVACY_API NetworkGate {
public:
    enum class State : u8 {
        Uninitialized,
        Active,
        CoordinatorLost,
    };

    NetworkGate() = default;

    State state() const { return m_state; }
    u64 committed_epoch() const { return m_committed_epoch; }
    Optional<u64> bound_session_id() const { return m_bound_session_id; }
    Optional<PolicySnapshot> const& current_snapshot() const { return m_current_snapshot; }

    // Installs a new snapshot if it matches the schema version, bound session, and strictly increases epoch.
    InstallOutcome install(PolicySnapshot const&);

    // Permanently closes admissions when the coordinator IPC channel disconnects.
    void mark_coordinator_lost();

    // Used at asynchronous completion boundaries to reject stale in-flight continuations.
    bool is_epoch_current(u64 captured_epoch) const;

    EvaluationResult evaluate_http_request(URL::URL const&, bool is_revalidation, bool is_speculative) const;
    EvaluationResult evaluate_fresh_cache_read() const;
    EvaluationResult evaluate_dns_lookup(StringView host, bool is_system_resolver) const;
    EvaluationResult evaluate_websocket(URL::URL const&) const;
    EvaluationResult evaluate_aia_fetch(URL::URL const&) const;
    EvaluationResult evaluate_webrtc_network() const;
    EvaluationResult evaluate_background_service(StringView service_name) const;

private:
    Optional<EvaluationResult> check_ready() const;
    PolicyReason denial_reason_for_current_policy() const;

    State m_state { State::Uninitialized };
    Optional<u64> m_bound_session_id;
    u64 m_committed_epoch { 0 };
    Optional<PolicySnapshot> m_current_snapshot;
};

}
