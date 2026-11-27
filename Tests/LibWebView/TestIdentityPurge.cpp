/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include "PrivacyTestFixtures.h"
#include <AK/LexicalPath.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibDatabase/Database.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibPrivacy/Identity/Types.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/IdentityManager.h>
#include <LibWebView/IdentityPurge.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/Utilities.h>

// Purging an identity, observed on disk: each marker is looked for in the raw bytes of the browser's files, including
// SQLite's write-ahead logs, rather than through the stores that wrote it.

namespace {

class PersistentApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(PersistentApplication)

public:
    explicit PersistentApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions& request_server_options, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::No;
        request_server_options.http_disk_cache_mode = WebView::HTTPDiskCacheMode::Enabled;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }
};

ByteString request_target(ByteString const& request_head)
{
    auto line_end = request_head.find("\r\n"sv).value_or(request_head.length());
    auto parts = request_head.substring_view(0, line_end).split_view(' ');
    return parts.size() >= 2 ? ByteString { parts[1] } : ByteString {};
}

ByteString http_response(StringView content_type, StringView body, StringView extra_headers = ""sv)
{
    return ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\n{}Connection: close\r\n\r\n{}",
        content_type, body.length(), extra_headers, body);
}

// /write?ck=<cookie marker>&ls=<storage marker> stores both and fetches a cacheable resource; /read reports them.
ByteString handle(ByteString const& request_head)
{
    auto target = request_target(request_head);
    if (target.starts_with("/write?"sv)) {
        return http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html><script>
const query = new URLSearchParams(location.search);
document.cookie = 'ck=' + query.get('ck') + '; path=/; max-age=86400';
localStorage.setItem('ls', query.get('ls'));
fetch('/cached?' + query.get('ls')).then(response => response.text()).then(() => { document.title = 'RES:WROTE'; });
</script>)HTML"sv);
    }
    if (target == "/read"sv) {
        return http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html><script>
document.title = 'RES:READ:' + localStorage.getItem('ls') + '|' + document.cookie;
</script>)HTML"sv);
    }
    if (target.starts_with("/cached?"sv))
        return http_response("text/plain"sv, "cached"sv, "Cache-Control: public, max-age=3600\r\n"sv);
    return http_response("text/plain"sv, "ok"sv);
}

Core::AnonymousBuffer load_test_theme()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string();
    return MUST(Gfx::load_system_theme(theme_path));
}

void spin_for(AK::Duration duration)
{
    bool done = false;
    auto timer = Core::Timer::create_single_shot(duration, [&] { done = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return done; });
}

ByteString wait_for_title(WebView::HeadlessWebView& view, StringView prefix)
{
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(20), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return timed_out || view.title().to_byte_string().starts_with(prefix); });
    if (timed_out)
        warnln("Timed out waiting for '{}' (title '{}')", prefix, view.title());
    VERIFY(!timed_out);
    return view.title().to_byte_string();
}

URL::URL url(u16 port, StringView path)
{
    return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}{}", port, path)).release_value();
}

// Every file of a database, including its write-ahead log and shared memory index.
bool database_files_contain(ByteString const& directory, StringView database_name, StringView marker)
{
    for (auto suffix : { ""sv, "-wal"sv, "-shm"sv, "-journal"sv }) {
        auto path = ByteString::formatted("{}/{}.db{}", directory, database_name, suffix);
        if (!FileSystem::exists(path))
            continue;
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
        auto bytes = MUST(file->read_until_eof());
        if (StringView { bytes }.contains(marker))
            return true;
    }
    return false;
}

// Every file these tests write and delete must be in the test's own directory, never in the user's profile.
void verify_profile_is_inside(Test::HeadlessTestEnvironment const& environment)
{
    auto const& paths = WebView::Application::the().profile().paths();
    for (auto const& path : { paths.config, paths.data, paths.cache, WebView::Application::request_server_options().cache_path }) {
        if (!path.starts_with(environment.directory())) {
            warnln("Refusing to run: the profile path {} is outside the test directory {}", path, environment.directory());
            VERIFY_NOT_REACHED();
        }
    }
}

// Every file under a directory, whatever store wrote it: databases, logs, caches, settings and anything else.
bool any_file_contains(ByteString const& root, StringView marker)
{
    Core::DirIterator iterator { root, Core::DirIterator::Flags::SkipParentAndBaseDir };
    while (iterator.has_next()) {
        auto path = iterator.next_full_path();
        auto stat = Core::System::lstat(path);
        if (stat.is_error())
            continue;
        if (S_ISDIR(stat.value().st_mode)) {
            if (any_file_contains(path, marker))
                return true;
            continue;
        }
        if (!S_ISREG(stat.value().st_mode))
            continue;
        auto file = Core::File::open(path, Core::File::OpenMode::Read);
        if (file.is_error())
            continue;
        auto bytes = file.value()->read_until_eof();
        if (!bytes.is_error() && StringView { bytes.value() }.contains(marker))
            return true;
    }
    return false;
}

ByteString identity_directory(ByteString const& root, StringView identity_id)
{
    return LexicalPath::join(root, "Identities"sv, identity_id).string();
}

struct Markers {
    ByteString cookie;
    ByteString storage;
};

Markers make_markers(StringView label)
{
    auto id = generate_random_uuid();
    return { ByteString::formatted("ck-{}-{}", label, id), ByteString::formatted("ls-{}-{}", label, id) };
}

NonnullOwnPtr<WebView::HeadlessWebView> write_markers(WebView::BrowsingSession& session, u16 port, Markers const& markers)
{
    auto view = WebView::HeadlessWebView::create_for_session(load_test_theme(), { 400, 300 }, session);
    view->load_from_browser_ui(url(port, ByteString::formatted("/write?ck={}&ls={}", markers.cookie, markers.storage)));
    wait_for_title(*view, "RES:WROTE"sv);
    return view;
}

ByteString read_markers(WebView::BrowsingSession& session, u16 port)
{
    auto view = WebView::HeadlessWebView::create_for_session(load_test_theme(), { 400, 300 }, session);
    view->load_from_browser_ui(url(port, "/read"sv));
    return wait_for_title(*view, "RES:READ:"sv);
}

// What the UI process holds: its own connection to the History database, with a store per identity.
struct UIHistory {
    NonnullRefPtr<Database::Database> database;

    static UIHistory open(ByteString const& directory)
    {
        auto database = MUST(Database::Database::create(directory, "History"sv));
        VERIFY(MUST(WebView::FaviconStore::migrate_schema(database)) == Database::MigrationOutcome::Success);
        VERIFY(MUST(WebView::HistoryStore::migrate_schema(database)) == Database::MigrationOutcome::Success);
        return { move(database) };
    }

    void record(StringView identity_id, ByteString const& marker) const
    {
        auto store = MUST(WebView::HistoryStore::create(*database, MUST(String::from_utf8(identity_id))));
        store->record_visit(URL::Parser::basic_parse(ByteString::formatted("https://{}.example/", marker)).release_value(), "Visited"_string, UnixDateTime::now(), WebView::HistoryVisitTransition::Link);
    }

    bool has(StringView identity_id, ByteString const& marker) const
    {
        auto store = MUST(WebView::HistoryStore::create(*database, MUST(String::from_utf8(identity_id))));
        return store->entry_for_url(URL::Parser::basic_parse(ByteString::formatted("https://{}.example/", marker)).release_value()).has_value();
    }
};

void grant_geolocation(WebView::BrowsingSession& session, u16 port)
{
    session.permission_manager->set_decision(Web::Permissions::PermissionName::Geolocation, url(port, "/"sv).origin(), WebView::PermissionDecision::Allow);
}

}

TEST_CASE(purging_an_identity_removes_what_it_stored_and_nothing_else)
{
    PrivacyTest::IndependentHTTPServer server(PrivacyTest::IndependentHTTPServer::Protocol::HTTP, PrivacyTest::IndependentHTTPServer::Family::IPv4, handle);
    Test::HeadlessTestEnvironment environment { "TestIdentityPurge"sv, 120 };
    auto app = TRY_OR_FAIL(environment.create_application<PersistentApplication>());
    verify_profile_is_inside(environment);

    auto const& paths = WebView::Application::the().profile().paths();
    auto cache_root = WebView::Application::request_server_options().cache_path;
    auto& identities = WebView::IdentityManager::the();

    auto personal = TRY_OR_FAIL(identities.create_identity(Privacy::IdentityDescriptor::persistent("personal"_string, "Personal"_string)));
    auto work_markers = make_markers("work"sv);
    auto personal_markers = make_markers("personal"sv);
    auto personal_view = write_markers(*personal, server.port(), personal_markers);
    grant_geolocation(*personal, server.port());
    auto history = UIHistory::open(paths.data);
    history.record("personal"sv, personal_markers.storage);

    {
        auto work = TRY_OR_FAIL(identities.create_identity(Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string)));
        auto work_view = write_markers(*work, server.port(), work_markers);
        grant_geolocation(*work, server.port());
        history.record("work"sv, work_markers.storage);

        // Positive controls: each identity's data is on disk where the purge must find it.
        EXPECT(database_files_contain(paths.data, "Ladybird"sv, work_markers.storage));
        EXPECT(database_files_contain(paths.data, "Ladybird"sv, personal_markers.storage));
        EXPECT(database_files_contain(paths.data, "History"sv, work_markers.storage));
        EXPECT(history.has("work"sv, work_markers.storage));
        EXPECT(FileSystem::exists(LexicalPath::join(identity_directory(paths.config, "work"sv), "Permissions.json"sv).string()));
        EXPECT(FileSystem::exists(identity_directory(cache_root, "work"sv)));
        EXPECT(read_markers(*work, server.port()).contains(work_markers.cookie));
        EXPECT(any_file_contains(environment.directory(), work_markers.storage));

        auto report = TRY_OR_FAIL(identities.purge_identity("work"sv));
        for (auto const& failure : report.failures)
            warnln("unexpected failure: {} {}", WebView::identity_purge_step_name(failure.step), failure.reason);
        EXPECT(report.is_complete());
        EXPECT_EQ(report.completed_steps.size(), 8u);
        EXPECT(!identities.find_session("work"sv));
        EXPECT(!identities.has_pending_purge("work"sv));
    }

    // The purged identity's session and its last page are gone now. Had its stores still been attached to disk, this
    // is when they would have written back what they held, so give them time to, then look.
    spin_for(AK::Duration::from_seconds(2));

    // Cookies are flushed from memory as the purge detaches the identity's cookie jar, so the deletion after it is
    // what removes them.
    EXPECT(!database_files_contain(paths.data, "Ladybird"sv, work_markers.cookie));
    EXPECT(!database_files_contain(paths.data, "Ladybird"sv, work_markers.storage));
    EXPECT(!database_files_contain(paths.data, "History"sv, work_markers.storage));
    EXPECT(!history.has("work"sv, work_markers.storage));
    EXPECT(!FileSystem::exists(identity_directory(paths.config, "work"sv)));
    EXPECT(!FileSystem::exists(identity_directory(cache_root, "work"sv)));
    EXPECT(!FileSystem::exists(LexicalPath::join(paths.data, "IdentityPurges.json"sv).string()));
    // Nor is either marker anywhere else in the profile: not in another store, a cache, a log or a settings file.
    EXPECT(!any_file_contains(environment.directory(), work_markers.cookie));
    EXPECT(!any_file_contains(environment.directory(), work_markers.storage));

    // The other identity keeps everything.
    EXPECT(database_files_contain(paths.data, "Ladybird"sv, personal_markers.storage));
    EXPECT(history.has("personal"sv, personal_markers.storage));
    EXPECT(FileSystem::exists(LexicalPath::join(identity_directory(paths.config, "personal"sv), "Permissions.json"sv).string()));
    auto personal_read = read_markers(*personal, server.port());
    EXPECT(personal_read.contains(personal_markers.storage));
    EXPECT(personal_read.contains(personal_markers.cookie));

    // An identity created again with the same name starts empty.
    auto recreated = TRY_OR_FAIL(identities.create_identity(Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string)));
    EXPECT_EQ(read_markers(*recreated, server.port()), "RES:READ:null|"sv);

    // Reserved and malformed names are refused before anything is journaled.
    EXPECT(identities.purge_identity("default"sv).is_error());
    EXPECT(identities.purge_identity(Privacy::PRIVATE_IDENTITY_ID).is_error());
    EXPECT(identities.purge_identity("../work"sv).is_error());
    EXPECT(!FileSystem::exists(LexicalPath::join(paths.data, "IdentityPurges.json"sv).string()));
}

TEST_CASE(an_interrupted_purge_finishes_at_the_next_start)
{
    PrivacyTest::IndependentHTTPServer server(PrivacyTest::IndependentHTTPServer::Protocol::HTTP, PrivacyTest::IndependentHTTPServer::Family::IPv4, handle);
    Test::HeadlessTestEnvironment environment { "TestIdentityPurgeRecovery"sv, 180 };
    auto work_markers = make_markers("work"sv);
    auto travel_markers = make_markers("travel"sv);
    ByteString data_path;
    ByteString config_path;
    ByteString cache_root;

    {
        auto app = TRY_OR_FAIL(environment.create_application<PersistentApplication>());
        verify_profile_is_inside(environment);
        data_path = WebView::Application::the().profile().paths().data;
        config_path = WebView::Application::the().profile().paths().config;
        cache_root = WebView::Application::request_server_options().cache_path;
        auto& identities = WebView::IdentityManager::the();

        auto work = TRY_OR_FAIL(identities.create_identity(Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string)));
        auto travel = TRY_OR_FAIL(identities.create_identity(Privacy::IdentityDescriptor::persistent("travel"_string, "Travel"_string)));
        auto work_view = write_markers(*work, server.port(), work_markers);
        auto travel_view = write_markers(*travel, server.port(), travel_markers);
        grant_geolocation(*work, server.port());

        // The cookie step fails, so the cookies the detached jar flushed stay on disk: the positive control for them.
        identities.set_purge_failure_for_testing(WebView::IdentityPurgeStep::Cookies);
        auto report = TRY_OR_FAIL(identities.purge_identity("work"sv));
        identities.set_purge_failure_for_testing({});

        EXPECT(!report.is_complete());
        EXPECT_EQ(report.failures.size(), 1u);
        EXPECT_EQ(report.failures.first().step, WebView::IdentityPurgeStep::Cookies);
        EXPECT(database_files_contain(data_path, "Ladybird"sv, work_markers.cookie));
        // The steps after it still ran.
        EXPECT(!database_files_contain(data_path, "Ladybird"sv, work_markers.storage));
        EXPECT(!FileSystem::exists(identity_directory(cache_root, "work"sv)));

        // Until the purge completes, the identity cannot come back and reattach to what is left.
        EXPECT(identities.has_pending_purge("work"sv));
        auto refused = identities.create_identity(Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string));
        EXPECT(refused.is_error());
    }

    // A second identity's purge is journaled, then the browser stops before it deletes anything, as in a crash. It is
    // written as purge_identity writes it, while no browser has the profile open.
    {
        WebView::IdentityPurgeJournal journal { LexicalPath::join(data_path, "IdentityPurges.json"sv).string() };
        journal.load();
        EXPECT(journal.is_readable());
        EXPECT(journal.contains("work"sv));
        TRY_OR_FAIL(journal.add("travel"_string));
    }
    EXPECT(database_files_contain(data_path, "Ladybird"sv, travel_markers.storage));
    EXPECT(FileSystem::exists(identity_directory(cache_root, "travel"sv)));

    {
        // The next start finishes both purges before anything can create the identities again.
        auto app = TRY_OR_FAIL(environment.create_application<PersistentApplication>());
        verify_profile_is_inside(environment);
        auto& identities = WebView::IdentityManager::the();
        EXPECT(!identities.has_pending_purge("work"sv));
        EXPECT(!identities.has_pending_purge("travel"sv));
        EXPECT(!FileSystem::exists(LexicalPath::join(data_path, "IdentityPurges.json"sv).string()));

        EXPECT(!database_files_contain(data_path, "Ladybird"sv, work_markers.cookie));
        EXPECT(!database_files_contain(data_path, "Ladybird"sv, travel_markers.cookie));
        EXPECT(!database_files_contain(data_path, "Ladybird"sv, travel_markers.storage));
        EXPECT(!FileSystem::exists(identity_directory(config_path, "work"sv)));
        EXPECT(!FileSystem::exists(identity_directory(cache_root, "travel"sv)));

        auto work = TRY_OR_FAIL(identities.create_identity(Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string)));
        EXPECT_EQ(read_markers(*work, server.port()), "RES:READ:null|"sv);
    }
}

TEST_CASE(an_unreadable_purge_journal_keeps_named_identities_closed)
{
    Test::HeadlessTestEnvironment environment { "TestIdentityPurgeJournal"sv, 60 };
    ByteString journal_path;
    {
        auto app = TRY_OR_FAIL(environment.create_application<PersistentApplication>());
        verify_profile_is_inside(environment);
        journal_path = LexicalPath::join(WebView::Application::the().profile().paths().data, "IdentityPurges.json"sv).string();
    }

    {
        auto file = TRY_OR_FAIL(Core::File::open(journal_path, Core::File::OpenMode::Write));
        TRY_OR_FAIL(file->write_until_depleted("{\"version\":1,\"pend"sv));
    }

    auto app = TRY_OR_FAIL(environment.create_application<PersistentApplication>());
    verify_profile_is_inside(environment);
    auto& identities = WebView::IdentityManager::the();
    EXPECT(!identities.purge_journal_is_readable());
    EXPECT(identities.create_identity(Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string)).is_error());
    EXPECT(identities.purge_identity("work"sv).is_error());
    // The default identity is never purged, so it keeps working.
    EXPECT(identities.find_session(Privacy::DEFAULT_IDENTITY_ID));
    // The damaged journal is left for repair, not replaced.
    EXPECT(FileSystem::exists(journal_path));
}
