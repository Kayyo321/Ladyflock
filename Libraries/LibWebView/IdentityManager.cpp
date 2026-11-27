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
#include <LibWebView/PrivacyActivity.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/StorageJar.h>
#include <LibWebView/ViewImplementation.h>
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

    m_purge_journal = make<IdentityPurgeJournal>(LexicalPath::join(Application::the().profile().paths().data, "IdentityPurges.json"sv).string());
    m_purge_journal->load();
}

ErrorOr<NonnullRefPtr<BrowsingSession>> IdentityManager::create_identity(Privacy::IdentityDescriptor descriptor)
{
    if (!Privacy::is_valid_identity_id(descriptor.id))
        return Error::from_string_literal("Invalid identity ID");

    if (descriptor.id == Privacy::DEFAULT_IDENTITY_ID || descriptor.id == Privacy::PRIVATE_IDENTITY_ID)
        return Error::from_string_literal("Identity ID is reserved");

    if (m_sessions.contains(descriptor.id))
        return Error::from_string_literal("Identity ID already exists");

    // An identity whose purge did not complete may still have data on disk, which it must not reattach to.
    if (!purge_journal_is_readable())
        return Error::from_string_literal("The identity purge journal cannot be read");
    if (has_pending_purge(descriptor.id))
        return Error::from_string_literal("Identity is being purged");

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
    Application::privacy_activity().remove_session(session->id());
    remove_domain_rules_for_identity(identity_id);
    return true;
}

bool IdentityManager::has_pending_purge(StringView identity_id) const
{
    return m_purge_journal && m_purge_journal->contains(identity_id);
}

bool IdentityManager::purge_journal_is_readable() const
{
    return !m_purge_journal || m_purge_journal->is_readable();
}

IdentityPurgeLocations IdentityManager::purge_locations() const
{
    auto const& paths = Application::the().profile().paths();
    return {
        .browsing_database_directory = paths.data,
        .configuration_root = paths.config,
        .cache_root = Application::request_server_options().cache_path,
        .history_database_directory = paths.data,
    };
}

ErrorOr<IdentityPurgeReport> IdentityManager::purge_identity(StringView identity_id)
{
    if (!Privacy::is_valid_identity_id(identity_id))
        return Error::from_string_literal("Invalid identity ID");
    if (identity_id == Privacy::DEFAULT_IDENTITY_ID || identity_id == Privacy::PRIVATE_IDENTITY_ID)
        return Error::from_string_literal("Identity ID is reserved");
    if (!m_purge_journal)
        return Error::from_string_literal("Identities are not set up");

    // The purge is on disk before anything is deleted, so a crash part way through finishes at the next start.
    auto id = TRY(String::from_utf8(identity_id));
    TRY(m_purge_journal->add(id));
    return run_purge(id);
}

Vector<IdentityPurgeReport> IdentityManager::resume_pending_purges()
{
    Vector<IdentityPurgeReport> reports;
    if (!m_purge_journal)
        return reports;
    for (auto const& identity_id : Vector { m_purge_journal->pending() })
        reports.append(run_purge(identity_id));
    return reports;
}

IdentityPurgeReport IdentityManager::run_purge(String const& identity_id)
{
    IdentityPurgeReport report { .identity_id = identity_id, .completed_steps = {}, .failures = {} };

    // Stop the identity's traffic first. Its views close, its workers and RequestServers go, and its stores are
    // detached from disk, so nothing it still does writes back what is about to be deleted. Replacing the stores lets
    // the old ones flush first, which the deletion below then removes.
    if (auto session = find_session(identity_id)) {
        ViewImplementation::for_each_view([&](ViewImplementation& view) {
            if (&view.session() == session.ptr())
                view.force_close();
            return IterationDecision::Continue;
        });
        destroy_identity(identity_id);
        Application::the().file_downloader().cancel_downloads_for_identity(identity_id);

        session->cookie_jar = CookieJar::create();
        session->storage_jar = StorageJar::create();
        if (session->permission_manager)
            session->permission_manager->stop_persisting();
    } else {
        remove_domain_rules_for_identity(identity_id);
    }
    report.completed_steps.append(IdentityPurgeStep::StopIdentity);

    purge_identity_data(identity_id, m_browsing_database, purge_locations(), report, m_purge_failure_for_testing);

    if (report.is_complete()) {
        if (m_purge_failure_for_testing == IdentityPurgeStep::ClearJournal)
            report.failures.append({ IdentityPurgeStep::ClearJournal, "Failure injected for testing"_string });
        else if (auto result = m_purge_journal->remove(identity_id); result.is_error())
            report.failures.append({ IdentityPurgeStep::ClearJournal, MUST(String::formatted("{}", result.error())) });
        else
            report.completed_steps.append(IdentityPurgeStep::ClearJournal);
    }

    for (auto const& failure : report.failures)
        warnln("Purging identity {}: {} failed: {}", identity_id, identity_purge_step_name(failure.step), failure.reason);
    return report;
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
