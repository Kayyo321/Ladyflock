/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Milestone 3: one mandatory proxy route, and SOCKS5 stream isolation per identity and top-level site.
//
// Every test points its destination at a "trap": an independent server on the port that a direct connection would reach
// once the independent DNS observer resolves the destination name to loopback. A routed request reaches neither; only
// the mock SOCKS5 proxy sees it, as a domain-name destination.

#include "HeadlessTest.h"
#include "PrivacyTestFixtures.h"
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
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
#include <LibWebView/IdentityManager.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/Utilities.h>

#include <stdlib.h>

namespace {

class ProxyRouteApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(ProxyRouteApplication)

public:
    explicit ProxyRouteApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions& request_server_options, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        request_server_options.http_disk_cache_mode = WebView::HTTPDiskCacheMode::Disabled;
        // Without test mode, RequestServer resolves names with the configured DNS server, the independent observer.
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }
};

struct FetchResult {
    Optional<Requests::NetworkError> error;
    Optional<u32> status_code;
    ByteString body;
};

static FetchResult fetch(Requests::RequestClient& client, ByteString const& url, Optional<StringView> top_level_site = {})
{
    Optional<HTTP::NetworkIsolationKey> network_isolation_key;
    if (top_level_site.has_value())
        network_isolation_key = HTTP::NetworkIsolationKey { .top_level_site = Utf16String::from_utf8(*top_level_site), .frame_site = Utf16String::from_utf8(*top_level_site) };

    auto request = client.start_request("GET"sv, URL::Parser::basic_parse(url).release_value(), {}, *HTTP::HeaderList::create(), {},
        HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No, Requests::RequestClient::TransferLease::No, {},
        Requests::RequestClient::CacheMissNotification::No, 0, move(network_isolation_key));
    VERIFY(request);

    FetchResult result;
    bool finished = false;
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(15), [&] { timed_out = true; });
    timer->start();
    request->set_buffered_request_finished_callback([&](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& error, HTTP::HeaderList const&, Optional<u32> status_code, Optional<String> const&, Requests::CacheState, Core::ImmutableBytes payload) {
        result.error = error;
        result.status_code = status_code;
        result.body = ByteString::copy(payload.bytes());
        finished = true;
    });
    Core::EventLoop::current().spin_until([&] { return finished || timed_out; });
    timer->stop();
    VERIFY(!timed_out);
    return result;
}

static ByteString run_page_fetch_probe(WebView::HeadlessWebView& view, StringView target_url)
{
    // Each probe's title starts with its own token, so a probe never mistakes the previous page's title for its result.
    static size_t s_probe_count = 0;
    auto token = ByteString::formatted("probe-{}:", ++s_probe_count);

    auto html = ByteString::formatted(R"HTML(<!doctype html>
<div id="local">local-rendered</div>
<script>
const localReady = "{}" + document.getElementById("local").textContent;
fetch("{}")
  .then(r => r.text())
  .then(text => {{ document.title = localReady + ":ok:" + text; }})
  .catch(() => {{ document.title = localReady + ":fetch-failed"; }});
</script>)HTML",
        token, target_url);
    view.load_html(html);

    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(20), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] {
        return timed_out || view.title().to_byte_string().starts_with(token);
    });
    timer->stop();
    VERIFY(!timed_out);
    return view.title().to_byte_string().substring(token.length());
}

static bool spin_until(Function<bool()> condition, AK::Duration timeout = AK::Duration::from_seconds(10))
{
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(timeout, [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return timed_out || condition(); });
    timer->stop();
    return !timed_out;
}

static void kill_request_servers_and_wait_for_replacement()
{
    NonnullRefPtr<Requests::RequestControlClient> old_control = WebView::Application::request_server_control_client();
    WebView::Application::process_manager().for_each_process([](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::RequestServer)
            MUST(Core::System::kill(process.pid(), SIGKILL));
    });
    Core::EventLoop::current().spin_until([&] {
        return &WebView::Application::request_server_control_client() != old_control.ptr();
    });
}

static auto load_theme()
{
    return MUST(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
}

}

TEST_CASE(proxy_route_survives_bypass_lists_redirects_helper_restarts_and_proxy_loss)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::IndependentHTTPServer trap(PrivacyTest::IndependentHTTPServer::Protocol::HTTP);
    PrivacyTest::MockRouteServer proxy(PrivacyTest::MockRouteServer::Mode::SOCKS5);

    // The environment names the trap as the HTTP proxy and exempts the destinations from proxying altogether. The
    // mandatory route must override both: neither the trap nor a direct connection may see any request.
    auto trap_proxy = ByteString::formatted("http://127.0.0.1:{}", trap.port());
    VERIFY(setenv("http_proxy", trap_proxy.characters(), 1) == 0);
    VERIFY(setenv("no_proxy", "fixture.example,excluded.example,localhost,127.0.0.1", 1) == 0);
    ScopeGuard restore_environment = [] {
        unsetenv("http_proxy");
        unsetenv("no_proxy");
    };

    auto dns_port = ByteString::number(dns_observer.port());
    auto route = ByteString::formatted("--privacy-proxy=socks5h://127.0.0.1:{}", proxy.port());
    Test::HeadlessTestEnvironment environment { "Milestone3ProxyRoute"sv, 120 };
    Array arguments {
        "--temporary-profile"sv,
        "--disable-http-disk-cache"sv,
        "--dns-server"sv,
        "127.0.0.1"sv,
        "--dns-port"sv,
        dns_port.view(),
        "--privacy-network-mode=proxy-only"sv,
        route.view(),
    };
    auto app = TRY_OR_FAIL(environment.create_application<ProxyRouteApplication>(arguments));
    auto view = WebView::HeadlessWebView::create(load_theme(), { 800, 600 });

    EXPECT_EQ(WebView::Application::privacy_policy_coordinator().network_mode(WebView::Application::default_session()), Privacy::NetworkMode::ProxyOnly);
    EXPECT(!WebView::Application::privacy_policy_coordinator().allows_dns_resolution(WebView::Application::default_session()));
    EXPECT(!WebView::Application::privacy_policy_coordinator().allows_webrtc_network(WebView::Application::default_session()));

    // A page's subresource goes through the route, with the destination name resolved at the proxy.
    auto target = ByteString::formatted("http://fixture.example:{}/resource", trap.port());
    EXPECT_EQ(run_page_fetch_probe(*view, target), "local-rendered:ok:via-socks:fixture.example"sv);
    auto streams = proxy.streams();
    VERIFY(!streams.is_empty());
    EXPECT_EQ(streams.first().address_type, 3);
    EXPECT_EQ(streams.first().host, "fixture.example"sv);
    EXPECT_EQ(streams.first().port, trap.port());
    // Without isolation, the route sends no credentials of its own.
    EXPECT(streams.first().username.is_empty());

    // A redirect to a host that the bypass list names still takes the route.
    auto redirect = ByteString::formatted("http://fixture.example:{}/redirect?to=http://excluded.example:{}/final", trap.port(), trap.port());
    EXPECT_EQ(run_page_fetch_probe(*view, redirect), "local-rendered:ok:via-socks:excluded.example"sv);
    EXPECT(any_of(proxy.streams(), [](auto const& stream) { return stream.host == "excluded.example"sv && stream.address_type == 3; }));

    // A loopback name that the bypass list exempts takes the route too.
    auto browser_instance = TRY_OR_FAIL(WebView::Application::request_server_manager().browser_instance(WebView::Application::default_session()));
    auto loopback = fetch(browser_instance->ui_client(), ByteString::formatted("http://localhost:{}/loopback", trap.port()));
    EXPECT(!loopback.error.has_value());
    EXPECT_EQ(loopback.body, "via-socks:localhost"sv);

    EXPECT_EQ(dns_observer.query_count(), 0u);
    EXPECT_EQ(trap.connection_count(), 0u);

    // A RequestServer launched after a crash receives the route with its first policy, before any client connects.
    kill_request_servers_and_wait_for_replacement();
    proxy.reset_counts();
    EXPECT_EQ(run_page_fetch_probe(*view, target), "local-rendered:ok:via-socks:fixture.example"sv);
    EXPECT(proxy.connection_count() >= 1u);

    // So does a newly launched RequestServer of its own for a site.
    auto site_instance = TRY_OR_FAIL(WebView::Application::request_server_manager().instance_for_site(WebView::Application::default_session(), "http://fixture.example"_utf16));
    auto from_site_instance = fetch(site_instance->ui_client(), target, "http://fixture.example"sv);
    EXPECT(!from_site_instance.error.has_value());
    EXPECT_EQ(from_site_instance.body, "via-socks:fixture.example"sv);

    EXPECT_EQ(dns_observer.query_count(), 0u);
    EXPECT_EQ(trap.connection_count(), 0u);

    // When the proxy goes away, requests fail visibly instead of connecting directly.
    proxy.stop();
    EXPECT_EQ(run_page_fetch_probe(*view, target), "local-rendered:fetch-failed"sv);
    auto after_loss = fetch(browser_instance->ui_client(), target);
    EXPECT(after_loss.error.has_value());
    auto redirect_after_loss = fetch(browser_instance->ui_client(), redirect);
    EXPECT(redirect_after_loss.error.has_value());

    EXPECT_EQ(dns_observer.query_count(), 0u);
    EXPECT_EQ(trap.connection_count(), 0u);
    EXPECT_EQ(trap.request_count(), 0u);
}

TEST_CASE(switching_to_the_route_closes_pooled_direct_connections)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::MockRouteServer origin(PrivacyTest::MockRouteServer::Mode::DirectHTTP);
    PrivacyTest::MockRouteServer proxy(PrivacyTest::MockRouteServer::Mode::SOCKS5);

    auto dns_port = ByteString::number(dns_observer.port());
    auto route = ByteString::formatted("--privacy-proxy=socks5h://127.0.0.1:{}", proxy.port());
    Test::HeadlessTestEnvironment environment { "Milestone3PooledConnection"sv, 120 };
    Array arguments {
        "--temporary-profile"sv,
        "--disable-http-disk-cache"sv,
        "--dns-server"sv,
        "127.0.0.1"sv,
        "--dns-port"sv,
        dns_port.view(),
        "--privacy-network-mode=allow"sv,
        route.view(),
    };
    auto app = TRY_OR_FAIL(environment.create_application<ProxyRouteApplication>(arguments));
    auto& session = WebView::Application::default_session();
    auto instance = TRY_OR_FAIL(WebView::Application::request_server_manager().browser_instance(session));

    // Positive control: with direct egress, two requests resolve the name and share one keep-alive connection.
    auto url = ByteString::formatted("http://fixture.example:{}/pooled", origin.port());
    auto first = fetch(instance->ui_client(), url);
    auto second = fetch(instance->ui_client(), url);
    EXPECT(!first.error.has_value());
    EXPECT_EQ(first.body, "via-direct:direct"sv);
    EXPECT_EQ(second.body, "via-direct:direct"sv);
    EXPECT_EQ(origin.connection_count(), 1u);
    EXPECT_EQ(origin.client_closed_count(), 0u);
    EXPECT(dns_observer.query_count() >= 1u);
    auto queries_before_switch = dns_observer.query_count();

    auto transition = WebView::Application::privacy_policy_coordinator().set_network_mode(session, Privacy::NetworkMode::ProxyOnly);
    EXPECT(transition.committed);
    EXPECT_EQ(WebView::Application::privacy_policy_coordinator().network_mode(session), Privacy::NetworkMode::ProxyOnly);

    // The switch closes the pooled direct connection rather than leaving it for the next request to reuse.
    EXPECT(spin_until([&] { return origin.client_closed_count() >= 1; }));

    auto routed = fetch(instance->ui_client(), url);
    EXPECT(!routed.error.has_value());
    EXPECT_EQ(routed.body, "via-socks:fixture.example"sv);
    EXPECT_EQ(origin.connection_count(), 1u);
    EXPECT_EQ(origin.request_count(), 2u);
    EXPECT_EQ(dns_observer.query_count(), queries_before_switch);
}

TEST_CASE(stream_isolation_separates_sites_identities_and_generations)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::IndependentHTTPServer trap(PrivacyTest::IndependentHTTPServer::Protocol::HTTP);
    PrivacyTest::MockRouteServer proxy(PrivacyTest::MockRouteServer::Mode::SOCKS5);
    PrivacyTest::MockRouteServer tor(PrivacyTest::MockRouteServer::Mode::SOCKS5);

    auto dns_port = ByteString::number(dns_observer.port());
    auto route = ByteString::formatted("--privacy-proxy=socks5h://127.0.0.1:{}", proxy.port());
    auto tor_route = ByteString::formatted("--privacy-tor-proxy=socks5h://127.0.0.1:{}", tor.port());
    Test::HeadlessTestEnvironment environment { "Milestone3StreamIsolation"sv, 120 };
    Array arguments {
        "--temporary-profile"sv,
        "--disable-http-disk-cache"sv,
        "--dns-server"sv,
        "127.0.0.1"sv,
        "--dns-port"sv,
        dns_port.view(),
        "--privacy-network-mode=proxy-only"sv,
        route.view(),
        "--privacy-proxy-isolation"sv,
        tor_route.view(),
    };
    auto app = TRY_OR_FAIL(environment.create_application<ProxyRouteApplication>(arguments));
    auto& session = WebView::Application::default_session();
    auto instance = TRY_OR_FAIL(WebView::Application::request_server_manager().browser_instance(session));
    auto url = ByteString::formatted("http://fixture.example:{}/isolated", trap.port());

    auto credentials_of_last_stream = [](PrivacyTest::MockRouteServer& server) {
        auto streams = server.streams();
        VERIFY(!streams.is_empty());
        return streams.last();
    };

    // Two top-level sites of one identity: same identity username, different site passwords, separate streams.
    EXPECT_EQ(fetch(instance->ui_client(), url, "http://a.example"sv).body, "via-socks:fixture.example"sv);
    auto site_a = credentials_of_last_stream(proxy);
    EXPECT_EQ(fetch(instance->ui_client(), url, "http://b.example"sv).body, "via-socks:fixture.example"sv);
    auto site_b = credentials_of_last_stream(proxy);
    EXPECT_EQ(proxy.streams().size(), 2u);
    EXPECT_EQ(site_a.username.length(), 32u);
    EXPECT_EQ(site_a.username, site_b.username);
    EXPECT_NE(site_a.password, site_b.password);
    EXPECT(!site_a.password.contains("example"sv));

    // Browser-initiated work has a stream of its own.
    EXPECT_EQ(fetch(instance->ui_client(), url).body, "via-socks:fixture.example"sv);
    auto browser = credentials_of_last_stream(proxy);
    EXPECT_EQ(browser.username, site_a.username);
    EXPECT_NE(browser.password, site_a.password);
    EXPECT_NE(browser.password, site_b.password);

    // Every stream that site A uses carries site A's credentials, whether its connection is reused or new.
    EXPECT_EQ(fetch(instance->ui_client(), url, "http://a.example"sv).body, "via-socks:fixture.example"sv);
    for (auto const& stream : proxy.streams()) {
        EXPECT_EQ(stream.username, site_a.username);
        EXPECT(stream.password == site_a.password || stream.password == site_b.password || stream.password == browser.password);
    }

    // Another identity on the same route has its own username, even for the same site.
    auto work_session = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string)));
    auto work_instance = TRY_OR_FAIL(WebView::Application::request_server_manager().browser_instance(*work_session));
    EXPECT_EQ(fetch(work_instance->ui_client(), url, "http://a.example"sv).body, "via-socks:fixture.example"sv);
    auto work_a = credentials_of_last_stream(proxy);
    EXPECT_NE(work_a.username, site_a.username);
    EXPECT_NE(work_a.password, site_a.password);

    // A Tor-routed identity takes the Tor endpoint, never the browser's proxy, with isolated credentials of its own.
    auto streams_before_tor = proxy.streams().size();
    auto tor_session = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(Privacy::IdentityDescriptor::tor_routed("tor-reader"_string, "Tor Reader"_string)));
    EXPECT_EQ(WebView::Application::privacy_policy_coordinator().network_mode(*tor_session), Privacy::NetworkMode::ProxyOnly);
    auto tor_instance = TRY_OR_FAIL(WebView::Application::request_server_manager().browser_instance(*tor_session));
    EXPECT_EQ(fetch(tor_instance->ui_client(), url, "http://a.example"sv).body, "via-socks:fixture.example"sv);
    auto tor_a = credentials_of_last_stream(tor);
    EXPECT_EQ(proxy.streams().size(), streams_before_tor);
    EXPECT_EQ(tor_a.username.length(), 32u);
    EXPECT_NE(tor_a.username, site_a.username);
    EXPECT_NE(tor_a.username, work_a.username);

    // Renewal gives site A new credentials and closes the identity's existing connections.
    auto closed_before_renewal = proxy.client_closed_count();
    auto renewal = WebView::Application::privacy_policy_coordinator().renew_route_isolation(session);
    EXPECT(renewal.committed);
    EXPECT(spin_until([&] { return proxy.client_closed_count() > closed_before_renewal; }));
    EXPECT_EQ(fetch(instance->ui_client(), url, "http://a.example"sv).body, "via-socks:fixture.example"sv);
    auto renewed_a = credentials_of_last_stream(proxy);
    EXPECT_NE(renewed_a.username, site_a.username);
    EXPECT_NE(renewed_a.password, site_a.password);

    EXPECT_EQ(dns_observer.query_count(), 0u);
    EXPECT_EQ(trap.connection_count(), 0u);
}

TEST_CASE(a_tor_routed_identity_without_a_tor_endpoint_has_no_network)
{
    PrivacyTest::IndependentDNSObserver dns_observer;
    PrivacyTest::IndependentHTTPServer destination(PrivacyTest::IndependentHTTPServer::Protocol::HTTP);

    auto dns_port = ByteString::number(dns_observer.port());
    Test::HeadlessTestEnvironment environment { "Milestone3TorWithoutEndpoint"sv, 120 };
    Array arguments {
        "--temporary-profile"sv,
        "--disable-http-disk-cache"sv,
        "--dns-server"sv,
        "127.0.0.1"sv,
        "--dns-port"sv,
        dns_port.view(),
        "--privacy-network-mode=allow"sv,
    };
    auto app = TRY_OR_FAIL(environment.create_application<ProxyRouteApplication>(arguments));
    auto url = ByteString::formatted("http://fixture.example:{}/tor", destination.port());

    // Positive control: the default identity connects directly.
    auto default_instance = TRY_OR_FAIL(WebView::Application::request_server_manager().browser_instance(WebView::Application::default_session()));
    EXPECT(!fetch(default_instance->ui_client(), url).error.has_value());
    EXPECT(destination.connection_count() >= 1u);
    dns_observer.reset_counts();
    destination.reset_counts();

    auto tor_session = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(Privacy::IdentityDescriptor::tor_routed("tor-reader"_string, "Tor Reader"_string)));
    auto& coordinator = WebView::Application::privacy_policy_coordinator();
    EXPECT(!coordinator.allows_network_egress(*tor_session));
    EXPECT(!coordinator.allows_dns_resolution(*tor_session));

    auto tor_instance = TRY_OR_FAIL(WebView::Application::request_server_manager().browser_instance(*tor_session));
    auto denied = fetch(tor_instance->ui_client(), url);
    EXPECT(denied.error == Requests::NetworkError::BlockedByPrivacyPolicy);

    // Proxy-only mode without a route is refused, and the identity's policy stays as it was.
    auto refused = coordinator.set_network_mode(WebView::Application::default_session(), Privacy::NetworkMode::ProxyOnly);
    EXPECT(!refused.committed);
    EXPECT_EQ(refused.reason, Privacy::PolicyReason::MandatoryRouteUnavailable);
    EXPECT_EQ(coordinator.network_mode(WebView::Application::default_session()), Privacy::NetworkMode::Allow);

    EXPECT_EQ(dns_observer.query_count(), 0u);
    EXPECT_EQ(destination.connection_count(), 0u);
}
