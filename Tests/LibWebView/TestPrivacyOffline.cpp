/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include "PrivacyTestFixtures.h"
#include <AK/LexicalPath.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestControlClient.h>
#include <LibRequests/WebSocket.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/Extensions/CatalogClient.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/ReportSubmission.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/Settings.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <RequestServer/CacheLevel.h>

namespace {

class Milestone1Application : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(Milestone1Application)

public:
    explicit Milestone1Application(Optional<ByteString> ladybird_binary_path)
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

class Milestone1CachedApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(Milestone1CachedApplication)

public:
    explicit Milestone1CachedApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions& request_server_options, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        request_server_options.http_disk_cache_mode = WebView::HTTPDiskCacheMode::Enabled;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }
};

struct SyncHTTPResult {
    bool finished { false };
    Optional<Requests::NetworkError> error;
    Optional<u32> status_code;
    ByteString body;
};

static SyncHTTPResult perform_request(
    Requests::RequestClient& client,
    StringView url_string,
    HTTP::CacheMode cache_mode = HTTP::CacheMode::NoStore,
    Optional<HTTP::NetworkIsolationKey> network_isolation_key = {})
{
    SyncHTTPResult result;
    auto request = client.start_request(
        "GET"sv,
        URL::Parser::basic_parse(url_string).release_value(),
        {},
        *HTTP::HeaderList::create(),
        {},
        cache_mode,
        HTTP::Cookie::IncludeCredentials::No,
        Requests::RequestClient::TransferLease::No,
        {},
        Requests::RequestClient::CacheMissNotification::No,
        0,
        move(network_isolation_key));
    VERIFY(request);

    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(15), [&] { timed_out = true; });
    timer->start();

    request->set_buffered_request_finished_callback([&](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error, HTTP::HeaderList const&, Optional<u32> status_code, Optional<String> const&, Requests::CacheState, Core::ImmutableBytes payload) {
        result.error = network_error;
        result.status_code = status_code;
        result.body = ByteString::copy(payload.bytes());
        result.finished = true;
    });

    Core::EventLoop::current().spin_until([&] { return result.finished || timed_out; });
    timer->stop();
    VERIFY(!timed_out);
    return result;
}

static Vector<ByteString> perform_resolve_host(Requests::RequestClient& client, ByteString const& host)
{
    bool finished = false;
    bool timed_out = false;
    Vector<ByteString> resolved;

    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
    timer->start();
    client.resolve_host(host, [&](Vector<ByteString> addresses) {
        resolved = move(addresses);
        finished = true;
    });
    Core::EventLoop::current().spin_until([&] { return finished || timed_out; });
    timer->stop();
    VERIFY(!timed_out);
    return resolved;
}

static void spin_for_ms(i64 ms)
{
    bool done = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(ms), [&] { done = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return done; });
}

static ByteString run_page_fetch_probe(WebView::HeadlessWebView& view, StringView target_url)
{
    auto html = ByteString::formatted(R"HTML(<!doctype html>
<html>
<head><title>boot</title></head>
<body>
<div id="local">ok</div>
<script>
fetch("{}")
  .then(r => r.text())
  .then(text => {{ document.title = "fetched:" + text; }})
  .catch(() => {{ document.title = "failed"; }});
</script>
</body>
</html>)HTML",
        target_url);

    view.load_html(html);

    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(15), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] {
        auto title = view.title().to_byte_string();
        return timed_out || title.starts_with("fetched:"sv) || title == "failed"sv;
    });
    timer->stop();
    VERIFY(!timed_out);
    return view.title().to_byte_string();
}

}

TEST_CASE(milestone1_startup_offline_and_browser_services)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::IndependentHTTPServer service_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const&) {
            return ByteString { "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}" };
        });

    auto dns_port_str = ByteString::number(dns_observer.port());
    auto report_endpoint = ByteString::formatted("http://reports.example:{}", service_server.port());

    auto previous_reports_endpoint = Core::Environment::get("LADYBIRD_REPORTS_ENDPOINT"sv);
    MUST(Core::Environment::set("LADYBIRD_REPORTS_ENDPOINT"sv, report_endpoint, Core::Environment::Overwrite::Yes));
    ScopeGuard restore_env = [&] {
        if (previous_reports_endpoint.has_value())
            (void)Core::Environment::set("LADYBIRD_REPORTS_ENDPOINT"sv, *previous_reports_endpoint, Core::Environment::Overwrite::Yes);
        else
            (void)Core::Environment::unset("LADYBIRD_REPORTS_ENDPOINT"sv);
    };

    Test::HeadlessTestEnvironment environment { "Milestone1StartupOffline"sv, 120 };
    Array<StringView, 7> args {
        "--temporary-profile"sv,
        "--disable-http-disk-cache"sv,
        "--dns-server"sv,
        "127.0.0.1"sv,
        "--dns-port"sv,
        dns_port_str.view(),
        "--offline"sv,
    };
    auto app = TRY_OR_FAIL(environment.create_application<Milestone1Application>(args));
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    // Default RequestServer instance must have been launched with OS-level --disable-network sandbox.
    auto browser_instance = TRY_OR_FAIL(WebView::Application::request_server_manager().browser_instance(WebView::Application::default_session()));
    EXPECT(browser_instance->network_sandbox_denied());

    // Automatic startup content blocker list update must not start any network update while Offline.
    EXPECT(!WebView::Application::the().content_blocker_list_update_in_progress());

    // AMO catalog client must fail closed without touching DNS or HTTP.
    WebView::Extensions::CatalogClient catalog;
    catalog.set_enable_public_amo_catalog(true);
    bool catalog_failed = false;
    catalog.search("privacy"sv, [&](ErrorOr<Vector<WebView::Extensions::CatalogEntry>> result) {
        catalog_failed = result.is_error();
    });
    EXPECT(catalog_failed);

    // ReportSubmission must fail closed without touching DNS or HTTP.
    WebView::ReportSubmission::Payload payload { .manifest = "{\"protocol\":1}", .attachments = {} };
    auto submission = WebView::ReportSubmission::create(move(payload));
    bool report_failed = false;
    submission->on_failed = [&](WebView::ReportSubmission::Failure failure, String const&) {
        EXPECT_EQ(failure, WebView::ReportSubmission::Failure::Sending);
        report_failed = true;
    };
    submission->start();
    Core::EventLoop::current().spin_until([&] { return report_failed; });
    EXPECT(report_failed);

    // Local HTML still renders cleanly while Offline.
    auto probe_url = ByteString::formatted("http://service.example:{}/check", service_server.port());
    EXPECT_EQ(run_page_fetch_probe(*view, probe_url), "failed"sv);

    spin_for_ms(150);
    EXPECT_EQ(dns_observer.query_count(), 0u);
    EXPECT_EQ(service_server.connection_count(), 0u);
    EXPECT_EQ(service_server.request_count(), 0u);
}

TEST_CASE(milestone1_ipv4_ipv6_os_dns_speculative_and_redirects)
{
    PrivacyTest::IndependentDNSObserver dns_observer(true);
    PrivacyTest::IndependentHTTPServer target_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const&) {
            return ByteString { "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 18\r\nConnection: close\r\n\r\nredirect-target-ok" };
        });
    PrivacyTest::IndependentHTTPServer redirect_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [target_port = target_server.port()](ByteString const& request_head) {
            if (request_head.starts_with("GET /redirect "sv)) {
                return ByteString::formatted(
                    "HTTP/1.1 302 Found\r\nLocation: http://target.example:{}/dest\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                    target_port);
            }
            return ByteString { "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 5\r\nConnection: close\r\n\r\nv4-ok" };
        });
    PrivacyTest::IndependentHTTPServer ipv6_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv6,
        [](ByteString const&) {
            return ByteString { "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 5\r\nConnection: close\r\n\r\nv6-ok" };
        });

    auto dns_port_str = ByteString::number(dns_observer.port());
    auto v4_url = ByteString::formatted("http://v4.example:{}/ok", redirect_server.port());
    auto redirect_url = ByteString::formatted("http://v4.example:{}/redirect", redirect_server.port());
    auto v6_literal_url = ByteString::formatted("http://[::1]:{}/v6", ipv6_server.port());

    // Positive control: IPv4, IPv6, redirect chain, speculative connect, custom DNS, and SystemDNS all work when Allowed.
    {
        Test::HeadlessTestEnvironment environment { "Milestone1TransportsAllow"sv, 120 };
        Array<StringView, 7> args {
            "--temporary-profile"sv,
            "--disable-http-disk-cache"sv,
            "--dns-server"sv,
            "127.0.0.1"sv,
            "--dns-port"sv,
            dns_port_str.view(),
            "--privacy-network-mode=allow"sv,
        };
        auto app = TRY_OR_FAIL(environment.create_application<Milestone1Application>(args));
        auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        auto client = TRY_OR_FAIL(WebView::Application::request_server_client(WebView::IsPrivate::No));

        auto v4_res = perform_request(*client, v4_url);
        EXPECT(!v4_res.error.has_value());
        EXPECT_EQ(v4_res.body, "v4-ok"sv);

        auto v6_res = perform_request(*client, v6_literal_url);
        EXPECT(!v6_res.error.has_value());
        EXPECT_EQ(v6_res.body, "v6-ok"sv);
        EXPECT(ipv6_server.connection_count() >= 1u);

        EXPECT_EQ(run_page_fetch_probe(*view, redirect_url), "fetched:redirect-target-ok"sv);
        EXPECT(target_server.request_count() >= 1u);

        auto resolved = perform_resolve_host(*client, "lookup.example");
        EXPECT(!resolved.is_empty());

        // Speculative preconnect establishes a TCP connection when allowed.
        size_t before_preconnect = redirect_server.connection_count();
        client->ensure_connection(URL::Parser::basic_parse(v4_url).release_value(), RequestServer::CacheLevel::CreateConnection);
        bool preconnected = false;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [&] { preconnected = true; });
        timer->start();
        Core::EventLoop::current().spin_until([&] {
            return preconnected || redirect_server.connection_count() > before_preconnect;
        });
        timer->stop();
        EXPECT(redirect_server.connection_count() > before_preconnect);

        // OS SystemDNS resolution via Hub RequestControlClient works when allowed.
        WebView::Application::settings().set_dns_settings(WebView::SystemDNS {}, true);
        auto os_resolved = perform_resolve_host(*client, "localhost");
        EXPECT(!os_resolved.is_empty());
    }

    dns_observer.reset_counts();
    redirect_server.reset_counts();
    target_server.reset_counts();
    ipv6_server.reset_counts();

    // Negative control: Offline denies IPv4, IPv6, redirects, speculative preconnect, custom DNS, and OS SystemDNS with 0 traffic.
    {
        Test::HeadlessTestEnvironment environment { "Milestone1TransportsOffline"sv, 120 };
        Array<StringView, 7> args {
            "--temporary-profile"sv,
            "--disable-http-disk-cache"sv,
            "--dns-server"sv,
            "127.0.0.1"sv,
            "--dns-port"sv,
            dns_port_str.view(),
            "--offline"sv,
        };
        auto app = TRY_OR_FAIL(environment.create_application<Milestone1Application>(args));
        auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        auto client = TRY_OR_FAIL(WebView::Application::request_server_client(WebView::IsPrivate::No));

        auto v4_res = perform_request(*client, v4_url);
        EXPECT(v4_res.error == Requests::NetworkError::BlockedByPrivacyPolicy);

        auto v6_res = perform_request(*client, v6_literal_url);
        EXPECT(v6_res.error == Requests::NetworkError::BlockedByPrivacyPolicy);

        EXPECT_EQ(run_page_fetch_probe(*view, redirect_url), "failed"sv);

        auto custom_resolved = perform_resolve_host(*client, "lookup.example");
        EXPECT(custom_resolved.is_empty());

        client->ensure_connection(URL::Parser::basic_parse(v4_url).release_value(), RequestServer::CacheLevel::CreateConnection);
        client->ensure_connection(URL::Parser::basic_parse(v4_url).release_value(), RequestServer::CacheLevel::ResolveOnly);

        // Switch to SystemDNS while offline and verify Hub delegated OS DNS is also denied.
        WebView::Application::settings().set_dns_settings(WebView::SystemDNS {}, true);
        auto os_resolved = perform_resolve_host(*client, "localhost");
        EXPECT(os_resolved.is_empty());

        spin_for_ms(150);
        EXPECT_EQ(dns_observer.query_count(), 0u);
        EXPECT_EQ(redirect_server.connection_count(), 0u);
        EXPECT_EQ(target_server.connection_count(), 0u);
        EXPECT_EQ(ipv6_server.connection_count(), 0u);
    }
}

TEST_CASE(milestone1_websockets_allowed_denied_and_live_revocation)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::IndependentWebSocketServer ws_server;

    auto dns_port_str = ByteString::number(dns_observer.port());
    auto ws_url = URL::Parser::basic_parse(ByteString::formatted("ws://ws.example:{}/echo", ws_server.port())).release_value();

    Test::HeadlessTestEnvironment environment { "Milestone1WebSockets"sv, 120 };
    Array<StringView, 7> args {
        "--temporary-profile"sv,
        "--disable-http-disk-cache"sv,
        "--dns-server"sv,
        "127.0.0.1"sv,
        "--dns-port"sv,
        dns_port_str.view(),
        "--privacy-network-mode=allow"sv,
    };
    auto app = TRY_OR_FAIL(environment.create_application<Milestone1Application>(args));
    auto client = TRY_OR_FAIL(WebView::Application::request_server_client(WebView::IsPrivate::No));

    // Positive control: WebSocket connects and echoes while Allowed.
    auto headers = HTTP::HeaderList::create();
    auto ws = client->websocket_connect(ws_url, {}, "http://ws.example"sv, {}, {}, *headers, 0, {});
    VERIFY(ws);

    bool opened = false;
    bool echoed = false;
    bool closed_or_errored = false;
    ws->on_open = [&] {
        opened = true;
        ws->send("hello-ws"sv);
    };
    ws->on_message = [&](Requests::WebSocket::Message message) {
        if (StringView { message.data.bytes() } == "hello-ws"sv)
            echoed = true;
    };
    ws->on_error = [&](Requests::WebSocket::Error) { closed_or_errored = true; };
    ws->on_close = [&](u16, ByteString, bool) { closed_or_errored = true; };

    {
        bool timed_out = false;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
        timer->start();
        Core::EventLoop::current().spin_until([&] { return echoed || timed_out; });
        timer->stop();
        VERIFY(!timed_out);
    }
    EXPECT(opened);
    EXPECT(echoed);
    EXPECT_EQ(ws_server.handshake_count(), 1u);

    // Live revocation while WebSocket is open: must close the live socket immediately.
    auto transition = WebView::Application::privacy_policy_coordinator().set_network_mode(
        WebView::Application::default_session(), Privacy::NetworkMode::Offline);
    EXPECT(transition.committed);

    {
        bool timed_out = false;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
        timer->start();
        Core::EventLoop::current().spin_until([&] {
            return timed_out || (closed_or_errored && ws_server.client_disconnect_count() >= 1u);
        });
        timer->stop();
        VERIFY(!timed_out);
    }
    EXPECT(closed_or_errored);
    EXPECT(ws_server.client_disconnect_count() >= 1u);

    // Negative control: new WebSocket connection after tightening fails with zero DNS or TCP traffic.
    dns_observer.reset_counts();
    ws_server.reset_counts();

    auto ws2 = client->websocket_connect(ws_url, {}, "http://ws.example"sv, {}, {}, *headers, 0, {});
    VERIFY(ws2);
    bool ws2_errored = false;
    bool ws2_opened = false;
    ws2->on_open = [&] { ws2_opened = true; };
    ws2->on_error = [&](Requests::WebSocket::Error) { ws2_errored = true; };
    ws2->on_close = [&](u16, ByteString, bool) { ws2_errored = true; };

    {
        bool timed_out = false;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
        timer->start();
        Core::EventLoop::current().spin_until([&] { return ws2_errored || timed_out; });
        timer->stop();
        VERIFY(!timed_out);
    }
    EXPECT(!ws2_opened);
    EXPECT(ws2_errored);
    spin_for_ms(150);
    EXPECT_EQ(dns_observer.query_count(), 0u);
    EXPECT_EQ(ws_server.connection_count(), 0u);
}

TEST_CASE(milestone1_live_revocation_in_flight_stream_and_relax_restart)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::IndependentStreamingHTTPServer stream_server;
    PrivacyTest::IndependentHTTPServer normal_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const&) {
            return ByteString { "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 10\r\nConnection: close\r\n\r\nrelaxed-ok" };
        });

    auto dns_port_str = ByteString::number(dns_observer.port());
    auto stream_url = ByteString::formatted("http://stream.example:{}/slow", stream_server.port());
    auto normal_url = ByteString::formatted("http://relaxed.example:{}/ok", normal_server.port());

    // Part A: Live tightening while an HTTP stream is actively open aborts it, closes the TCP socket,
    // and blocks subsequent traffic while keeping local rendering functional.
    {
        Test::HeadlessTestEnvironment environment { "Milestone1LiveRevoke"sv, 120 };
        Array<StringView, 7> args {
            "--temporary-profile"sv,
            "--disable-http-disk-cache"sv,
            "--dns-server"sv,
            "127.0.0.1"sv,
            "--dns-port"sv,
            dns_port_str.view(),
            "--privacy-network-mode=allow"sv,
        };
        auto app = TRY_OR_FAIL(environment.create_application<Milestone1Application>(args));
        auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        auto client = TRY_OR_FAIL(WebView::Application::request_server_client(WebView::IsPrivate::No));

        auto request = client->start_request(
            "GET"sv,
            URL::Parser::basic_parse(stream_url).release_value(),
            {},
            *HTTP::HeaderList::create(),
            {},
            HTTP::CacheMode::NoStore,
            HTTP::Cookie::IncludeCredentials::No);
        VERIFY(request);

        bool got_chunk = false;
        bool finished = false;
        Optional<Requests::NetworkError> finish_error;
        request->set_unbuffered_request_callbacks(
            [](NonnullRefPtr<HTTP::HeaderList>, Optional<u32>, Optional<String> const&, Requests::CacheState) { },
            [&](Requests::ResponseData data) {
                if (!data.bytes().is_empty())
                    got_chunk = true;
            },
            [&](Core::ImmutableBytes data) {
                if (!data.bytes().is_empty())
                    got_chunk = true;
            },
            [&](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> network_error) {
                finish_error = network_error;
                finished = true;
            });

        {
            bool timed_out = false;
            auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
            timer->start();
            Core::EventLoop::current().spin_until([&] { return got_chunk || timed_out; });
            timer->stop();
            VERIFY(!timed_out);
        }
        EXPECT(got_chunk);
        EXPECT(!finished);

        auto before_epoch = WebView::Application::privacy_policy_coordinator().committed_epoch(WebView::Application::default_session());
        auto transition = WebView::Application::privacy_policy_coordinator().set_network_mode(
            WebView::Application::default_session(), Privacy::NetworkMode::Offline);
        EXPECT(transition.committed);
        EXPECT(transition.epoch > before_epoch);

        {
            bool timed_out = false;
            auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
            timer->start();
            Core::EventLoop::current().spin_until([&] {
                return timed_out || (finished && stream_server.client_disconnect_count() >= 1u);
            });
            timer->stop();
            VERIFY(!timed_out);
        }
        EXPECT(finished);
        EXPECT(finish_error == Requests::NetworkError::BlockedByPrivacyPolicy);
        EXPECT(stream_server.client_disconnect_count() >= 1u);

        dns_observer.reset_counts();
        normal_server.reset_counts();

        auto denied_after = perform_request(*client, normal_url);
        EXPECT(denied_after.error == Requests::NetworkError::BlockedByPrivacyPolicy);
        EXPECT_EQ(run_page_fetch_probe(*view, normal_url), "failed"sv);
        spin_for_ms(150);
        EXPECT_EQ(dns_observer.query_count(), 0u);
        EXPECT_EQ(normal_server.connection_count(), 0u);
    }

    // Part B: Starting Offline (--disable-network sandbox) and relaxing live to Allow replaces
    // the network-sandboxed RequestServer cleanly so subsequent allowed traffic succeeds.
    dns_observer.reset_counts();
    normal_server.reset_counts();
    {
        Test::HeadlessTestEnvironment environment { "Milestone1OfflineToAllow"sv, 120 };
        Array<StringView, 7> args {
            "--temporary-profile"sv,
            "--disable-http-disk-cache"sv,
            "--dns-server"sv,
            "127.0.0.1"sv,
            "--dns-port"sv,
            dns_port_str.view(),
            "--offline"sv,
        };
        auto app = TRY_OR_FAIL(environment.create_application<Milestone1Application>(args));
        auto instance = TRY_OR_FAIL(WebView::Application::request_server_manager().browser_instance(WebView::Application::default_session()));
        EXPECT(instance->network_sandbox_denied());

        auto denied = perform_request(instance->ui_client(), normal_url);
        EXPECT(denied.error == Requests::NetworkError::BlockedByPrivacyPolicy);
        EXPECT_EQ(dns_observer.query_count(), 0u);
        EXPECT_EQ(normal_server.connection_count(), 0u);

        auto relax = WebView::Application::privacy_policy_coordinator().set_network_mode(
            WebView::Application::default_session(), Privacy::NetworkMode::Allow);
        EXPECT(relax.committed);
        EXPECT(!instance->network_sandbox_denied());

        auto allowed = perform_request(instance->ui_client(), normal_url);
        EXPECT(!allowed.error.has_value());
        EXPECT_EQ(allowed.body, "relaxed-ok"sv);
        EXPECT(dns_observer.query_count() >= 1u);
        EXPECT(normal_server.request_count() >= 1u);
    }
}

TEST_CASE(milestone1_fresh_cache_without_revalidation_and_webrtc_gating)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::IndependentHTTPServer cache_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const& request_head) {
            if (request_head.starts_with("GET /cached "sv)) {
                return ByteString {
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Cache-Control: public, max-age=3600\r\n"
                    "Content-Length: 11\r\n"
                    "Connection: close\r\n\r\n"
                    "cached-body"
                };
            }
            if (request_head.starts_with("GET /nocache "sv)) {
                return ByteString {
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Cache-Control: no-cache\r\n"
                    "ETag: \"v1\"\r\n"
                    "Content-Length: 15\r\n"
                    "Connection: close\r\n\r\n"
                    "revalidate-body"
                };
            }
            return ByteString { "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" };
        });

    auto dns_port_str = ByteString::number(dns_observer.port());
    auto cached_url = ByteString::formatted("http://cache.example:{}/cached", cache_server.port());
    auto nocache_url = ByteString::formatted("http://cache.example:{}/nocache", cache_server.port());
    auto uncached_url = ByteString::formatted("http://cache.example:{}/miss", cache_server.port());

    Test::HeadlessTestEnvironment environment { "Milestone1CacheAndWebRTC"sv, 120 };
    Array<StringView, 6> args {
        "--temporary-profile"sv,
        "--dns-server"sv,
        "127.0.0.1"sv,
        "--dns-port"sv,
        dns_port_str.view(),
        "--privacy-network-mode=allow"sv,
    };
    auto app = TRY_OR_FAIL(environment.create_application<Milestone1CachedApplication>(args));
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    // Use the site-specific RequestServer (which owns the site's HTTP disk cache) and a matching NetworkIsolationKey.
    auto site = "http://cache.example"_utf16;
    auto site_instance = TRY_OR_FAIL(WebView::Application::request_server_manager().instance_for_site(
        WebView::Application::default_session(), site));
    auto& client = site_instance->ui_client();
    HTTP::NetworkIsolationKey nik { .top_level_site = site, .frame_site = site };

    // Populate disk cache while Allowed.
    auto warm_cached = perform_request(client, cached_url, HTTP::CacheMode::Default, nik);
    EXPECT(!warm_cached.error.has_value());
    EXPECT_EQ(warm_cached.body, "cached-body"sv);

    auto warm_nocache = perform_request(client, nocache_url, HTTP::CacheMode::Default, nik);
    EXPECT(!warm_nocache.error.has_value());
    EXPECT_EQ(warm_nocache.body, "revalidate-body"sv);

    // Launch a WebRTCClient while Allowed; it must be marked network_access_allowed == true.
    RefPtr<WebView::WebContentClient> web_content_client;
    WebView::WebContentClient::for_each_client([&](WebView::WebContentClient& wc) {
        web_content_client = wc;
        return IterationDecision::Break;
    });
    VERIFY(web_content_client);

    auto allowed_webrtc_handle = TRY_OR_FAIL(WebView::launch_webrtc_client_process(*web_content_client));
    Optional<pid_t> unrestricted_webrtc_pid;
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::WebRTCClient) {
            EXPECT(process.network_access_allowed());
            unrestricted_webrtc_pid = process.pid();
        }
    });
    VERIFY(unrestricted_webrtc_pid.has_value());

    // Tighten live to Offline.
    auto transition = WebView::Application::privacy_policy_coordinator().set_network_mode(
        WebView::Application::default_session(), Privacy::NetworkMode::Offline);
    EXPECT(transition.committed);

    // Unrestricted WebRTCClient from Allow mode must be forcefully terminated and reaped.
    auto count_unrestricted_webrtc = [] {
        size_t count = 0;
        WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
            if (process.type() == WebView::ProcessType::WebRTCClient && process.network_access_allowed())
                ++count;
        });
        return count;
    };
    {
        bool timed_out = false;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
        timer->start();
        Core::EventLoop::current().spin_until([&] {
            return timed_out || count_unrestricted_webrtc() == 0u;
        });
        timer->stop();
        VERIFY(!timed_out);
    }
    EXPECT_EQ(count_unrestricted_webrtc(), 0u);

    // Launching WebRTCClient while Offline must enforce --disable-network (network_access_allowed == false).
    auto offline_webrtc_handle = TRY_OR_FAIL(WebView::launch_webrtc_client_process(*web_content_client));
    bool found_offline_webrtc = false;
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::WebRTCClient) {
            EXPECT(!process.network_access_allowed());
            found_offline_webrtc = true;
        }
    });
    EXPECT(found_offline_webrtc);
    (void)allowed_webrtc_handle;
    (void)offline_webrtc_handle;

    // Reset observers and verify disk cache behavior while Offline.
    dns_observer.reset_counts();
    cache_server.reset_counts();

    // Fresh cached entry MUST still be served without touching DNS or network.
    auto offline_cached = perform_request(client, cached_url, HTTP::CacheMode::Default, nik);
    EXPECT(!offline_cached.error.has_value());
    EXPECT_EQ(offline_cached.body, "cached-body"sv);

    // Entry requiring revalidation (Cache-Control: no-cache) MUST fail closed with zero traffic.
    auto offline_nocache = perform_request(client, nocache_url, HTTP::CacheMode::Default, nik);
    EXPECT(offline_nocache.error == Requests::NetworkError::BlockedByPrivacyPolicy);

    // Uncached entry MUST fail closed with zero traffic.
    auto offline_miss = perform_request(client, uncached_url, HTTP::CacheMode::Default, nik);
    EXPECT(offline_miss.error == Requests::NetworkError::BlockedByPrivacyPolicy);

    spin_for_ms(150);
    EXPECT_EQ(dns_observer.query_count(), 0u);
    EXPECT_EQ(cache_server.connection_count(), 0u);
    EXPECT_EQ(cache_server.request_count(), 0u);
}
