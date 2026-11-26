/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include "PrivacyTestFixtures.h"
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestControlClient.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/Utilities.h>

namespace {

// Uses non-test-mode so RequestServer does not short-circuit *.test to loopback and actually queries
// our independent DNS observer when allowed.
class Milestone0Application : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(Milestone0Application)

public:
    explicit Milestone0Application(Optional<ByteString> ladybird_binary_path)
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

static ByteString run_local_page_subresource_probe(WebView::HeadlessWebView& view, StringView target_url)
{
    size_t loads_finished = 0;
    view.on_load_finish = [&](auto const&) { ++loads_finished; };

    auto html = ByteString::formatted(R"HTML(<!doctype html>
<html>
<head><title>boot</title></head>
<body>
<div id="local">local-rendered</div>
<script>
const target = "{}";
const localReady = document.getElementById("local").textContent;
fetch(target)
  .then(r => r.text())
  .then(text => {{ document.title = localReady + ":ok:" + text; }})
  .catch(() => {{ document.title = localReady + ":fetch-failed"; }});
</script>
</body>
</html>)HTML",
        target_url);

    view.load_html(html);

    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(20), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] {
        return timed_out || view.title().to_byte_string().starts_with("local-rendered:"sv);
    });
    timer->stop();
    VERIFY(!timed_out);
    return view.title().to_byte_string();
}

}

TEST_CASE(milestone0_http_allowed_and_denied_with_independent_observers)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::IndependentHTTPServer http_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTP,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const&) {
            return ByteString { "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 7\r\nConnection: close\r\n\r\nhttp-ok" };
        });

    auto dns_port_str = ByteString::number(dns_observer.port());
    auto target_url = ByteString::formatted("http://fixture.example:{}/resource", http_server.port());

    // Positive control: fixed allow startup policy resolves via DNS observer and fetches fixture.
    {
        Test::HeadlessTestEnvironment environment { "Milestone0HTTPAllow"sv, 120 };
        Array<StringView, 7> args {
            "--temporary-profile"sv,
            "--disable-http-disk-cache"sv,
            "--dns-server"sv,
            "127.0.0.1"sv,
            "--dns-port"sv,
            dns_port_str.view(),
            "--privacy-network-mode=allow"sv,
        };
        auto app = TRY_OR_FAIL(environment.create_application<Milestone0Application>(args));
        auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });

        auto result = run_local_page_subresource_probe(*view, target_url);
        EXPECT_EQ(result, "local-rendered:ok:http-ok"sv);
        EXPECT(dns_observer.query_count() >= 1u);
        EXPECT(http_server.connection_count() >= 1u);
        EXPECT(http_server.request_count() >= 1u);
    }

    dns_observer.reset_counts();
    http_server.reset_counts();

    // Negative control: fixed offline startup policy renders local page, fails fetch visibly,
    // and generates zero DNS queries and zero HTTP server connections.
    {
        Test::HeadlessTestEnvironment environment { "Milestone0HTTPDeny"sv, 120 };
        Array<StringView, 7> args {
            "--temporary-profile"sv,
            "--disable-http-disk-cache"sv,
            "--dns-server"sv,
            "127.0.0.1"sv,
            "--dns-port"sv,
            dns_port_str.view(),
            "--privacy-network-mode=offline"sv,
        };
        auto app = TRY_OR_FAIL(environment.create_application<Milestone0Application>(args));
        auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });

        auto result = run_local_page_subresource_probe(*view, target_url);
        EXPECT_EQ(result, "local-rendered:fetch-failed"sv);
        EXPECT_EQ(dns_observer.query_count(), 0u);
        EXPECT_EQ(http_server.connection_count(), 0u);
        EXPECT_EQ(http_server.request_count(), 0u);

        // Kill RequestServer so Hub launches a brand-new replacement RequestServer and verify
        // the newly created process also starts default-closed and enforces the offline policy.
        NonnullRefPtr<Requests::RequestControlClient> old_control = WebView::Application::request_server_control_client();
        WebView::Application::process_manager().for_each_process([](WebView::Process& process) {
            if (process.type() == WebView::ProcessType::RequestServer)
                MUST(Core::System::kill(process.pid(), SIGKILL));
        });
        Core::EventLoop::current().spin_until([&] {
            return &WebView::Application::request_server_control_client() != old_control.ptr();
        });

        // Also test a newly launched site-specific RequestServer instance.
        auto site_instance = TRY_OR_FAIL(WebView::Application::request_server_manager().instance_for_site(
            WebView::Application::default_session(), "http://fixture.example"_utf16));
        Optional<Requests::NetworkError> direct_error;
        bool direct_finished = false;
        auto req = site_instance->ui_client().start_request(
            "GET"sv, URL::Parser::basic_parse(target_url).release_value(), {}, *HTTP::HeaderList::create(), {},
            HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
        VERIFY(req);
        req->set_buffered_request_finished_callback([&](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& err, HTTP::HeaderList const&, Optional<u32>, Optional<String> const&, Requests::CacheState, Core::ImmutableBytes) {
            direct_error = err;
            direct_finished = true;
        });
        Core::EventLoop::current().spin_until([&] { return direct_finished; });
        EXPECT(direct_error == Requests::NetworkError::BlockedByPrivacyPolicy);

        auto after_restart_result = run_local_page_subresource_probe(*view, target_url);
        EXPECT_EQ(after_restart_result, "local-rendered:fetch-failed"sv);
        EXPECT_EQ(dns_observer.query_count(), 0u);
        EXPECT_EQ(http_server.connection_count(), 0u);
        EXPECT_EQ(http_server.request_count(), 0u);
    }
}

TEST_CASE(milestone0_https_allowed_and_denied_with_trusted_test_certificate)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::IndependentHTTPServer https_server(
        PrivacyTest::IndependentHTTPServer::Protocol::HTTPS,
        PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const&) {
            return ByteString { "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 8\r\nConnection: close\r\n\r\nhttps-ok" };
        });

    auto dns_port_str = ByteString::number(dns_observer.port());
    auto target_url = ByteString::formatted("https://device.test:{}/resource", https_server.port());

    // Positive control: HTTPS fixture resolves via DNS observer and validates against trusted test CA.
    {
        Test::HeadlessTestEnvironment environment { "Milestone0HTTPSAllow"sv, 120 };
        auto ca_path = PrivacyTest::write_test_ca_file(environment.directory());
        auto cert_arg = ByteString::formatted("--certificate={}", ca_path);
        Array<StringView, 8> args {
            "--temporary-profile"sv,
            "--disable-http-disk-cache"sv,
            cert_arg.view(),
            "--dns-server"sv,
            "127.0.0.1"sv,
            "--dns-port"sv,
            dns_port_str.view(),
            "--privacy-network-mode=allow"sv,
        };
        auto app = TRY_OR_FAIL(environment.create_application<Milestone0Application>(args));
        auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });

        auto result = run_local_page_subresource_probe(*view, target_url);
        EXPECT_EQ(result, "local-rendered:ok:https-ok"sv);
        EXPECT(dns_observer.query_count() >= 1u);
        EXPECT(https_server.connection_count() >= 1u);
        EXPECT(https_server.request_count() >= 1u);
    }

    dns_observer.reset_counts();
    https_server.reset_counts();

    // Negative control: HTTPS denied under fixed offline policy with zero DNS queries or TLS connections.
    {
        Test::HeadlessTestEnvironment environment { "Milestone0HTTPSDeny"sv, 120 };
        auto ca_path = PrivacyTest::write_test_ca_file(environment.directory());
        auto cert_arg = ByteString::formatted("--certificate={}", ca_path);
        Array<StringView, 8> args {
            "--temporary-profile"sv,
            "--disable-http-disk-cache"sv,
            cert_arg.view(),
            "--dns-server"sv,
            "127.0.0.1"sv,
            "--dns-port"sv,
            dns_port_str.view(),
            "--privacy-network-mode=offline"sv,
        };
        auto app = TRY_OR_FAIL(environment.create_application<Milestone0Application>(args));
        auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });

        auto result = run_local_page_subresource_probe(*view, target_url);
        EXPECT_EQ(result, "local-rendered:fetch-failed"sv);
        EXPECT_EQ(dns_observer.query_count(), 0u);
        EXPECT_EQ(https_server.connection_count(), 0u);
        EXPECT_EQ(https_server.request_count(), 0u);
    }
}
