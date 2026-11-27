/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibPrivacy/Policy/Compiler.h>
#include <LibPrivacy/Policy/Types.h>
#include <LibRequests/Forward.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>

namespace WebView {

// Hub-owned policy coordinator. Compiles typed, versioned Privacy::PolicySnapshot values per
// BrowsingSession, tracks monotonically increasing epochs, and installs snapshots onto
// RequestServer and other enforcing subsystems with synchronous acknowledgements.
class WEBVIEW_API PrivacyPolicyCoordinator {
    AK_MAKE_NONCOPYABLE(PrivacyPolicyCoordinator);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct TransitionResult {
        bool committed { false };
        u64 epoch { 0 };
        Privacy::PolicyReason reason { Privacy::PolicyReason::None };
        Vector<ByteString> diagnostics;
    };

    struct Configuration {
        Privacy::NetworkMode default_network_mode { Privacy::NetworkMode::Allow };
        // The mandatory route of the ProxyOnly mode. ProxyOnly without one stays closed.
        Optional<Privacy::ProxyRoute> proxy_route {};
        bool isolate_proxy_streams { false };
        // The route of Tor-routed identities, whose streams are always isolated. Without one they stay closed.
        Optional<Privacy::ProxyRoute> tor_route {};
        // The administrator's managed network ceiling. Every policy any session compiles is held within it.
        Optional<Privacy::NetworkPolicy> administrative_ceiling {};
    };

    explicit PrivacyPolicyCoordinator(Configuration);
    ~PrivacyPolicyCoordinator() = default;

    Privacy::PolicySnapshot const& snapshot_for_session(BrowsingSession const&);

    Privacy::NetworkMode network_mode(BrowsingSession const&) const;
    u64 committed_epoch(BrowsingSession const&) const;

    // Fast Hub-side admission checks. During a tightening transition, these fail closed immediately
    // before asynchronous helper teardown completes.
    bool allows_network_egress(BrowsingSession const&) const;
    bool allows_dns_resolution(BrowsingSession const&) const;
    bool allows_webrtc_network(BrowsingSession const&) const;
    bool allows_background_services(BrowsingSession const&) const;

    // Synchronously installs the session's active policy snapshot onto a newly launched or
    // restarted RequestServer control plane before any client connections are handed out.
    ErrorOr<void> install_policy_on_request_server(BrowsingSession const&, Requests::RequestControlClient&);

    // Live policy transition with quiesce, acknowledgement verification, and helper cleanup.
    // ProxyOnly uses the configured proxy route; without one, the transition is rejected and the current policy stays.
    TransitionResult set_network_mode(BrowsingSession&, Privacy::NetworkMode);

    // Starts a new isolation generation for the session's route: every later stream gets new credentials, so it shares
    // no Tor circuit with an earlier one, and the session's existing connections are closed.
    TransitionResult renew_route_isolation(BrowsingSession&);

    // Installs a custom compiled PolicyInput (e.g., with administrative ceiling or identity requirement).
    TransitionResult apply_policy_input(BrowsingSession&, Privacy::PolicyInput const&);

    void remove_session(u64 session_id) { m_sessions.remove(session_id); }

    Function<void(BrowsingSession const&, Privacy::PolicySnapshot const&)> on_policy_committed;

private:
    struct SessionState {
        // Secret that the session's isolation credentials are derived from. Each identity has its own session, and so
        // its own secret; the secret never leaves Hub and the session's network owners.
        ByteString isolation_secret;
        u64 isolation_generation { 1 };
        Privacy::PolicyInput input;
        Privacy::PolicySnapshot committed_snapshot;
        // Effective snapshot used for immediate Hub-side gating during a tightening transition.
        Privacy::PolicySnapshot admission_snapshot;
        Vector<ByteString> last_diagnostics;
    };

    SessionState& ensure_session_state(BrowsingSession const&);
    SessionState const* find_session_state(BrowsingSession const&) const;
    // The policy a session would start with, for admission checks before its state exists.
    Privacy::NetworkPolicy preview_policy(BrowsingSession const&) const;
    Privacy::PolicyInput default_input_for_session(BrowsingSession const&, ByteString const& isolation_secret, u64 isolation_generation) const;
    static Privacy::ProxyRoute route_for_session(Privacy::ProxyRoute route, bool isolate, ByteString const& isolation_secret, u64 isolation_generation);
    Privacy::NetworkPolicy preference_for_mode(Privacy::NetworkMode, ByteString const& isolation_secret, u64 isolation_generation) const;
    Optional<Privacy::NetworkPolicy> identity_requirement_for_session(BrowsingSession const&, ByteString const& isolation_secret, u64 isolation_generation) const;
    u64 allocate_epoch() { return m_next_epoch++; }

    Configuration m_configuration;
    u64 m_next_epoch { 1 };
    HashMap<u64, SessionState> m_sessions;
};

}
