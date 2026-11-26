/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Milestone 4 site protections. Each control has a positive control (off, or a permitted site), a denied case and
// compatibility fixtures, observed from an independent server rather than from the page's own script.

#include "HeadlessTest.h"
#include "PrivacyTestFixtures.h"
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/PrivacyActivity.h>
#include <LibWebView/Settings.h>
#include <LibWebView/Utilities.h>

namespace {

class SiteControlsApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(SiteControlsApplication)

public:
    explicit SiteControlsApplication(Optional<ByteString> ladybird_binary_path)
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

static void spin_for(AK::Duration duration)
{
    bool done = false;
    auto timer = Core::Timer::create_single_shot(duration, [&] { done = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return done; });
}

static auto load_theme()
{
    return MUST(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
}

// Loads the URL as the browser UI would, and returns the request target that the server received for it.
static ByteString navigate_and_observe(WebView::HeadlessWebView& view, PrivacyTest::MockRouteServer& server, ByteString const& url, StringView path)
{
    auto seen_before = server.request_targets().size();
    view.load_from_browser_ui(URL::Parser::basic_parse(url).release_value());
    ByteString target;
    VERIFY(spin_until([&] {
        auto targets = server.request_targets();
        for (size_t i = seen_before; i < targets.size(); ++i) {
            if (targets[i].starts_with(path)) {
                target = targets[i];
                return true;
            }
        }
        return false;
    }));
    return target;
}

static bool spin_until_view_url(WebView::HeadlessWebView& view, StringView expected)
{
    return spin_until([&] { return view.url().serialize().to_byte_string() == expected; });
}

}

TEST_CASE(tracking_parameters_reach_the_server_when_the_control_is_off)
{
    PrivacyTest::MockRouteServer origin(PrivacyTest::MockRouteServer::Mode::DirectHTTP);
    Test::HeadlessTestEnvironment environment { "Milestone4TrackingOff"sv, 120 };
    Array arguments { "--temporary-profile"sv };
    auto app = TRY_OR_FAIL(environment.create_application<SiteControlsApplication>(arguments));
    auto view = WebView::HeadlessWebView::create(load_theme(), { 800, 600 });

    auto url = ByteString::formatted("http://127.0.0.1:{}/page?utm_source=news&id=1&fbclid=abc", origin.port());
    EXPECT_EQ(navigate_and_observe(*view, origin, url, "/page"sv), "/page?utm_source=news&id=1&fbclid=abc"sv);
    EXPECT_EQ(WebView::Application::privacy_activity().counters_for(WebView::Application::default_session()).navigations_sanitized, 0u);
}

TEST_CASE(tracking_parameters_come_off_top_level_navigations_before_they_are_sent)
{
    PrivacyTest::MockRouteServer origin(PrivacyTest::MockRouteServer::Mode::DirectHTTP);
    auto base = ByteString::formatted("http://127.0.0.1:{}", origin.port());

    // A page whose link carries tracking parameters and a hyperlink-auditing ping, and which follows it itself.
    origin.set_html_for_path("/links"sv, ByteString::formatted(R"HTML(<!doctype html>
<a id="link" href="{}/target?utm_campaign=spring&id=7#details" ping="{}/ping">go</a>
<script>document.getElementById("link").click();</script>)HTML",
                                             base, base));

    Test::HeadlessTestEnvironment environment { "Milestone4TrackingOn"sv, 120 };
    Array arguments { "--temporary-profile"sv, "--privacy-strip-tracking-parameters"sv };
    auto app = TRY_OR_FAIL(environment.create_application<SiteControlsApplication>(arguments));
    auto view = WebView::HeadlessWebView::create(load_theme(), { 800, 600 });
    auto& activity = WebView::Application::privacy_activity();
    auto& session = WebView::Application::default_session();

    // Denied: the tags never leave the browser, and the page's own parameter and the fragment stay.
    EXPECT_EQ(navigate_and_observe(*view, origin, ByteString::formatted("{}/page?utm_source=news&id=1&fbclid=abc#top", base), "/page"sv), "/page?id=1"sv);
    EXPECT(spin_until_view_url(*view, ByteString::formatted("{}/page?id=1#top", base)));

    // A query of nothing but tracking parameters goes away entirely.
    EXPECT_EQ(navigate_and_observe(*view, origin, ByteString::formatted("{}/only?gclid=xyz", base), "/only"sv), "/only"sv);

    // Permitted: a sign-in callback has nothing to strip and arrives exactly as written.
    EXPECT_EQ(navigate_and_observe(*view, origin, ByteString::formatted("{}/callback?code=4%2F0Ab_x&state=s%3D1%26t%3D2", base), "/callback"sv), "/callback?code=4%2F0Ab_x&state=s%3D1%26t%3D2"sv);

    // Compatibility: a signed URL keeps every signed parameter byte for byte; names that only look alike stay too.
    EXPECT_EQ(navigate_and_observe(*view, origin, ByteString::formatted("{}/signed?X-Amz-Signature=a%2Bb%3D&utm_campaign=z&X-Amz-Expires=60&utmost=1", base), "/signed"sv),
        "/signed?X-Amz-Signature=a%2Bb%3D&X-Amz-Expires=60&utmost=1"sv);

    // A navigation that the page starts itself is cleaned the same way, and no hyperlink-auditing ping is sent.
    EXPECT_EQ(navigate_and_observe(*view, origin, ByteString::formatted("{}/links", base), "/target"sv), "/target?id=7"sv);
    EXPECT(spin_until_view_url(*view, ByteString::formatted("{}/target?id=7#details", base)));
    spin_for(AK::Duration::from_milliseconds(300));
    EXPECT(!any_of(origin.request_targets(), [](auto const& target) { return target.starts_with("/ping"sv); }));

    // Stated limit: a redirect that the server sends is followed as it is.
    auto redirect = ByteString::formatted("{}/redirect?to={}/landing?utm_source=server&x=1", base, base);
    EXPECT_EQ(navigate_and_observe(*view, origin, redirect, "/landing"sv), "/landing?utm_source=server&x=1"sv);

    auto counters = activity.counters_for(session);
    EXPECT_EQ(counters.navigations_sanitized, 4u);
    EXPECT_EQ(counters.tracking_parameters_removed, 5u);
}

namespace {

// A page whose script, inline script and event handler each fetch a marker (the "images" are text, so onerror fires), and
// whose <noscript> image only loads when script does not run. /always-pixel loads either way.
static ByteString scripted_page(StringView frame_url)
{
    return ByteString::formatted(R"HTML(<!doctype html>
<script src="/script.js"></script>
<script>new Image().src = "/inline-beacon";</script>
<noscript><img src="/noscript-pixel"></noscript>
<img src="/always-pixel" onerror="new Image().src = '/handler-beacon'">
<iframe src="{}"></iframe>)HTML",
        frame_url);
}

static constexpr auto scripted_frame = R"HTML(<!doctype html>
<script>new Image().src = "/frame-beacon";</script>
<img src="/frame-pixel">)HTML"sv;

static bool requested(PrivacyTest::MockRouteServer& server, StringView path)
{
    return any_of(server.request_targets(), [&](auto const& target) { return target.starts_with(path); });
}

// Loads the page and waits until both it and its frame have loaded their images, then a little longer for any script.
static void load_and_settle(WebView::HeadlessWebView& view, ByteString const& url, PrivacyTest::MockRouteServer& top, PrivacyTest::MockRouteServer& frame)
{
    view.load_from_browser_ui(URL::Parser::basic_parse(url).release_value());
    VERIFY(spin_until([&] { return requested(top, "/always-pixel"sv) && requested(frame, "/frame-pixel"sv); }));
    spin_for(AK::Duration::from_milliseconds(500));
}

}

TEST_CASE(scripting_can_be_turned_off_for_a_site_and_its_frames)
{
    // Two origins, each standing in for its own site: 127.0.0.1 and localhost.
    PrivacyTest::MockRouteServer first(PrivacyTest::MockRouteServer::Mode::DirectHTTP);
    PrivacyTest::MockRouteServer second(PrivacyTest::MockRouteServer::Mode::DirectHTTP);
    auto first_base = ByteString::formatted("http://127.0.0.1:{}", first.port());
    auto second_base = ByteString::formatted("http://localhost:{}", second.port());
    first.set_html_for_path("/page"sv, scripted_page(ByteString::formatted("{}/frame", second_base)));
    first.set_html_for_path("/frame"sv, scripted_frame);
    second.set_html_for_path("/page"sv, scripted_page(ByteString::formatted("{}/frame", first_base)));
    second.set_html_for_path("/frame"sv, scripted_frame);

    Test::HeadlessTestEnvironment environment { "Milestone4SiteScripting"sv, 120 };
    Array arguments { "--temporary-profile"sv };
    auto app = TRY_OR_FAIL(environment.create_application<SiteControlsApplication>(arguments));
    auto view = WebView::HeadlessWebView::create(load_theme(), { 800, 600 });
    auto& settings = WebView::Application::settings();

    // Permitted: with no site turned off, every script runs and the <noscript> image is never fetched.
    load_and_settle(*view, ByteString::formatted("{}/page", first_base), first, second);
    EXPECT(requested(first, "/script.js"sv));
    EXPECT(requested(first, "/inline-beacon"sv));
    EXPECT(requested(first, "/handler-beacon"sv));
    EXPECT(!requested(first, "/noscript-pixel"sv));
    EXPECT(requested(second, "/frame-beacon"sv));

    // Denied: with 127.0.0.1 turned off, its page fetches no script, runs no inline script or event handler, and shows
    // its <noscript> content instead. Its frame from another site runs no script either, since the top-level site
    // decides.
    TRY_OR_FAIL(settings.set_scripting_enabled_for_site("127.0.0.1"sv, false));
    EXPECT(!settings.scripting_enabled_for_site("127.0.0.1"sv));
    first.reset_counts();
    second.reset_counts();
    load_and_settle(*view, ByteString::formatted("{}/page?again", first_base), first, second);
    EXPECT(!requested(first, "/script.js"sv));
    EXPECT(!requested(first, "/inline-beacon"sv));
    EXPECT(!requested(first, "/handler-beacon"sv));
    EXPECT(requested(first, "/noscript-pixel"sv));
    EXPECT(!requested(second, "/frame-beacon"sv));
    EXPECT(WebView::Application::privacy_activity().counters_for(WebView::Application::default_session()).navigations_with_scripting_blocked >= 1u);

    // Compatibility: another site is unaffected, including its frame from the turned-off site.
    first.reset_counts();
    second.reset_counts();
    load_and_settle(*view, ByteString::formatted("{}/page", second_base), second, first);
    EXPECT(requested(second, "/script.js"sv));
    EXPECT(requested(second, "/inline-beacon"sv));
    EXPECT(requested(second, "/handler-beacon"sv));
    EXPECT(!requested(second, "/noscript-pixel"sv));
    EXPECT(requested(first, "/frame-beacon"sv));

    // Turning the site back on lets its next document run script again.
    TRY_OR_FAIL(settings.set_scripting_enabled_for_site("127.0.0.1"sv, true));
    first.reset_counts();
    second.reset_counts();
    load_and_settle(*view, ByteString::formatted("{}/page?restored", first_base), first, second);
    EXPECT(requested(first, "/script.js"sv));
    EXPECT(requested(first, "/inline-beacon"sv));
    EXPECT(!requested(first, "/noscript-pixel"sv));
}

// Not an assertion on speed: records what the per-site scripting check costs a page load on this machine, by loading the
// same script-heavy page with no site turned off (the check returns at once) and with an unrelated site turned off (each
// environment looks its top-level site up once per policy).
TEST_CASE(site_scripting_cost)
{
    PrivacyTest::MockRouteServer origin(PrivacyTest::MockRouteServer::Mode::DirectHTTP);
    StringBuilder page;
    page.append("<!doctype html><body>"sv);
    for (size_t i = 0; i < 200; ++i)
        page.appendff("<button onclick=\"void {}\">b</button><script>window.n = (window.n || 0) + 1;</script>", i);
    page.append("<script>document.title = 'done:' + window.n;</script>"sv);
    origin.set_html_for_path("/heavy"sv, page.to_byte_string());

    Test::HeadlessTestEnvironment environment { "Milestone4SiteScriptingCost"sv, 300 };
    Array arguments { "--temporary-profile"sv };
    auto app = TRY_OR_FAIL(environment.create_application<SiteControlsApplication>(arguments));
    auto view = WebView::HeadlessWebView::create(load_theme(), { 800, 600 });
    static constexpr size_t loads = 40;

    auto measure = [&](StringView label) {
        // One warm-up load, then the measured ones.
        for (size_t i = 0; i <= loads; ++i) {
            if (i == 1) {
                auto start = MonotonicTime::now();
                for (size_t j = 0; j < loads; ++j) {
                    view->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/heavy?{}-{}", origin.port(), label, j)).release_value());
                    auto expected_query = ByteString::formatted("{}-{}", label, j);
                    VERIFY(spin_until([&] { return view->title() == "done:200"_utf16 && view->url().query() == expected_query.view(); }));
                }
                auto elapsed = MonotonicTime::now() - start;
                outln("Page with 200 scripts and 200 handlers, {}: {} us per load over {} loads", label, elapsed.to_microseconds() / static_cast<i64>(loads), loads);
                return;
            }
            view->load_from_browser_ui(URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/heavy?warmup-{}", origin.port(), label)).release_value());
            VERIFY(spin_until([&] { return view->title() == "done:200"_utf16; }));
        }
    };

    // A, B, A: the second run without the check tells drift from cost.
    measure("no-sites-off"sv);
    TRY_OR_FAIL(WebView::Application::settings().set_scripting_enabled_for_site("unrelated.example"sv, false));
    measure("unrelated-site-off"sv);
    TRY_OR_FAIL(WebView::Application::settings().set_scripting_enabled_for_site("unrelated.example"sv, true));
    measure("no-sites-off-again"sv);
}
