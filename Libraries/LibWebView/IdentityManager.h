/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibDatabase/Database.h>
#include <LibPrivacy/Identity/NavigationHandoff.h>
#include <LibPrivacy/Identity/Types.h>
#include <LibURL/URL.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>

namespace WebView {

// Hub-owned registry of named browsing identities and domain-assignment handoff rules.
// Every named identity owns its own isolated BrowsingSession (CookieJar, StorageJar,
// PermissionManager, BlobURLStore, HSTSStore, RequestServer instances, and WebContent processes).
class WEBVIEW_API IdentityManager {
    AK_MAKE_NONCOPYABLE(IdentityManager);

public:
    AK_ALLOC_WITH_KMALLOC;

    static IdentityManager& the();

    IdentityManager() = default;
    ~IdentityManager() = default;

    void register_default_session(NonnullRefPtr<BrowsingSession>, RefPtr<Database::Database> browsing_database);

    ErrorOr<NonnullRefPtr<BrowsingSession>> create_identity(Privacy::IdentityDescriptor);
    RefPtr<BrowsingSession> find_session(StringView identity_id) const;
    Vector<Privacy::IdentityDescriptor> identities() const;
    void for_each_named_session(Function<void(BrowsingSession&)> const&) const;

    // Permanently destroys a non-default identity, terminating its workers and retiring its RequestServers.
    bool destroy_identity(StringView identity_id);

    void add_domain_rule(Privacy::DomainAssignmentRule);
    void remove_domain_rules_for_identity(StringView identity_id);
    void clear_domain_rules();
    Vector<Privacy::DomainAssignmentRule> const& domain_rules() const { return m_domain_rules; }

    Optional<Privacy::DomainAssignmentRule> matching_domain_rule(URL::URL const&) const;

    // Evaluates whether a top-level navigation from source_session to destination_url must be handed off
    // to another identity BEFORE any DNS resolution or network dispatch in source_session.
    Privacy::NavigationHandoffDecision evaluate_navigation_handoff(
        BrowsingSession const& source_session,
        URL::URL const& destination_url,
        StringView http_method,
        bool has_post_body,
        bool has_opener,
        bool has_referrer,
        bool has_credentials,
        bool user_confirmed = false) const;

private:
    RefPtr<Database::Database> m_browsing_database;
    HashMap<String, NonnullRefPtr<BrowsingSession>> m_sessions;
    Vector<Privacy::DomainAssignmentRule> m_domain_rules;
};

}
