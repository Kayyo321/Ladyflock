/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include "PrivacyTestFixtures.h"
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibDatabase/Database.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibPrivacy/Identity/NavigationHandoff.h>
#include <LibPrivacy/Identity/Types.h>
#include <LibPrivacy/Storage/PartitionKey.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/IdentityManager.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/Settings.h>
#include <LibWebView/StorageJar.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WorkerAgentManager.h>

namespace {

class Milestone2TransientApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(Milestone2TransientApplication)

public:
    explicit Milestone2TransientApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions& request_server_options, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        request_server_options.http_disk_cache_mode = WebView::HTTPDiskCacheMode::Disabled;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }
};

class Milestone2PersistentApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(Milestone2PersistentApplication)

public:
    explicit Milestone2PersistentApplication(Optional<ByteString> ladybird_binary_path)
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

static ByteString make_http_response(StringView content_type, StringView body, StringView extra_headers = ""sv)
{
    return ByteString::formatted(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: {}\r\n"
        "Content-Length: {}\r\n"
        "{}"
        "Connection: close\r\n"
        "\r\n"
        "{}",
        content_type,
        body.length(),
        extra_headers,
        body);
}

static ByteString extract_request_target(ByteString const& request_head)
{
    auto first_line_end = request_head.find("\r\n"sv);
    auto first_line = first_line_end.has_value()
        ? request_head.substring_view(0, *first_line_end)
        : request_head.view();
    auto parts = first_line.split_view(' ');
    if (parts.size() >= 2)
        return parts[1];
    return "/"sv;
}

static void spin_for_ms(i64 ms)
{
    bool done = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(ms), [&] { done = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return done; });
}

static ByteString wait_for_title_prefix(WebView::HeadlessWebView& view, StringView prefix, i64 timeout_seconds = 15)
{
    ByteString matched;
    bool done = false;
    bool timed_out = false;

    auto check = [&](Utf16String const& title) {
        auto utf8 = title.to_byte_string();
        if (utf8.starts_with(prefix)) {
            matched = move(utf8);
            done = true;
        }
    };

    view.on_title_change = [&](Utf16String const& title) { check(title); };
    check(view.title());
    if (done) {
        view.on_title_change = nullptr;
        return matched;
    }

    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(timeout_seconds), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return done || timed_out; });
    timer->stop();
    view.on_title_change = nullptr;

    if (timed_out)
        warnln("Timed out waiting for title prefix '{}' (current title: '{}')", prefix, view.title());
    VERIFY(!timed_out);
    return matched;
}

static void configure_custom_dns(PrivacyTest::IndependentDNSObserver const& dns)
{
    WebView::Application::settings().set_dns_settings(WebView::DNSSettings(WebView::DNSOverUDP {
                                                          .server_address = "127.0.0.1"sv,
                                                          .port = dns.port(),
                                                          .validate_dnssec_locally = false,
                                                      }),
        false);
}

static Core::AnonymousBuffer load_test_theme()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string();
    return MUST(Gfx::load_system_theme(theme_path));
}

}

// 1. Same-origin embed works within its own partition (positive control) and cannot read another
//    top-level site's state (negative control); A -> B -> A cross-site ancestor isolation;
//    and opaque contexts remain excluded from persistent web storage.
TEST_CASE(milestone2_partitioned_embed_positive_negative_and_aba_ancestor)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    u16 server_port = 0;

    PrivacyTest::IndependentHTTPServer http_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [&](ByteString const& request_head) {
            auto target = extract_request_target(request_head);
            if (target == "/embed-write"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><body><script>
localStorage.setItem('embed_key', 'from-site-a');
document.cookie = 'ec=from-site-a; path=/';
window.top.postMessage('WROTE:' + localStorage.getItem('embed_key') + '|' + document.cookie, '*');
</script></body></html>)HTML"sv);
            }
            if (target == "/embed-read"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><body><script>
const v = localStorage.getItem('embed_key');
const c = document.cookie;
window.top.postMessage('READ:' + (v === null ? 'NULL' : v) + '|' + c, '*');
</script></body></html>)HTML"sv);
            }
            if (target == "/host-write-embed"sv) {
                auto html = ByteString::formatted(R"HTML(<!doctype html>
<html><head><title>boot</title></head><body>
<script>
window.addEventListener('message', e => {{ document.title = 'RES:' + e.data; }});
</script>
<iframe src="http://embed.test:{}/embed-write"></iframe>
</body></html>)HTML",
                    server_port);
                return make_http_response("text/html; charset=utf-8"sv, html);
            }
            if (target == "/host-read-embed"sv) {
                auto html = ByteString::formatted(R"HTML(<!doctype html>
<html><head><title>boot</title></head><body>
<script>
window.addEventListener('message', e => {{ document.title = 'RES:' + e.data; }});
</script>
<iframe src="http://embed.test:{}/embed-read"></iframe>
</body></html>)HTML",
                    server_port);
                return make_http_response("text/html; charset=utf-8"sv, html);
            }
            if (target == "/top-write"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><head><title>boot</title></head><body><script>
localStorage.setItem('first_party', 'secret-a');
document.cookie = 'fpc=secret-a; path=/';
document.title = 'RES:TOP_WROTE:' + localStorage.getItem('first_party');
</script></body></html>)HTML"sv);
            }
            if (target == "/top-read"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><head><title>boot</title></head><body><script>
const v = localStorage.getItem('first_party');
const result = 'TOP_READ:' + (v === null ? 'NULL' : v);
if (window.top === window) {
    document.title = 'RES:' + result;
} else {
    window.top.postMessage(result, '*');
}
</script></body></html>)HTML"sv);
            }
            if (target == "/aba-outer"sv) {
                auto html = ByteString::formatted(R"HTML(<!doctype html>
<html><head><title>boot</title></head><body>
<script>
window.addEventListener('message', e => {{ document.title = 'RES:' + e.data; }});
</script>
<iframe src="http://site-b.test:{}/aba-middle"></iframe>
</body></html>)HTML",
                    server_port);
                return make_http_response("text/html; charset=utf-8"sv, html);
            }
            if (target == "/aba-middle"sv) {
                auto html = ByteString::formatted(R"HTML(<!doctype html>
<html><body>
<iframe src="http://site-a.test:{}/top-read"></iframe>
</body></html>)HTML",
                    server_port);
                return make_http_response("text/html; charset=utf-8"sv, html);
            }
            if (target == "/opaque-host"sv) {
                auto html = ByteString::formatted(R"HTML(<!doctype html>
<html><head><title>boot</title></head><body>
<script>
window.addEventListener('message', e => {{ document.title = 'RES:' + e.data; }});
</script>
<iframe sandbox="allow-scripts" src="http://site-a.test:{}/opaque-probe"></iframe>
</body></html>)HTML",
                    server_port);
                return make_http_response("text/html; charset=utf-8"sv, html);
            }
            if (target == "/opaque-probe"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><body><script>
try {
    const existing = localStorage.getItem('first_party');
    localStorage.setItem('opaque_leak', 'bad');
    window.top.postMessage('OPAQUE_UNEXPECTED:' + existing, '*');
} catch (err) {
    window.top.postMessage('OPAQUE_DENIED:' + err.name, '*');
}
</script></body></html>)HTML"sv);
            }
            return make_http_response("text/plain"sv, "ok"sv);
        });
    server_port = http_server.port();

    Test::HeadlessTestEnvironment environment { "TestPrivacyMilestone2Partitioning"sv, 90 };
    auto app = TRY_OR_FAIL(environment.create_application<Milestone2TransientApplication>());
    configure_custom_dns(dns_observer);
    auto theme = load_test_theme();

    // 1a. Same-origin embed writes under site-a.test
    {
        auto view_write = WebView::HeadlessWebView::create(theme, { 400, 300 });
        view_write->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/host-write-embed", server_port)).release_value());
        auto result = wait_for_title_prefix(*view_write, "RES:"sv);
        EXPECT(result.contains("WROTE:from-site-a|"sv));
    }

    // Positive control: embed.test under a second site-a.test view reads its own localStorage partition
    {
        auto view_same_site = WebView::HeadlessWebView::create(theme, { 400, 300 });
        view_same_site->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/host-read-embed", server_port)).release_value());
        auto result = wait_for_title_prefix(*view_same_site, "RES:"sv);
        EXPECT(result.contains("READ:from-site-a|"sv));
    }

    // Negative control: embed.test under site-b.test cannot read site-a.test's partition
    {
        auto view_other_site = WebView::HeadlessWebView::create(theme, { 400, 300 });
        view_other_site->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-b.test:{}/host-read-embed", server_port)).release_value());
        auto result = wait_for_title_prefix(*view_other_site, "RES:"sv);
        EXPECT_EQ(result, "RES:READ:NULL|"sv);
    }

    // Partitioned third-party cookies (SameSite=None; Secure; Partitioned):
    // Positive control under site-a.test, negative control under site-b.test, and A -> B -> A isolation.
    {
        auto& jar = *WebView::Application::default_session().cookie_jar;
        auto embed_url = URL::Parser::basic_parse("https://embed.test/widget"sv).release_value();
        auto parsed_embed_cookie = HTTP::Cookie::parse_cookie(embed_url, "ec=from-site-a; Path=/; Secure; SameSite=None; Partitioned"sv);
        VERIFY(parsed_embed_cookie.has_value());

        HTTP::Cookie::PartitionContext site_a_ctx { .top_level_site = "https://site-a.test"_utf16, .has_cross_site_ancestor = true };
        HTTP::Cookie::PartitionContext site_b_ctx { .top_level_site = "https://site-b.test"_utf16, .has_cross_site_ancestor = true };
        jar.set_cookie(embed_url, *parsed_embed_cookie, HTTP::Cookie::Source::Http, site_a_ctx, HTTP::Cookie::non_http_same_site_context(site_a_ctx));

        EXPECT(jar.get_cookie(embed_url, HTTP::Cookie::Source::NonHttp, site_a_ctx, HTTP::Cookie::non_http_same_site_context(site_a_ctx)).contains("ec=from-site-a"sv));
        EXPECT(jar.get_cookie(embed_url, HTTP::Cookie::Source::NonHttp, site_b_ctx, HTTP::Cookie::non_http_same_site_context(site_b_ctx)).is_empty());

        // First-party SameSite=Lax cookie on site-a.test is readable in first-party site-a.test, NOT in A -> B -> A!
        auto site_a_url = URL::Parser::basic_parse("https://site-a.test/"sv).release_value();
        auto parsed_fp_cookie = HTTP::Cookie::parse_cookie(site_a_url, "fpc=secret-a; Path=/; SameSite=Lax"sv);
        VERIFY(parsed_fp_cookie.has_value());
        HTTP::Cookie::PartitionContext fp_a_ctx { .top_level_site = "https://site-a.test"_utf16, .has_cross_site_ancestor = false };
        HTTP::Cookie::PartitionContext aba_ctx { .top_level_site = "https://site-a.test"_utf16, .has_cross_site_ancestor = true };
        jar.set_cookie(site_a_url, *parsed_fp_cookie, HTTP::Cookie::Source::NonHttp, fp_a_ctx, HTTP::Cookie::non_http_same_site_context(fp_a_ctx));
        EXPECT(jar.get_cookie(site_a_url, HTTP::Cookie::Source::NonHttp, fp_a_ctx, HTTP::Cookie::non_http_same_site_context(fp_a_ctx)).contains("fpc=secret-a"sv));
        EXPECT(jar.get_cookie(site_a_url, HTTP::Cookie::Source::NonHttp, aba_ctx, HTTP::Cookie::non_http_same_site_context(aba_ctx)).is_empty());
    }

    // 1b. A -> B -> A cross-site ancestor isolation
    {
        auto view_top_write = WebView::HeadlessWebView::create(theme, { 400, 300 });
        view_top_write->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/top-write", server_port)).release_value());
        auto wrote = wait_for_title_prefix(*view_top_write, "RES:"sv);
        EXPECT_EQ(wrote, "RES:TOP_WROTE:secret-a"sv);

        auto view_aba = WebView::HeadlessWebView::create(theme, { 400, 300 });
        view_aba->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/aba-outer", server_port)).release_value());
        auto aba_result = wait_for_title_prefix(*view_aba, "RES:"sv);
        EXPECT_EQ(aba_result, "RES:TOP_READ:NULL"sv);

        // Positive control: first-party site-a.test still reads its own storage
        auto view_top_read = WebView::HeadlessWebView::create(theme, { 400, 300 });
        view_top_read->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/top-read", server_port)).release_value());
        auto top_result = wait_for_title_prefix(*view_top_read, "RES:"sv);
        EXPECT_EQ(top_result, "RES:TOP_READ:secret-a"sv);
    }

    // 1c. Opaque contexts (sandboxed iframe without allow-same-origin and data: URL) are excluded from persistent storage
    {
        auto view_opaque = WebView::HeadlessWebView::create(theme, { 400, 300 });
        view_opaque->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/opaque-host", server_port)).release_value());
        auto opaque_result = wait_for_title_prefix(*view_opaque, "RES:"sv);
        EXPECT(opaque_result.starts_with("RES:OPAQUE_DENIED:"sv));

        auto view_data = WebView::HeadlessWebView::create(theme, { 400, 300 });
        view_data->load_from_browser_ui(URL::Parser::basic_parse("data:text/html,<script>try{localStorage.setItem('x','1');document.title='RES:UNEXPECTED';}catch(e){document.title='RES:DATA_DENIED:'+e.name;}</script>"sv).release_value());
        auto data_result = wait_for_title_prefix(*view_data, "RES:"sv);
        EXPECT(data_result.starts_with("RES:DATA_DENIED:"sv));
    }
}

static void verify_two_identities_isolated(bool use_shared_request_server)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    Atomic<size_t> cached_counter { 0 };

    PrivacyTest::IndependentHTTPServer http_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [&](ByteString const& request_head) {
            auto target = extract_request_target(request_head);
            if (target == "/write"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><head><title>boot</title></head><body><script>
localStorage.setItem('id_token', 'work-only');
document.cookie = 'id_cookie=work-only; path=/';
document.title = 'RES:WROTE:' + localStorage.getItem('id_token') + '|' + document.cookie;
</script></body></html>)HTML"sv);
            }
            if (target == "/read"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><head><title>boot</title></head><body><script>
const v = localStorage.getItem('id_token');
document.title = 'RES:READ:' + (v === null ? 'NULL' : v) + '|' + document.cookie;
</script></body></html>)HTML"sv);
            }
            if (target == "/cached-resource"sv) {
                auto n = cached_counter.fetch_add(1) + 1;
                auto body = ByteString::formatted("cached-value-{}", n);
                return make_http_response("text/plain"sv, body, "Cache-Control: public, max-age=3600\r\n"sv);
            }
            if (target == "/fetch-cached"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><head><title>boot</title></head><body><script>
fetch('/cached-resource').then(r => r.text()).then(t => { document.title = 'RES:CACHED:' + t; });
</script></body></html>)HTML"sv);
            }
            return make_http_response("text/plain"sv, "ok"sv);
        });
    u16 server_port = http_server.port();

    Test::HeadlessTestEnvironment environment { "TestPrivacyMilestone2Identities"sv, 90 };
    Array<StringView, 1> shared_args = { "--max-site-request-servers=0"sv };
    auto app = TRY_OR_FAIL(use_shared_request_server
            ? environment.create_application<Milestone2PersistentApplication>(shared_args.span())
            : environment.create_application<Milestone2PersistentApplication>());
    configure_custom_dns(dns_observer);
    auto theme = load_test_theme();

    EXPECT_EQ(WebView::RequestServerManager::the().uses_site_request_servers(), !use_shared_request_server);

    auto work_session = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(
        Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string)));
    auto personal_session = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(
        Privacy::IdentityDescriptor::persistent("personal"_string, "Personal"_string)));
    EXPECT_NE(work_session->id(), personal_session->id());

    // Open views in both identities and verify process isolation + cookie/storage isolation
    auto work_view_1 = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, work_session);
    auto personal_view = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, personal_session);
    EXPECT_NE(work_view_1->web_content_pid(), personal_view->web_content_pid());

    work_view_1->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/write", server_port)).release_value());
    auto wrote = wait_for_title_prefix(*work_view_1, "RES:"sv);
    EXPECT(wrote.contains("WROTE:work-only|"sv));
    EXPECT(wrote.contains("id_cookie=work-only"sv));

    // Positive control: second view in "work" sees both localStorage and cookies
    auto work_view_2 = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, work_session);
    work_view_2->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/read", server_port)).release_value());
    auto work_read = wait_for_title_prefix(*work_view_2, "RES:"sv);
    EXPECT(work_read.contains("READ:work-only|"sv));
    EXPECT(work_read.contains("id_cookie=work-only"sv));

    // Negative control: "personal" sees neither localStorage nor cookies from "work"
    personal_view->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/read", server_port)).release_value());
    auto personal_read = wait_for_title_prefix(*personal_view, "RES:"sv);
    EXPECT_EQ(personal_read, "RES:READ:NULL|"sv);

    // RequestServer cache isolation (verified in BOTH site-specific and shared --max-site-request-servers=0 modes!)
    auto work_rs = TRY_OR_FAIL(WebView::RequestServerManager::the().instance_for_site(*work_session, "http://app.test"_utf16));
    auto personal_rs = TRY_OR_FAIL(WebView::RequestServerManager::the().instance_for_site(*personal_session, "http://app.test"_utf16));
    EXPECT_NE(work_rs.ptr(), personal_rs.ptr());
    EXPECT(work_rs->cache_path().contains("/Identities/work"sv));
    EXPECT(personal_rs->cache_path().contains("/Identities/personal"sv));
    EXPECT_NE(work_rs->cache_path(), personal_rs->cache_path());

    // Fetch cacheable resource twice in "work" (2nd hit must serve from work's cache), then in "personal" (must miss!)
    work_view_1->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/fetch-cached", server_port)).release_value());
    auto work_cache_1 = wait_for_title_prefix(*work_view_1, "RES:CACHED:"sv);
    EXPECT_EQ(work_cache_1, "RES:CACHED:cached-value-1"sv);

    work_view_2->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/fetch-cached", server_port)).release_value());
    auto work_cache_2 = wait_for_title_prefix(*work_view_2, "RES:CACHED:"sv);
    EXPECT_EQ(work_cache_2, "RES:CACHED:cached-value-1"sv);

    personal_view->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/fetch-cached", server_port)).release_value());
    auto personal_cache = wait_for_title_prefix(*personal_view, "RES:CACHED:"sv);
    EXPECT_EQ(personal_cache, "RES:CACHED:cached-value-2"sv);

    // PermissionManager isolation across identities
    auto app_origin = URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}", server_port)).release_value().origin();
    work_session->permission_manager->set_decision(Web::Permissions::PermissionName::Geolocation, app_origin, WebView::PermissionDecision::Allow);
    EXPECT_EQ(work_session->permission_manager->effective_state(Web::Permissions::PermissionName::Geolocation, app_origin), Web::Permissions::EffectiveState::Granted);
    EXPECT_NE(personal_session->permission_manager->effective_state(Web::Permissions::PermissionName::Geolocation, app_origin), Web::Permissions::EffectiveState::Granted);

    // HistoryStore isolation across identities sharing the same SQLite database
    auto history_db = TRY_OR_FAIL(Database::Database::create(environment.directory(), "History"sv));
    EXPECT_EQ(TRY_OR_FAIL(WebView::FaviconStore::migrate_schema(history_db)), Database::MigrationOutcome::Success);
    EXPECT_EQ(TRY_OR_FAIL(WebView::HistoryStore::migrate_schema(history_db)), Database::MigrationOutcome::Success);
    auto work_history = TRY_OR_FAIL(WebView::HistoryStore::create(history_db, "work"_string));
    auto personal_history = TRY_OR_FAIL(WebView::HistoryStore::create(history_db, "personal"_string));
    auto work_url = URL::Parser::basic_parse("https://work-only.example/dashboard"sv).release_value();
    work_history->record_visit(work_url, "Work Dashboard"_string, UnixDateTime::now(), WebView::HistoryVisitTransition::Link);
    EXPECT(work_history->entry_for_url(work_url).has_value());
    EXPECT(!personal_history->entry_for_url(work_url).has_value());
}

// 2. Two persistent identities remain isolated across WebContent, RequestServer (BOTH site-specific
//    and shared --max-site-request-servers=0), HTTP caches, cookies, storage, permissions, and history.
TEST_CASE(milestone2_two_identities_isolated_site_and_shared_request_server)
{
    // Mode A: Site-specific RequestServers (default)
    verify_two_identities_isolated(/*use_shared_request_server=*/false);

    // Mode B: Shared RequestServer (--max-site-request-servers=0)
    verify_two_identities_isolated(/*use_shared_request_server=*/true);
}

// 3. SharedWorkers and BroadcastChannel are isolated by identity AND top-level site partition,
//    and closing the last private view cleanly tears down the private session so a newly opened
//    private view gets a fresh session with zero leftover cookies, storage, or SharedWorkers.
TEST_CASE(milestone2_shared_workers_broadcast_channel_and_closing_last_private_view)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    u16 server_port = 0;

    PrivacyTest::IndependentHTTPServer http_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [&](ByteString const& request_head) {
            auto target = extract_request_target(request_head);
            if (target == "/counter-worker.js"sv) {
                return make_http_response("application/javascript"sv, R"JS(
const workerId = Math.random().toString(36).slice(2, 10);
let count = 0;
onconnect = (event) => {
    const port = event.ports[0];
    count += 1;
    port.postMessage(workerId + ':' + count);
};
)JS"sv);
            }
            if (target == "/sw-embed"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><body><script>
const w = new SharedWorker('/counter-worker.js', { name: 'ctr' });
w.port.onmessage = (e) => {
    window.top.postMessage('SW:' + e.data, '*');
};
w.port.start();
</script></body></html>)HTML"sv);
            }
            if (target == "/host-sw"sv) {
                auto html = ByteString::formatted(R"HTML(<!doctype html>
<html><head><title>boot</title></head><body>
<script>
window.addEventListener('message', e => {{ document.title = 'RES:' + e.data; }});
</script>
<iframe src="http://embed.test:{}/sw-embed"></iframe>
</body></html>)HTML",
                    server_port);
                return make_http_response("text/html; charset=utf-8"sv, html);
            }
            if (target == "/bc-listen"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><head><title>boot</title></head><body><script>
window.__msgs = [];
const ch = new BroadcastChannel('isolation-bus');
ch.onmessage = (e) => {
    window.__msgs.push(e.data);
    document.title = 'MSG:' + window.__msgs.join(',');
};
document.title = 'READY';
</script></body></html>)HTML"sv);
            }
            if (target == "/bc-send"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><head><title>boot</title></head><body><script>
const ch = new BroadcastChannel('isolation-bus');
ch.postMessage('ping-work');
document.title = 'SENT';
</script></body></html>)HTML"sv);
            }
            if (target == "/bc-embed-listen"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><body><script>
const ch = new BroadcastChannel('embed-bus');
ch.onmessage = (e) => { window.top.postMessage('EMSG:' + e.data, '*'); };
window.top.postMessage('EREADY', '*');
</script></body></html>)HTML"sv);
            }
            if (target == "/bc-embed-send"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><body><script>
const ch = new BroadcastChannel('embed-bus');
ch.postMessage('embed-ping-a');
window.top.postMessage('ESENT', '*');
</script></body></html>)HTML"sv);
            }
            if (target == "/host-bc-listen"sv) {
                auto html = ByteString::formatted(R"HTML(<!doctype html>
<html><head><title>boot</title></head><body>
<script>
window.addEventListener('message', e => {{ document.title = e.data; }});
</script>
<iframe src="http://embed.test:{}/bc-embed-listen"></iframe>
</body></html>)HTML",
                    server_port);
                return make_http_response("text/html; charset=utf-8"sv, html);
            }
            if (target == "/host-bc-send"sv) {
                auto html = ByteString::formatted(R"HTML(<!doctype html>
<html><head><title>boot</title></head><body>
<script>
window.addEventListener('message', e => {{ document.title = e.data; }});
</script>
<iframe src="http://embed.test:{}/bc-embed-send"></iframe>
</body></html>)HTML",
                    server_port);
                return make_http_response("text/html; charset=utf-8"sv, html);
            }
            if (target == "/private-write"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><head><title>boot</title></head><body><script>
localStorage.setItem('pk', 'priv-secret');
document.cookie = 'pc=priv-secret; path=/';
const w = new SharedWorker('/counter-worker.js', { name: 'priv-sw' });
w.port.onmessage = (e) => {
    document.title = 'PRIV:' + e.data + '|' + localStorage.getItem('pk') + '|' + document.cookie;
};
w.port.start();
</script></body></html>)HTML"sv);
            }
            if (target == "/private-read"sv) {
                return make_http_response("text/html; charset=utf-8"sv, R"HTML(<!doctype html>
<html><head><title>boot</title></head><body><script>
const v = localStorage.getItem('pk');
const c = document.cookie;
const w = new SharedWorker('/counter-worker.js', { name: 'priv-sw' });
w.port.onmessage = (e) => {
    document.title = 'PRIV:' + e.data + '|' + (v === null ? 'NULL' : v) + '|' + c;
};
w.port.start();
</script></body></html>)HTML"sv);
            }
            return make_http_response("text/plain"sv, "ok"sv);
        });
    server_port = http_server.port();

    Test::HeadlessTestEnvironment environment { "TestPrivacyMilestone2Workers"sv, 90 };
    auto app = TRY_OR_FAIL(environment.create_application<Milestone2TransientApplication>());
    configure_custom_dns(dns_observer);
    auto theme = load_test_theme();

    auto work_session = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(
        Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string)));
    auto personal_session = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(
        Privacy::IdentityDescriptor::persistent("personal"_string, "Personal"_string)));

    // 3a. SharedWorker isolation across top-level sites AND across identities
    {
        auto work_a1 = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, work_session);
        work_a1->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/host-sw", server_port)).release_value());
        auto r1 = wait_for_title_prefix(*work_a1, "RES:SW:"sv);
        auto parts1 = r1.substring_view(7).split_view(':');
        VERIFY(parts1.size() == 2);
        ByteString worker_id_a1 = parts1[0];
        EXPECT_EQ(parts1[1], "1"sv);

        // Positive control: second tab on site-a.test in same identity shares the worker (count == 2)
        auto work_a2 = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, work_session);
        work_a2->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/host-sw", server_port)).release_value());
        auto r2 = wait_for_title_prefix(*work_a2, "RES:SW:"sv);
        auto parts2 = r2.substring_view(7).split_view(':');
        VERIFY(parts2.size() == 2);
        EXPECT_EQ(parts2[0], worker_id_a1.view());
        EXPECT_EQ(parts2[1], "2"sv);

        // Negative control 1: embed.test under site-b.test in same identity gets a distinct SharedWorker
        auto work_b = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, work_session);
        work_b->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-b.test:{}/host-sw", server_port)).release_value());
        auto rb = wait_for_title_prefix(*work_b, "RES:SW:"sv);
        auto partsb = rb.substring_view(7).split_view(':');
        VERIFY(partsb.size() == 2);
        EXPECT_NE(partsb[0], worker_id_a1.view());
        EXPECT_EQ(partsb[1], "1"sv);

        // Negative control 2: embed.test under site-a.test in another identity gets a distinct SharedWorker
        auto personal_a = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, personal_session);
        personal_a->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/host-sw", server_port)).release_value());
        auto rp = wait_for_title_prefix(*personal_a, "RES:SW:"sv);
        auto partsp = rp.substring_view(7).split_view(':');
        VERIFY(partsp.size() == 2);
        EXPECT_NE(partsp[0], worker_id_a1.view());
        EXPECT_EQ(partsp[1], "1"sv);
    }

    // 3b. BroadcastChannel isolation across identities AND across top-level sites
    {
        auto work_listener = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, work_session);
        auto personal_listener = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, personal_session);
        work_listener->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/bc-listen", server_port)).release_value());
        personal_listener->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/bc-listen", server_port)).release_value());
        EXPECT_EQ(wait_for_title_prefix(*work_listener, "READY"sv), "READY"sv);
        EXPECT_EQ(wait_for_title_prefix(*personal_listener, "READY"sv), "READY"sv);

        auto work_sender = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, work_session);
        work_sender->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/bc-send", server_port)).release_value());
        EXPECT_EQ(wait_for_title_prefix(*work_sender, "SENT"sv), "SENT"sv);

        // Positive control: work_listener receives the message
        EXPECT_EQ(wait_for_title_prefix(*work_listener, "MSG:"sv), "MSG:ping-work"sv);

        // Negative control: personal_listener does NOT receive it
        spin_for_ms(150);
        EXPECT_EQ(personal_listener->title().to_byte_string(), "READY"sv);
    }
    {
        // Cross-site BroadcastChannel isolation for embed.test under site-a.test vs site-b.test
        auto a_listener = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, work_session);
        auto b_listener = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, work_session);
        a_listener->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/host-bc-listen", server_port)).release_value());
        b_listener->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-b.test:{}/host-bc-listen", server_port)).release_value());
        EXPECT_EQ(wait_for_title_prefix(*a_listener, "EREADY"sv), "EREADY"sv);
        EXPECT_EQ(wait_for_title_prefix(*b_listener, "EREADY"sv), "EREADY"sv);

        auto a_sender = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, work_session);
        a_sender->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://site-a.test:{}/host-bc-send", server_port)).release_value());
        EXPECT_EQ(wait_for_title_prefix(*a_sender, "ESENT"sv), "ESENT"sv);

        // Positive control: embed.test under site-a.test receives the message
        EXPECT_EQ(wait_for_title_prefix(*a_listener, "EMSG:"sv), "EMSG:embed-ping-a"sv);

        // Negative control: embed.test under site-b.test does NOT receive it
        spin_for_ms(150);
        EXPECT_EQ(b_listener->title().to_byte_string(), "EREADY"sv);
    }

    // 3c. Closing the last private view cleanly tears down the private session
    {
        u64 first_private_session_id = 0;
        ByteString first_priv_worker_id;
        {
            auto priv1 = WebView::HeadlessWebView::create(theme, { 400, 300 }, WebView::IsPrivate::Yes);
            first_private_session_id = priv1->session().id();
            priv1->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/private-write", server_port)).release_value());
            auto res = wait_for_title_prefix(*priv1, "PRIV:"sv);
            EXPECT(res.contains("|priv-secret|"sv));
            EXPECT(res.contains("pc=priv-secret"sv));
            auto worker_part = res.substring_view(5).split_view('|')[0];
            auto sw_parts = worker_part.split_view(':');
            VERIFY(sw_parts.size() == 2);
            first_priv_worker_id = sw_parts[0];
            EXPECT_EQ(sw_parts[1], "1"sv);
        }
        // Last private view is now destroyed!

        auto priv2 = WebView::HeadlessWebView::create(theme, { 400, 300 }, WebView::IsPrivate::Yes);
        EXPECT_NE(priv2->session().id(), first_private_session_id);
        priv2->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://app.test:{}/private-read", server_port)).release_value());
        auto res2 = wait_for_title_prefix(*priv2, "PRIV:"sv);
        auto fields = res2.substring_view(5).split_view('|', SplitBehavior::KeepEmpty);
        VERIFY(fields.size() == 3);
        auto sw_parts2 = fields[0].split_view(':');
        VERIFY(sw_parts2.size() == 2);
        EXPECT_NE(sw_parts2[0], first_priv_worker_id.view());
        EXPECT_EQ(sw_parts2[1], "1"sv);
        EXPECT_EQ(fields[1], "NULL"sv);
        EXPECT_EQ(fields[2], ""sv);
    }
}

// 4. Legacy profile migration maps old rows strictly to the "default" identity, reconstructs
//    deterministic first-party partition keys for valid HTTP(S) origins, quarantines "file://",
//    "null", and opaque keys out of live partitions, and recovers cleanly after an interrupted migration.
TEST_CASE(milestone2_legacy_profile_migration_quarantine_and_interrupted_recovery)
{
    Test::HeadlessTestEnvironment environment { "TestPrivacyMilestone2Migration"sv, 60 };
    auto db = TRY_OR_FAIL(Database::Database::create(environment.directory(), "LegacyProfile"sv));

    // Seed pre-Milestone-2 legacy schemas: WebStorage v2, Cookies v3, History v4
    {
        TRY_OR_FAIL(db->execute_raw(R"SQL(
            CREATE TABLE IF NOT EXISTS SchemaVersions (
                store TEXT PRIMARY KEY,
                version INTEGER NOT NULL
            );
            INSERT OR REPLACE INTO SchemaVersions (store, version) VALUES ('WebStorage', 2);
            INSERT OR REPLACE INTO SchemaVersions (store, version) VALUES ('Cookies', 3);
            INSERT OR REPLACE INTO SchemaVersions (store, version) VALUES ('History', 4);

            CREATE TABLE WebStorage (
                storage_endpoint INTEGER,
                storage_key TEXT,
                bottle_key TEXT,
                bottle_value TEXT,
                last_access_time INTEGER,
                PRIMARY KEY (storage_endpoint, storage_key, bottle_key)
            );
            INSERT INTO WebStorage (storage_endpoint, storage_key, bottle_key, bottle_value, last_access_time)
            VALUES
                (2, 'https://legacy.example', 'theme', 'dark', 1000),
                (2, 'file://', 'local_note', 'keep-safe', 1001),
                (2, 'null', 'opaque_item', 'hidden', 1002);

            CREATE TABLE Cookies (
                name TEXT,
                value TEXT,
                same_site INTEGER,
                creation_time INTEGER,
                last_access_time INTEGER,
                expiry_time INTEGER,
                domain TEXT,
                path TEXT,
                secure BOOLEAN,
                http_only BOOLEAN,
                host_only BOOLEAN,
                persistent BOOLEAN,
                partition_key TEXT NOT NULL DEFAULT '',
                PRIMARY KEY (name, domain, host_only, path, partition_key)
            );

            CREATE TABLE History (
                url TEXT PRIMARY KEY,
                title TEXT NOT NULL,
                visit_count INTEGER NOT NULL,
                last_visited_time INTEGER NOT NULL,
                direct_visit_count INTEGER NOT NULL DEFAULT 0,
                last_qualifying_visit_time INTEGER NOT NULL DEFAULT 0,
                last_direct_visit_time INTEGER NOT NULL DEFAULT 0,
                decayed_visit_score REAL NOT NULL DEFAULT 0,
                decayed_direct_score REAL NOT NULL DEFAULT 0,
                score_updated_at INTEGER NOT NULL DEFAULT 0,
                favicon_hash TEXT
            );

            CREATE TABLE OmniboxEngagements (
                normalized_input TEXT NOT NULL,
                destination_kind INTEGER NOT NULL,
                destination_key TEXT NOT NULL,
                destination TEXT NOT NULL,
                explicit_use_count INTEGER NOT NULL,
                default_use_count INTEGER NOT NULL,
                last_used_time INTEGER NOT NULL,
                PRIMARY KEY (normalized_input, destination_kind, destination_key)
            );
        )SQL"sv));

        auto now_ms = UnixDateTime::now().milliseconds_since_epoch();
        TRY_OR_FAIL(db->execute_raw(ByteString::formatted(R"SQL(
            INSERT INTO Cookies (name, value, same_site, creation_time, last_access_time, expiry_time, domain, path, secure, http_only, host_only, persistent, partition_key)
            VALUES ('session', 'legacy-tok', 1, {0}, {0}, {1}, 'legacy.example', '/', 0, 0, 1, 1, '');
            INSERT INTO History (url, title, visit_count, last_visited_time)
            VALUES ('https://legacy.example/welcome', 'Legacy Welcome', 3, {0});
        )SQL",
            now_ms,
            now_ms + 3600 * 1000)));
    }

    // Phase A: Inject a fault mid-backfill and prove atomic rollback (zero data loss, schema stays at v2)
    WebView::StorageJar::set_migration_fault_hook_for_testing([](size_t migrated_rows) -> ErrorOr<void> {
        if (migrated_rows == 1)
            return Error::from_string_literal("Simulated crash mid-migration");
        return {};
    });
    auto failed_migration = WebView::StorageJar::migrate_schema(db);
    WebView::StorageJar::set_migration_fault_hook_for_testing(nullptr);
    EXPECT(failed_migration.is_error());

    // Schema version MUST remain 2 and all 3 legacy rows MUST remain intact!
    {
        auto version = TRY_OR_FAIL(db->schema_version("WebStorage"sv));
        EXPECT_EQ(version, Optional<u32> { 2u });

        auto count_stmt = TRY_OR_FAIL(db->prepare_statement("SELECT COUNT(*) FROM WebStorage"sv));
        i64 row_count = 0;
        db->execute_statement(count_stmt, [&](auto id) -> ErrorOr<void> {
            row_count = db->result_column<i64>(id, 0);
            return {};
        });
        EXPECT_EQ(row_count, 3);
    }

    // Phase B: Clear fault and run migrations (twice to verify idempotent resume!)
    EXPECT_EQ(TRY_OR_FAIL(WebView::FaviconStore::migrate_schema(db)), Database::MigrationOutcome::Success);
    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(db)), Database::MigrationOutcome::Success);
    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(db)), Database::MigrationOutcome::Success);
    EXPECT_EQ(TRY_OR_FAIL(WebView::HistoryStore::migrate_schema(db)), Database::MigrationOutcome::Success);

    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(db)), Database::MigrationOutcome::Success);
    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(db)), Database::MigrationOutcome::Success);
    EXPECT_EQ(TRY_OR_FAIL(WebView::HistoryStore::migrate_schema(db)), Database::MigrationOutcome::Success);

    // Phase C: Verify default identity received migrated data and quarantined legacy keys, while "work" sees nothing
    auto default_storage = TRY_OR_FAIL(WebView::StorageJar::create(db, "default"_string));
    auto work_storage = TRY_OR_FAIL(WebView::StorageJar::create(db, "work"_string));

    auto legacy_origin = URL::Parser::basic_parse("https://legacy.example"sv).release_value().origin();
    auto expected_key = Privacy::StoragePartitionKey::from_context(legacy_origin, legacy_origin, false)->to_storage_key_string();
    EXPECT_EQ(expected_key, "p1|https://legacy.example|https://legacy.example|0"_string);

    auto migrated_theme = default_storage->get_item(Web::StorageAPI::StorageEndpointType::LocalStorage, expected_key, "theme"_utf16);
    EXPECT_EQ(migrated_theme, Optional<Utf16String> { "dark"_utf16 });
    EXPECT(!default_storage->get_item(Web::StorageAPI::StorageEndpointType::LocalStorage, "file://"_string, "local_note"_utf16).has_value());
    EXPECT(!default_storage->get_item(Web::StorageAPI::StorageEndpointType::LocalStorage, "null"_string, "opaque_item"_utf16).has_value());

    // Quarantined legacy entries are preserved out of live web partitions and can be cleared explicitly
    auto quarantined = default_storage->quarantined_legacy_entries();
    EXPECT_EQ(quarantined.size(), 2u);
    bool saw_file_quarantine = false;
    bool saw_null_quarantine = false;
    for (auto const& entry : quarantined) {
        EXPECT(!entry.reason.is_empty());
        if (entry.legacy_storage_key == "file://"sv && entry.bottle_key == "local_note"_utf16 && entry.bottle_value == "keep-safe"_utf16)
            saw_file_quarantine = true;
        if (entry.legacy_storage_key == "null"sv && entry.bottle_key == "opaque_item"_utf16 && entry.bottle_value == "hidden"_utf16)
            saw_null_quarantine = true;
    }
    EXPECT(saw_file_quarantine);
    EXPECT(saw_null_quarantine);

    default_storage->clear_quarantined_legacy_entries();
    EXPECT(default_storage->quarantined_legacy_entries().is_empty());

    // Work identity sees ZERO migrated storage
    EXPECT(!work_storage->get_item(Web::StorageAPI::StorageEndpointType::LocalStorage, expected_key, "theme"_utf16).has_value());

    // Cookies migrated strictly to default identity
    auto default_cookies = TRY_OR_FAIL(WebView::CookieJar::create(db, "default"_string));
    auto work_cookies = TRY_OR_FAIL(WebView::CookieJar::create(db, "work"_string));
    auto legacy_url = URL::Parser::basic_parse("https://legacy.example/"sv).release_value();
    EXPECT(default_cookies->get_cookie(legacy_url, HTTP::Cookie::Source::Http, {}, {}).contains("session=legacy-tok"sv));
    EXPECT(work_cookies->get_cookie(legacy_url, HTTP::Cookie::Source::Http, {}, {}).is_empty());

    // History migrated strictly to default identity
    auto default_history = TRY_OR_FAIL(WebView::HistoryStore::create(db, "default"_string));
    auto work_history = TRY_OR_FAIL(WebView::HistoryStore::create(db, "work"_string));
    auto history_url = URL::Parser::basic_parse("https://legacy.example/welcome"sv).release_value();
    EXPECT(default_history->entry_for_url(history_url).has_value());
    EXPECT(!work_history->entry_for_url(history_url).has_value());
}

// 5. Cross-identity navigation handoff is evaluated BEFORE DNS or network dispatch in the source
//    identity: crossing from a more restrictive route (Offline) to a less restrictive route (Allow)
//    requires explicit user confirmation, displays the destination URL, drops opener/referrer/credentials,
//    never replays a POST body, and emits zero DNS or HTTP traffic in the source identity.
TEST_CASE(milestone2_cross_identity_navigation_handoff_before_dns)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::IndependentHTTPServer http_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const&) {
            return make_http_response("text/html; charset=utf-8"sv, "<!doctype html><html><head><title>HANDOFF_LOADED</title></head><body>ok</body></html>"sv);
        });
    u16 server_port = http_server.port();

    Test::HeadlessTestEnvironment environment { "TestPrivacyMilestone2Handoff"sv, 90 };
    auto app = TRY_OR_FAIL(environment.create_application<Milestone2TransientApplication>());
    configure_custom_dns(dns_observer);
    auto theme = load_test_theme();

    auto offline_session = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(
        Privacy::IdentityDescriptor::offline("offline-reader"_string, "Offline Reader"_string, /*ephemeral=*/true)));
    auto work_session = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(
        Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string)));

    EXPECT_EQ(WebView::Application::privacy_policy_coordinator().network_mode(*offline_session), Privacy::NetworkMode::Offline);
    EXPECT_EQ(WebView::Application::privacy_policy_coordinator().network_mode(*work_session), Privacy::NetworkMode::Allow);

    WebView::IdentityManager::the().add_domain_rule({
        .pattern = "*.work.example"_string,
        .target_identity_id = "work"_string,
    });

    auto offline_view = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, offline_session);
    dns_observer.reset_counts();
    http_server.reset_counts();

    // Phase A: User declines confirmation -> navigation aborted before DNS or HTTP!
    bool confirmation_prompted = false;
    bool handoff_called = false;
    Privacy::NavigationHandoffDecision prompted_decision;

    offline_view->on_confirm_identity_handoff = [&](Privacy::NavigationHandoffDecision const& decision, URL::URL const&) {
        confirmation_prompted = true;
        prompted_decision = decision;
        return false;
    };
    offline_view->on_identity_handoff = [&](Privacy::NavigationHandoffDecision const&, URL::URL const&) {
        handoff_called = true;
    };

    auto target_url_str = ByteString::formatted("http://portal.work.example:{}/secret?token=xyz", server_port);
    offline_view->load_from_browser_ui(URL::Parser::basic_parse(target_url_str).release_value());
    spin_for_ms(150);

    EXPECT(confirmation_prompted);
    EXPECT(!handoff_called);
    EXPECT_EQ(prompted_decision.action, Privacy::HandoffAction::RequiresConfirmation);
    EXPECT_EQ(prompted_decision.target_identity_id, "work"_string);
    EXPECT(prompted_decision.displayed_destination_url.bytes_as_string_view().contains("portal.work.example"sv));
    EXPECT(prompted_decision.displayed_destination_url.bytes_as_string_view().contains("/secret?token=xyz"sv));
    EXPECT(prompted_decision.open_fresh_context);
    EXPECT(prompted_decision.drop_opener);
    EXPECT(prompted_decision.strip_referrer);
    EXPECT(prompted_decision.strip_credentials);
    EXPECT(!prompted_decision.replay_post_body);
    EXPECT_EQ(dns_observer.query_count(), 0u);
    EXPECT_EQ(http_server.connection_count(), 0u);

    // Phase B: User confirms -> handoff fires BEFORE any source DNS/HTTP, and fresh context in "work" loads cleanly!
    confirmation_prompted = false;
    handoff_called = false;
    Privacy::NavigationHandoffDecision handoff_decision;
    size_t dns_at_handoff = 999;
    size_t http_at_handoff = 999;
    OwnPtr<WebView::HeadlessWebView> handed_off_view;

    offline_view->on_confirm_identity_handoff = [&](Privacy::NavigationHandoffDecision const&, URL::URL const&) {
        confirmation_prompted = true;
        return true;
    };
    offline_view->on_identity_handoff = [&](Privacy::NavigationHandoffDecision const& decision, URL::URL const& destination) {
        handoff_called = true;
        handoff_decision = decision;
        dns_at_handoff = dns_observer.query_count();
        http_at_handoff = http_server.connection_count();

        auto target_session = WebView::IdentityManager::the().find_session(decision.target_identity_id);
        VERIFY(target_session);
        handed_off_view = WebView::HeadlessWebView::create_for_session(theme, { 400, 300 }, target_session.release_nonnull());
        handed_off_view->load_from_browser_ui(destination);
    };

    offline_view->load_from_browser_ui(URL::Parser::basic_parse(target_url_str).release_value());
    EXPECT(confirmation_prompted);
    EXPECT(handoff_called);
    EXPECT_EQ(handoff_decision.action, Privacy::HandoffAction::HandoffToNewContext);
    EXPECT_EQ(handoff_decision.target_identity_id, "work"_string);
    EXPECT(handoff_decision.open_fresh_context);
    EXPECT(handoff_decision.drop_opener);
    EXPECT(handoff_decision.strip_referrer);
    EXPECT(handoff_decision.strip_credentials);
    EXPECT(!handoff_decision.replay_post_body);
    // Source identity MUST have emitted zero DNS and zero HTTP before handoff!
    EXPECT_EQ(dns_at_handoff, 0u);
    EXPECT_EQ(http_at_handoff, 0u);

    // Positive control: the fresh context in "work" loads the destination URL cleanly!
    VERIFY(handed_off_view);
    EXPECT_EQ(wait_for_title_prefix(*handed_off_view, "HANDOFF_LOADED"sv), "HANDOFF_LOADED"sv);
    EXPECT(http_server.request_count() >= 1u);
}
