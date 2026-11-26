/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibWebView/Application.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/IdentityManager.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/StorageJar.h>
#include <LibWebView/WorkerAgentManager.h>

namespace WebView {

IdentityManager& IdentityManager::the()
{
    return Application::identity_manager();
}

void IdentityManager::register_default_session(NonnullRefPtr<BrowsingSession> default_session, RefPtr<Database::Database> browsing_database)
{
    m_browsing_database = move(browsing_database);
    m_sessions.set(default_session->identity_id(), move(default_session));
}

ErrorOr<NonnullRefPtr<BrowsingSession>> IdentityManager::create_identity(Privacy::IdentityDescriptor descriptor)
{
    if (!Privacy::is_valid_identity_id(descriptor.id))
        return Error::from_string_literal("Invalid identity ID");

    if (descriptor.id == Privacy::DEFAULT_IDENTITY_ID || descriptor.id == Privacy::PRIVATE_IDENTITY_ID)
        return Error::from_string_literal("Identity ID is reserved");

    if (m_sessions.contains(descriptor.id))
        return Error::from_string_literal("Identity ID already exists");

    auto session = BrowsingSession::create_for_identity(descriptor);

    if (!session->is_ephemeral()) {
        if (m_browsing_database) {
            session->cookie_jar = TRY(CookieJar::create(*m_browsing_database, descriptor.id));
            session->storage_jar = TRY(StorageJar::create(*m_browsing_database, descriptor.id));
        } else {
            session->cookie_jar = CookieJar::create();
            session->storage_jar = StorageJar::create();
        }
        // Keep HSTS isolated per identity in-memory until identity-scoped HSTS persistence is added.
        session->hsts_store = HSTSStore::create();
        session->blob_url_store = make<BlobURLStore>();

        auto permissions_directory = LexicalPath::join(
            Application::the().profile().paths().config,
            "Identities"sv,
            descriptor.id.to_byte_string());
        (void)Core::Directory::create(permissions_directory, Core::Directory::CreateDirectories::Yes);
        auto permissions_path = LexicalPath::join(permissions_directory.string(), "Permissions.json"sv).string();
        session->permission_manager = PermissionManager::create_for_ordinary_session(move(permissions_path));
    } else {
        auto* inherit_from = descriptor.inherit_ordinary_permission_blocks
            ? Application::default_session().permission_manager.ptr()
            : nullptr;
        session->permission_manager = PermissionManager::create_ephemeral(inherit_from);
    }

    Application::the().register_permission_manager(*session->permission_manager);

    // Ensure the session's initial privacy policy snapshot is compiled immediately.
    (void)Application::privacy_policy_coordinator().snapshot_for_session(*session);

    m_sessions.set(descriptor.id, session);
    return session;
}

RefPtr<BrowsingSession> IdentityManager::find_session(StringView identity_id) const
{
    if (auto it = m_sessions.find(identity_id); it != m_sessions.end())
        return it->value;
    return nullptr;
}

Vector<Privacy::IdentityDescriptor> IdentityManager::identities() const
{
    Vector<Privacy::IdentityDescriptor> result;
    result.ensure_capacity(m_sessions.size());
    for (auto const& [_, session] : m_sessions)
        result.unchecked_append(session->identity());
    return result;
}

void IdentityManager::for_each_named_session(Function<void(BrowsingSession&)> const& callback) const
{
    for (auto const& [id, session] : m_sessions) {
        if (id == Privacy::DEFAULT_IDENTITY_ID)
            continue;
        callback(*session);
    }
}

bool IdentityManager::destroy_identity(StringView identity_id)
{
    if (identity_id == Privacy::DEFAULT_IDENTITY_ID)
        return false;

    auto maybe_session = m_sessions.take(identity_id);
    if (!maybe_session.has_value())
        return false;

    auto session = maybe_session.release_value();
    WorkerAgentManager::the().remove_workers_for_session(session->id());
    RequestServerManager::the().retire_instances_for_session(*session);
    Application::privacy_policy_coordinator().remove_session(session->id());
    remove_domain_rules_for_identity(identity_id);
    return true;
}

void IdentityManager::add_domain_rule(Privacy::DomainAssignmentRule rule)
{
    m_domain_rules.remove_all_matching([&](auto const& existing) {
        return existing.pattern.equals_ignoring_ascii_case(rule.pattern);
    });
    m_domain_rules.append(move(rule));
}

void IdentityManager::remove_domain_rules_for_identity(StringView identity_id)
{
    m_domain_rules.remove_all_matching([&](auto const& rule) {
        return rule.target_identity_id == identity_id;
    });
}

void IdentityManager::clear_domain_rules()
{
    m_domain_rules.clear();
}

Optional<Privacy::DomainAssignmentRule> IdentityManager::matching_domain_rule(URL::URL const& url) const
{
    if (url.scheme() != "http"sv && url.scheme() != "https"sv)
        return {};

    auto host_view = url.serialized_host();
    if (host_view.is_empty())
        return {};

    Optional<Privacy::DomainAssignmentRule> best_match;
    size_t best_specificity = 0;
    for (auto const& rule : m_domain_rules) {
        if (!rule.matches_host(host_view))
            continue;

        // Exact matches take precedence over wildcard matches; longer suffixes win among wildcards.
        auto pattern_view = rule.pattern.bytes_as_string_view();
        bool is_wildcard = pattern_view.starts_with("*."sv);
        size_t specificity = (is_wildcard ? 0u : 10000u) + pattern_view.length();
        if (!best_match.has_value() || specificity > best_specificity) {
            best_match = rule;
            best_specificity = specificity;
        }
    }

    return best_match;
}

Privacy::NavigationHandoffDecision IdentityManager::evaluate_navigation_handoff(
    BrowsingSession const& source_session,
    URL::URL const& destination_url,
    StringView http_method,
    bool has_post_body,
    bool has_opener,
    bool has_referrer,
    bool has_credentials,
    bool user_confirmed) const
{
    auto stay = [&] {
        Privacy::NavigationHandoffDecision decision;
        decision.action = Privacy::HandoffAction::StayInCurrentIdentity;
        decision.target_identity_id = source_session.identity_id();
        return decision;
    };

    auto rule = matching_domain_rule(destination_url);
    if (!rule.has_value())
        return stay();

    auto target_session = find_session(rule->target_identity_id);
    if (!target_session)
        return stay();

    auto& coordinator = Application::privacy_policy_coordinator();
    auto source_snapshot = coordinator.snapshot_for_session(source_session);
    auto target_snapshot = coordinator.snapshot_for_session(*target_session);

    auto source_descriptor = source_session.identity();
    source_descriptor.network_mode = source_snapshot.network.mode;

    auto target_descriptor = target_session->identity();
    target_descriptor.network_mode = target_snapshot.network.mode;

    Privacy::NavigationHandoffRequest request {
        .source_identity = move(source_descriptor),
        .source_policy = source_snapshot.network,
        .target_identity = move(target_descriptor),
        .target_policy = target_snapshot.network,
        .destination_url = destination_url,
        .http_method = ByteString(http_method),
        .has_post_body = has_post_body,
        .has_opener = has_opener,
        .has_referrer = has_referrer,
        .has_credentials = has_credentials,
        .user_confirmed = user_confirmed,
    };
    return Privacy::NavigationHandoffEvaluator::evaluate(request);
}

}
