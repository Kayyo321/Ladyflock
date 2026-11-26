/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Milestone 4: the read-only about:privacy dashboard. Its data is bound to the identity it was opened in, secrets never
// reach it, and a tab capture keeps only hosts and goes away when it expires or its tab closes.

#include "HeadlessTest.h"
#include "PrivacyTestFixtures.h"
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/IdentityManager.h>
#include <LibWebView/PrivacyActivity.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/Utilities.h>

namespace {

class DashboardApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(DashboardApplication)

public:
    explicit DashboardApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions& request_server_options, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        request_server_options.http_disk_cache_mode = WebView::HTTPDiskCacheMode::Disabled;
    }
};

static bool spin_until(Function<bool()> condition, AK::Duration timeout = AK::Duration::from_seconds(15))
{
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(timeout, [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return timed_out || condition(); });
    timer->stop();
    return !timed_out;
}

static auto load_theme()
{
    return MUST(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
}

// Runs script in the dashboard page and returns the text it puts in the title after the token.
static ByteString run_in_dashboard(WebView::HeadlessWebView& dashboard, StringView body)
{
    static size_t s_count = 0;
    auto token = ByteString::formatted("RESULT-{}:", ++s_count);
    dashboard.run_javascript(MUST(String::formatted("(() => {{ const done = text => {{ document.title = '{}' + text; }}; {} }})();", token, body)));
    VERIFY(spin_until([&] { return dashboard.title().to_byte_string().starts_with(token); }));
    return dashboard.title().to_byte_string().substring(token.length());
}

// Sends the dashboard's own backend a message, as the page does, and returns the data of its reply as JSON. Capture
// requests are all answered with the capture's state.
static ByteString dashboard_message(WebView::HeadlessWebView& dashboard, StringView name, StringView data = "undefined"sv)
{
    auto reply = name.is_one_of("startCapture"sv, "stopCapture"sv) ? "loadCapture"sv : name;
    return run_in_dashboard(dashboard, ByteString::formatted(R"(
        const handler = event => {{
            if (event.detail.name !== "{}") return;
            document.removeEventListener("WebUIMessage", handler);
            done(JSON.stringify(event.detail.data));
        }};
        document.addEventListener("WebUIMessage", handler);
        ladybird.sendMessage("{}", {});)",
                                           reply, name, data));
}

static NonnullOwnPtr<WebView::HeadlessWebView> open_dashboard(NonnullRefPtr<WebView::BrowsingSession> session)
{
    auto view = WebView::HeadlessWebView::create_for_session(load_theme(), { 800, 600 }, move(session));
    view->load_from_browser_ui(URL::Parser::basic_parse("about:privacy"sv).release_value());
    VERIFY(spin_until([&] { return view->title() == "Privacy"_utf16; }));
    // Wait until the page has rendered the status it asked for.
    VERIFY(spin_until([&] {
        return run_in_dashboard(*view, "done(String(document.querySelector('#network').children.length));"sv) != "0"sv;
    }));
    return view;
}

}

TEST_CASE(dashboard_shows_its_own_identity_and_never_a_secret)
{
    Test::HeadlessTestEnvironment environment { "Milestone4DashboardRedaction"sv, 120 };
    Array arguments {
        "--temporary-profile"sv,
        "--privacy-network-mode=proxy-only"sv,
        "--privacy-proxy=socks5h://markeruser:markerpassword@127.0.0.1:9"sv,
        "--privacy-tor-proxy=socks5h://127.0.0.1:9150"sv,
    };
    auto app = TRY_OR_FAIL(environment.create_application<DashboardApplication>(arguments));
    auto& coordinator = WebView::Application::privacy_policy_coordinator();

    NonnullRefPtr<WebView::BrowsingSession> default_session = WebView::Application::default_session();
    auto tor_session = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(Privacy::IdentityDescriptor::tor_routed("tor-reader"_string, "Tor Reader"_string)));
    auto tor_secret = coordinator.snapshot_for_session(*tor_session).network.route->isolation_secret;
    VERIFY(!tor_secret.is_empty());

    auto default_dashboard = open_dashboard(default_session);
    auto tor_dashboard = open_dashboard(tor_session);

    // Each dashboard reports its own identity's route, redacted.
    auto default_status = dashboard_message(*default_dashboard, "loadStatus"sv);
    EXPECT(default_status.contains("\"mode\":\"proxy-only\""sv));
    EXPECT(default_status.contains("socks5h://127.0.0.1:9\""sv));
    auto tor_status = dashboard_message(*tor_dashboard, "loadStatus"sv);
    EXPECT(tor_status.contains("\"kind\":\"tor-routed\""sv) || tor_status.contains("Tor Reader"sv));
    EXPECT(tor_status.contains("socks5h://127.0.0.1:9150 (isolated per identity and site)"sv));

    // Neither the route's credentials nor the isolation secret reach the page, as data or as text.
    auto default_text = run_in_dashboard(*default_dashboard, "done(document.body.innerText);"sv);
    auto tor_text = run_in_dashboard(*tor_dashboard, "done(document.body.innerText);"sv);
    EXPECT(default_text.contains("socks5h://127.0.0.1:9"sv));
    for (auto const& output : { default_status, tor_status, default_text, tor_text }) {
        EXPECT(!output.contains("markeruser"sv));
        EXPECT(!output.contains("markerpassword"sv));
        EXPECT(!output.contains(tor_secret));
    }

    // A dashboard lists only its own identity's tabs, and cannot start a capture of another identity's tab.
    auto tor_tab_id = tor_dashboard->view_id();
    auto default_tab_id = default_dashboard->view_id();
    EXPECT(default_status.contains(ByteString::formatted("\"id\":{}", default_tab_id)));
    EXPECT(!default_status.contains(ByteString::formatted("\"id\":{}", tor_tab_id)));
    EXPECT(tor_status.contains(ByteString::formatted("\"id\":{}", tor_tab_id)));
    EXPECT(!tor_status.contains(ByteString::formatted("\"id\":{}", default_tab_id)));

    auto foreign = dashboard_message(*default_dashboard, "startCapture"sv, ByteString::formatted("{{ tabId: {}, seconds: 60 }}", tor_tab_id));
    EXPECT(foreign.contains("\"active\":false"sv));
    EXPECT(!WebView::Application::privacy_activity().capture_for(tor_tab_id, *tor_session).has_value());
}

TEST_CASE(tab_capture_keeps_hosts_only_and_expires)
{
    PrivacyTest::MockRouteServer origin(PrivacyTest::MockRouteServer::Mode::DirectHTTP);
    origin.set_html_for_path("/page"sv, ByteString::formatted("<!doctype html><img src=\"http://127.0.0.1:{}/pixel?q=IPMARKER\">", origin.port()));

    Test::HeadlessTestEnvironment environment { "Milestone4DashboardCapture"sv, 120 };
    Array arguments { "--temporary-profile"sv };
    auto app = TRY_OR_FAIL(environment.create_application<DashboardApplication>(arguments));
    auto& activity = WebView::Application::privacy_activity();
    auto session = NonnullRefPtr<WebView::BrowsingSession> { WebView::Application::default_session() };

    // The capture's clock is the test's, so expiry can be checked without waiting for it.
    auto fake_now = MonotonicTime::now();
    activity.set_clock_for_testing([&] { return fake_now; });

    auto dashboard = open_dashboard(session);
    auto tab = WebView::HeadlessWebView::create_for_session(load_theme(), { 800, 600 }, session);

    // Nothing is captured before the user starts a capture.
    tab->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://localhost:{}/page?before=1", origin.port())).release_value());
    VERIFY(spin_until([&] { return any_of(origin.request_targets(), [](auto const& target) { return target.starts_with("/pixel"sv); }); }));
    EXPECT(dashboard_message(*dashboard, "loadCapture"sv, ByteString::formatted("{{ tabId: {} }}", tab->view_id())).contains("\"active\":false"sv));

    // A capture records the hosts the tab contacts: a name as it is, an address only as the kind of address it is.
    auto started = dashboard_message(*dashboard, "startCapture"sv, ByteString::formatted("{{ tabId: {}, seconds: 10 }}", tab->view_id()));
    EXPECT(started.contains("\"active\":true"sv));
    origin.reset_counts();
    tab->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://localhost:{}/page?secret=QUERYMARKER", origin.port())).release_value());
    VERIFY(spin_until([&] { return any_of(origin.request_targets(), [](auto const& target) { return target.starts_with("/pixel"sv); }); }));

    ByteString capture;
    EXPECT(spin_until([&] {
        capture = dashboard_message(*dashboard, "loadCapture"sv, ByteString::formatted("{{ tabId: {} }}", tab->view_id()));
        return capture.contains("\"host\":\"localhost\""sv) && capture.contains("(IPv4 address)"sv);
    }));
    // Never a path, a query or an address.
    EXPECT(!capture.contains("QUERYMARKER"sv));
    EXPECT(!capture.contains("IPMARKER"sv));
    EXPECT(!capture.contains("/page"sv));
    EXPECT(!capture.contains("127.0.0.1"sv));
    EXPECT(!capture.contains(ByteString::number(origin.port())));
    auto capture_text = run_in_dashboard(*dashboard, "done(document.querySelector('#capture-entries').innerText);"sv);
    EXPECT(!capture_text.contains("QUERYMARKER"sv));

    // Once its time is up, the capture and everything in it is discarded, whether or not the page asks again.
    fake_now = fake_now + AK::Duration::from_seconds(11);
    EXPECT(spin_until([&] { return !activity.capture_for(tab->view_id(), *session).has_value(); }, AK::Duration::from_seconds(5)));
    EXPECT(dashboard_message(*dashboard, "loadCapture"sv, ByteString::formatted("{{ tabId: {} }}", tab->view_id())).contains("\"active\":false"sv));

    // A capture also goes away with its tab.
    OwnPtr<WebView::HeadlessWebView> second_tab = WebView::HeadlessWebView::create_for_session(load_theme(), { 800, 600 }, session);
    auto second_tab_id = second_tab->view_id();
    activity.start_capture(*second_tab, AK::Duration::from_seconds(60));
    EXPECT(activity.capture_for(second_tab_id, *session).has_value());
    second_tab = nullptr;
    EXPECT(!activity.capture_for(second_tab_id, *session).has_value());

    // Captures are bounded.
    auto bounded_tab = WebView::HeadlessWebView::create_for_session(load_theme(), { 800, 600 }, session);
    activity.start_capture(*bounded_tab, AK::Duration::from_seconds(60));
    auto url = URL::Parser::basic_parse("https://tracker.example/x"sv).release_value();
    for (size_t i = 0; i < WebView::PrivacyActivity::MAX_CAPTURE_ENTRIES + 25; ++i)
        activity.record_capture_event(*bounded_tab, url, WebView::PrivacyActivity::CaptureEventKind::Request);
    auto bounded = activity.capture_for(bounded_tab->view_id(), *session);
    VERIFY(bounded.has_value());
    EXPECT_EQ(bounded->entries.size(), WebView::PrivacyActivity::MAX_CAPTURE_ENTRIES);
    EXPECT_EQ(bounded->dropped_entries, 25u);

    activity.set_clock_for_testing({});
}
