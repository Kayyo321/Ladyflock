/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/Mutex.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/Utilities.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

class RuntimeTestApp : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(RuntimeTestApp)

public:
    explicit RuntimeTestApp(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions& request_server_options, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::No;
        browser_options.enable_extensions = true;
        browser_options.enable_content_blocker = WebView::EnableContentBlocker::No;
        request_server_options.http_disk_cache_mode = WebView::HTTPDiskCacheMode::Testing;
    }
};

struct HttpResponseSpec {
    int status_code { 200 };
    ByteString status_text { "OK" };
    ByteString content_type { "text/html; charset=utf-8" };
    ByteString extra_headers {};
    ByteString body {};
};

class LoopbackHttpServer {
public:
    explicit LoopbackHttpServer(Function<HttpResponseSpec(ByteString const& path)> handler)
        : m_handler(move(handler))
    {
        m_listen_fd = MUST(Core::System::socket(AF_INET, SOCK_STREAM, 0));
        int reuse = 1;
        MUST(Core::System::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        MUST(Core::System::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
        MUST(Core::System::listen(m_listen_fd, 32));
        MUST(Core::System::fcntl(m_listen_fd, F_SETFL, O_NONBLOCK));

        socklen_t len = sizeof(addr);
        VERIFY(::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        m_port = ntohs(addr.sin_port);

        m_thread = Threading::Thread::construct("LoopbackHttpServer"sv, [this] -> intptr_t {
            serve();
            return 0;
        });
        m_thread->start();
    }

    ~LoopbackHttpServer()
    {
        m_stop.store(true);
        (void)m_thread->join();
        (void)Core::System::close(m_listen_fd);
    }

    u16 port() const { return m_port; }

    size_t hits(ByteString const& path) const
    {
        AK::MutexLocker locker(m_mutex);
        return m_hits.get(path).value_or(0);
    }

private:
    void serve()
    {
        while (!m_stop.load()) {
            pollfd pfd { .fd = m_listen_fd, .events = POLLIN, .revents = 0 };
            int rc = ::poll(&pfd, 1, 25);
            if (rc <= 0 || (pfd.revents & POLLIN) == 0)
                continue;

            int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
            if (client_fd < 0)
                continue;

            ByteString req_buf;
            char chunk[2048];
            for (;;) {
                pollfd cpfd { .fd = client_fd, .events = POLLIN, .revents = 0 };
                if (::poll(&cpfd, 1, 1000) <= 0)
                    break;
                ssize_t n = ::read(client_fd, chunk, sizeof(chunk));
                if (n <= 0)
                    break;
                req_buf = ByteString::formatted("{}{}", req_buf, StringView { chunk, static_cast<size_t>(n) });
                if (req_buf.contains("\r\n\r\n"sv))
                    break;
            }

            ByteString path = "/"sv;
            if (auto first_eol = req_buf.find("\r\n"sv); first_eol.has_value()) {
                auto line = req_buf.substring_view(0, *first_eol);
                auto parts = line.split_view(' ');
                if (parts.size() >= 2)
                    path = parts[1];
            }

            {
                AK::MutexLocker locker(m_mutex);
                m_hits.set(path, m_hits.get(path).value_or(0) + 1);
            }

            auto spec = m_handler(path);
            StringBuilder resp;
            resp.appendff("HTTP/1.1 {} {}\r\n", spec.status_code, spec.status_text);
            resp.appendff("Content-Type: {}\r\n", spec.content_type);
            resp.appendff("Content-Length: {}\r\n", spec.body.length());
            resp.append("Access-Control-Allow-Origin: *\r\n"sv);
            if (!spec.extra_headers.is_empty())
                resp.append(spec.extra_headers);
            resp.append("Connection: close\r\n\r\n"sv);
            resp.append(spec.body);

            auto raw = resp.string_view().bytes();
            size_t written = 0;
            while (written < raw.size()) {
                ssize_t w = ::write(client_fd, raw.data() + written, raw.size() - written);
                if (w <= 0)
                    break;
                written += static_cast<size_t>(w);
            }
            ::close(client_fd);
        }
    }

    Function<HttpResponseSpec(ByteString const& path)> m_handler;
    int m_listen_fd { -1 };
    u16 m_port { 0 };
    Atomic<bool> m_stop { false };
    RefPtr<Threading::Thread> m_thread;
    mutable AK::Mutex m_mutex;
    HashMap<ByteString, size_t> m_hits;
};

void write_ext_file(ByteString const& dir, StringView rel, StringView content)
{
    auto path = LexicalPath::join(dir, rel).string();
    auto parent = LexicalPath::dirname(path);
    if (!parent.is_empty())
        MUST(Core::Directory::create(parent, Core::Directory::CreateDirectories::Yes));
    auto f = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(f->write_until_depleted(content.bytes()));
}

ErrorOr<Core::AnonymousBuffer> load_default_theme()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    return Gfx::load_system_theme(theme_path.string());
}

}

TEST_CASE(two_origin_content_scripts_revocation_and_private_exclusion)
{
    Test::HeadlessTestEnvironment env { "TestExtContentScripts"sv, 90 };
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, env.directory(), Core::Environment::Overwrite::Yes));
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());

    LoopbackHttpServer server([](ByteString const& path) -> HttpResponseSpec {
        if (path == "/origin-a"sv)
            return { .body = "<!DOCTYPE html><title>Origin A</title><script>window.pageGlobal = 'secret';</script><p id='msg'>A</p>" };
        return { .body = "<!DOCTYPE html><title>Origin B</title><p id='msg'>B</p>" };
    });

    auto ext_dir = LexicalPath::join(env.directory(), "cs-ext"sv).string();
    write_ext_file(ext_dir, "manifest.json"sv, R"({
        "manifest_version": 2,
        "name": "Two Origin Content Script",
        "version": "1.0",
        "browser_specific_settings": { "gecko": { "id": "tworigin@ladyflock.example" } },
        "background": { "scripts": ["bg.js"] },
        "content_scripts": [{
            "matches": ["http://127.0.0.1/*"],
            "run_at": "document_idle",
            "css": ["inject.css"],
            "js": ["content.js"]
        }]
    })"sv);
    write_ext_file(ext_dir, "inject.css"sv, "body { background-color: rgb(12, 34, 56); }"sv);
    write_ext_file(ext_dir, "bg.js"sv, R"(
        browser.runtime.onMessage.addListener(async (value, sender) => {
            const cloned = value.self === value && value.entries.get("count") === 42n;
            return new Map([["cloned", cloned], ["tab", sender.tab.id]]);
        });
    )"sv);
    write_ext_file(ext_dir, "content.js"sv, R"(
        const isolated = (typeof window.pageGlobal === 'undefined') ? 'ISOLATED' : 'LEAKED';
        document.documentElement.setAttribute('data-ladyflock-ext', isolated);
        document.title = 'EXT:' + isolated;
        const message = { entries: new Map([["count", 42n]]) };
        message.self = message;
        browser.runtime.sendMessage(message).then(result => {
            document.title = result instanceof Map && result.get("cloned") && result.get("tab") > 0 ? "CLONED:" + isolated : "MESSAGE:BAD";
        }, () => { document.title = "MESSAGE:ERROR"; });
    )"sv);

    auto& mgr = *app->default_session().extension_manager;
    auto const& ext = TRY_OR_FAIL(mgr.load_unpacked(ext_dir, true));

    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    size_t loads = 0;
    Vector<Utf16String> titles;
    auto wait_for_page = [&](StringView phase, Function<bool()> condition) {
        bool expired = false;
        auto timeout = Core::Timer::create_single_shot(AK::Duration::from_seconds(8), [&] { expired = true; });
        timeout->start();
        Core::EventLoop::current().spin_until([&] { return expired || condition(); });
        timeout->stop();
        if (expired)
            warnln("Timed out waiting for {} ({} loads, last title '{}')", phase, loads, titles.is_empty() ? Utf16String {} : titles.last());
        EXPECT(!expired);
        return !expired;
    };
    view->on_load_finish = [&](auto const&) { ++loads; };
    view->on_title_change = [&](auto const& t) { titles.append(t); };
    if (!wait_for_page("initial document"sv, [&] { return loads >= 1; }))
        return;

    auto url_a = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/origin-a", server.port()))).release_value();
    auto url_b = URL::Parser::basic_parse(MUST(String::formatted("http://localhost:{}/origin-b", server.port()))).release_value();
    auto expect_no_injection = [&](WebView::HeadlessWebView& target, URL::URL const& url, Vector<Utf16String>& target_titles) {
        target_titles.clear();
        target.load_from_browser_ui(url);
        if (!wait_for_page("complete navigation with no matching extension"sv, [&] {
            auto const& document = target.traversable().active_document();
            return document.creation_url() == url && document.is_completely_loaded();
        }))
            return;

        // Probe after document_idle so an early load notification cannot conceal an injection.
        target.run_javascript(R"(
            const injected = document.documentElement.hasAttribute('data-ladyflock-ext')
                || getComputedStyle(document.body).backgroundColor === 'rgb(12, 34, 56)';
            document.title = !document.getElementById('msg') ? 'MISSING FIXTURE'
                : injected ? 'UNEXPECTED EXTENSION' : 'NO EXTENSION';
        )"_string);
        if (wait_for_page("extension DOM probe"sv, [&] {
            return target_titles.contains_slow("NO EXTENSION"_utf16) || target_titles.contains_slow("UNEXPECTED EXTENSION"_utf16)
                || target_titles.contains_slow("MISSING FIXTURE"_utf16);
        }))
            EXPECT(target_titles.contains_slow("NO EXTENSION"_utf16));
    };

    // 1. Matched origin runs content script in isolated world + CSS.
    size_t prev_loads = loads;
    titles.clear();
    view->load_from_browser_ui(url_a);
    if (!wait_for_page("isolated script message"sv, [&] {
        return loads > prev_loads && (titles.contains_slow("CLONED:ISOLATED"_utf16) || titles.contains_slow("CLONED:LEAKED"_utf16)
            || titles.contains_slow("MESSAGE:BAD"_utf16) || titles.contains_slow("MESSAGE:ERROR"_utf16));
    }))
        return;
    EXPECT(titles.contains_slow("CLONED:ISOLATED"_utf16));
    view->run_javascript("document.title = 'CSS:' + getComputedStyle(document.body).backgroundColor + ':PAGE:' + window.pageGlobal;"_string);
    if (!wait_for_page("injected CSS"sv, [&] {
        return titles.contains_slow("CSS:rgb(12, 34, 56):PAGE:secret"_utf16);
    }))
        return;

    // 2. Non-matching second origin does NOT receive content script.
    expect_no_injection(*view, url_b, titles);

    // 3. Private browsing session NEVER attaches extensions.
    auto private_view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::Yes);
    size_t priv_loads = 0;
    Vector<Utf16String> priv_titles;
    private_view->on_load_finish = [&](auto const&) { ++priv_loads; };
    private_view->on_title_change = [&](auto const& t) { priv_titles.append(t); };
    if (!wait_for_page("initial private document"sv, [&] { return priv_loads >= 1; }))
        return;
    expect_no_injection(*private_view, url_a, priv_titles);

    // 4. Live revocation stops injection on subsequent navigation.
    TRY_OR_FAIL(mgr.set_site_access(ext.extension_id, true, "127.0.0.1"_string, false));
    expect_no_injection(*view, url_a, titles);
}

TEST_CASE(mv2_event_page_wakeup_idle_teardown_and_mv3_activetab_scripting)
{
    Test::HeadlessTestEnvironment env { "TestExtMv2Mv3"sv, 90 };
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, env.directory(), Core::Environment::Overwrite::Yes));
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());

    LoopbackHttpServer server([](ByteString const&) -> HttpResponseSpec {
        return { .body = "<!DOCTYPE html><title>Target Page</title><div id='box'>hello</div>" };
    });

    // MV2 Quicknote-style with persistent: false event page + sidebar_action + storage
    auto mv2_dir = LexicalPath::join(env.directory(), "mv2-quicknote"sv).string();
    write_ext_file(mv2_dir, "manifest.json"sv, R"({
        "manifest_version": 2,
        "name": "Quicknote Fixture",
        "version": "1.0",
        "browser_specific_settings": { "gecko": { "id": "quicknote@ladyflock.example" } },
        "permissions": ["storage"],
        "background": { "scripts": ["bg.js"], "persistent": false },
        "browser_action": { "default_title": "Note" },
        "sidebar_action": { "default_panel": "panel.html", "default_title": "Notes", "open_at_install": true }
    })"sv);
    write_ext_file(mv2_dir, "panel.html"sv, "<!DOCTYPE html><body>panel</body>"sv);
    write_ext_file(mv2_dir, "bg.js"sv, R"(
        browser.browserAction.onClicked.addListener(async () => {
            await browser.storage.local.set({ note: 'saved-by-event-page' });
        });
    )"sv);

    // MV3 Beastify-style with activeTab + scripting + web_accessible_resources
    auto mv3_dir = LexicalPath::join(env.directory(), "mv3-beastify"sv).string();
    write_ext_file(mv3_dir, "manifest.json"sv, R"({
        "manifest_version": 3,
        "name": "Beastify Fixture",
        "version": "1.0",
        "browser_specific_settings": { "gecko": { "id": "beastify@ladyflock.example" } },
        "permissions": ["activeTab", "scripting"],
        "background": { "scripts": ["bg.js"] },
        "action": { "default_title": "Beastify" },
        "web_accessible_resources": [{
            "resources": ["beasts/frog.txt"],
            "matches": ["http://127.0.0.1/*"]
        }]
    })"sv);
    write_ext_file(mv3_dir, "beasts/frog.txt"sv, "ribbit"sv);
    write_ext_file(mv3_dir, "secret.txt"sv, "internal-only"sv);
    write_ext_file(mv3_dir, "bg.js"sv, R"(
        chrome.action.onClicked.addListener(async (tab) => {
            await chrome.scripting.insertCSS({
                target: { tabId: tab.id },
                css: "body { border-top: 4px solid rgb(200, 10, 20); }"
            });
            await chrome.scripting.executeScript({
                target: { tabId: tab.id },
                func: () => { document.title = 'MV3_SCRIPTED'; }
            });
        });
    )"sv);

    auto& mgr = *app->default_session().extension_manager;
    mgr.set_event_page_idle_timeout_ms(200);
    auto const& mv2_ext = TRY_OR_FAIL(mgr.load_unpacked(mv2_dir, true));
    auto const& mv3_ext = TRY_OR_FAIL(mgr.load_unpacked(mv3_dir, true));

    // open_at_install opens sidebar controller automatically for MV2 fixture.
    EXPECT(mgr.sidebar_controller().is_open(mv2_ext.extension_id, 1));

    // web_accessible_resources checks.
    auto url_127 = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/page", server.port()))).release_value();
    auto url_local = URL::Parser::basic_parse(MUST(String::formatted("http://localhost:{}/page", server.port()))).release_value();
    auto frog_url = URL::Parser::basic_parse(MUST(String::formatted("moz-extension://{}/beasts/frog.txt", mv3_ext.uuid))).release_value();
    auto secret_url = URL::Parser::basic_parse(MUST(String::formatted("moz-extension://{}/secret.txt", mv3_ext.uuid))).release_value();
    EXPECT(mgr.serve_resource(frog_url, url_127.origin()).allowed);
    EXPECT(!mgr.serve_resource(frog_url, url_local.origin()).allowed);
    EXPECT(!mgr.serve_resource(secret_url, url_127.origin()).allowed);

    // Waking non-persistent MV2 event page on action click stores data and later tears down after idle timeout.
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    size_t loads = 0;
    Vector<Utf16String> titles;
    view->on_load_finish = [&](auto const&) { ++loads; };
    view->on_title_change = [&](auto const& t) { titles.append(t); };
    Core::EventLoop::current().spin_until([&] { return loads >= 1; });

    view->load_from_browser_ui(url_127);
    Core::EventLoop::current().spin_until([&] { return loads >= 2; });

    EXPECT(!mgr.is_background_running(mv2_ext.extension_id));
    EXPECT_EQ(mgr.invoke_action_for_tab(mv2_ext.extension_id, view->view_id(), 1), WebView::Extensions::ExtensionActionController::InvokeOutcome::DispatchedOnClicked);
    Core::EventLoop::current().spin_until([&] {
        return mgr.storage().get(mv2_ext.extension_id, JsonValue {}).get_string("note"sv) == Optional<String> { "saved-by-event-page"_string };
    });
    Core::EventLoop::current().spin_until([&] { return !mgr.is_background_running(mv2_ext.extension_id); });

    // Before MV3 click, activeTab is not granted; clicking grants activeTab and executes scripting API.
    EXPECT(!mgr.has_host_permission(mv3_ext.extension_id, url_127, view->view_id()));
    EXPECT_EQ(mgr.invoke_action_for_tab(mv3_ext.extension_id, view->view_id(), 1), WebView::Extensions::ExtensionActionController::InvokeOutcome::DispatchedOnClicked);
    EXPECT(mgr.has_host_permission(mv3_ext.extension_id, url_127, view->view_id()));
    Core::EventLoop::current().spin_until([&] { return titles.contains_slow("MV3_SCRIPTED"_utf16); });

    // Navigating to a new page clears activeTab grant.
    size_t prev_loads = loads;
    view->load_from_browser_ui(url_local);
    Core::EventLoop::current().spin_until([&] { return loads > prev_loads; });
    EXPECT(!mgr.has_host_permission(mv3_ext.extension_id, url_local, view->view_id()));
}

TEST_CASE(web_request_blocking_nav_fetch_redirect_and_cache)
{
    Test::HeadlessTestEnvironment env { "TestExtWebRequest"sv, 90 };
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, env.directory(), Core::Environment::Overwrite::Yes));
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());

    LoopbackHttpServer server([](ByteString const& path) -> HttpResponseSpec {
        if (path == "/allow"sv) {
            return { .body = R"(<!DOCTYPE html><title>Allowed</title><script>
                window.testBlockedFetch = async (target, label) => {
                    try {
                        await fetch(target);
                        document.title = label + ':ALLOWED';
                    } catch (e) {
                        document.title = label + ':BLOCKED';
                    }
                };
            </script>)" };
        }
        if (path == "/redirect-to-blocked"sv) {
            return {
                .status_code = 302,
                .status_text = "Found",
                .extra_headers = "Location: /blocked-redirect\r\n",
                .body = "redirecting",
            };
        }
        if (path == "/blocked-cached"sv) {
            return {
                .extra_headers = "Cache-Control: public, max-age=3600\r\n",
                .body = "<!DOCTYPE html><title>Cached Body</title>",
            };
        }
        return { .body = "<!DOCTYPE html><title>Blocked Target Reached</title>" };
    });

    auto& mgr = *app->default_session().extension_manager;

    // Pre-warm disk cache for /blocked-cached BEFORE loading the blocking extension.
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    size_t loads = 0;
    Vector<Utf16String> titles;
    view->on_load_finish = [&](auto const&) { ++loads; };
    view->on_title_change = [&](auto const& t) { titles.append(t); };
    Core::EventLoop::current().spin_until([&] { return loads >= 1; });

    auto cached_url = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/blocked-cached", server.port()))).release_value();
    view->load_from_browser_ui(cached_url);
    Core::EventLoop::current().spin_until([&] { return loads >= 2; });
    EXPECT_EQ(server.hits("/blocked-cached"sv), 1u);

    // Now load blocking webRequest extension (ContentBlocker is disabled in RuntimeTestApp!).
    auto wr_dir = LexicalPath::join(env.directory(), "wr-ext"sv).string();
    write_ext_file(wr_dir, "manifest.json"sv, R"({
        "manifest_version": 2,
        "name": "WebRequest Blocker",
        "version": "1.0",
        "browser_specific_settings": { "gecko": { "id": "wr-blocker@ladyflock.example" } },
        "permissions": ["webRequest", "webRequestBlocking", "http://127.0.0.1/*"],
        "background": { "scripts": ["bg.js"] }
    })"sv);
    write_ext_file(wr_dir, "bg.js"sv, R"(
        browser.webRequest.onBeforeRequest.addListener(
            async (details) => {
                if (details.url.includes('/blocked'))
                    return { cancel: true };
                return {};
            },
            { urls: ["http://127.0.0.1/*"] },
            ["blocking"]
        );
        browser.webRequest.onBeforeRequest.addListener(
            () => ({ cancel: true }),
            { urls: ["http://127.0.0.1/only-this-path"] },
            ["blocking"]
        );
        browser.webRequest.onBeforeRequest.addListener(
            () => ({ cancel: true }),
            { urls: ["http://127.0.0.1/*"] }
        );
    )"sv);

    auto const& wr_ext = TRY_OR_FAIL(mgr.load_unpacked(wr_dir, true));
    // Ensure background page has started and registered its onBeforeRequest listener.
    bool bg_ready = false;
    mgr.ensure_background_started(wr_ext.extension_id, [&](bool ready) { bg_ready = ready; });
    Core::EventLoop::current().spin_until([&] {
        return bg_ready && mgr.web_request_coordinator().snapshot().entries.size() == 3;
    });

    // 1. Allowed page loads and hits server; fetch('/blocked-fetch') cancels without hitting server.
    auto allow_url = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/allow", server.port()))).release_value();
    size_t prev_loads = loads;
    view->load_from_browser_ui(allow_url);
    Core::EventLoop::current().spin_until([&] { return loads > prev_loads; });

    titles.clear();
    view->run_javascript("window.testBlockedFetch('/allowed-fetch', 'ALLOWED');"_string);
    Core::EventLoop::current().spin_until([&] {
        return titles.contains_slow("ALLOWED:ALLOWED"_utf16) || titles.contains_slow("ALLOWED:BLOCKED"_utf16);
    });
    EXPECT(titles.contains_slow("ALLOWED:ALLOWED"_utf16));
    EXPECT_EQ(server.hits("/allowed-fetch"sv), 1u);

    titles.clear();
    view->run_javascript("window.testBlockedFetch('/blocked-fetch', 'F1');"_string);
    Core::EventLoop::current().spin_until([&] { return titles.contains_slow("F1:BLOCKED"_utf16); });
    EXPECT_EQ(server.hits("/blocked-fetch"sv), 0u);

    // 2. Redirect target '/blocked-redirect' is cancelled before hitting server.
    titles.clear();
    view->run_javascript("window.testBlockedFetch('/redirect-to-blocked', 'F2');"_string);
    Core::EventLoop::current().spin_until([&] { return titles.contains_slow("F2:BLOCKED"_utf16); });
    EXPECT_EQ(server.hits("/redirect-to-blocked"sv), 1u);
    EXPECT_EQ(server.hits("/blocked-redirect"sv), 0u);

    // 3. Cached URL '/blocked-cached' is cancelled even via RequestServer before cache read.
    titles.clear();
    prev_loads = loads;
    view->load_from_browser_ui(cached_url);
    Core::EventLoop::current().spin_until([&] { return loads > prev_loads || titles.contains_slow("Cached Body"_utf16); });
    EXPECT(!titles.contains_slow("Cached Body"_utf16));
    EXPECT_EQ(server.hits("/blocked-cached"sv), 1u);

    // 4. Top-level navigation to '/blocked-nav' is cancelled before server is ever hit.
    auto blocked_nav_url = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/blocked-nav", server.port()))).release_value();
    prev_loads = loads;
    titles.clear();
    view->load_from_browser_ui(blocked_nav_url);
    Core::EventLoop::current().spin_until([&] { return loads > prev_loads || titles.contains_slow("Blocked Target Reached"_utf16); });
    EXPECT(!titles.contains_slow("Blocked Target Reached"_utf16));
    EXPECT_EQ(server.hits("/blocked-nav"sv), 0u);
}

TEST_CASE(real_signed_amo_toolbar_action)
{
    auto public_xpi = Core::Environment::get("LADYFLOCK_TEST_AMO_SCROLL_TO_TOP_XPI"sv);
    if (!public_xpi.has_value())
        return;

    Test::HeadlessTestEnvironment env { "TestExtSignedAction"sv, 30 };
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, env.directory(), Core::Environment::Overwrite::Yes));
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());
    auto& manager = *app->default_session().extension_manager;
    auto stage = TRY_OR_FAIL(manager.stage_xpi(*public_xpi));
    EXPECT_EQ(stage.sha256_hex, "3641b7d768842222e4b0c8dfc391504e53a5512a6b067406ba110bc3d7f9c97e"_string);
    EXPECT_EQ(stage.compatibility_status, Web::Extensions::CompatibilityStatus::TestedInLadyflock);
    auto extension = TRY_OR_FAIL(manager.approve_install(stage.token));

    LoopbackHttpServer server([](ByteString const&) -> HttpResponseSpec {
        return { .body = R"(<!doctype html><title>Top</title>
            <div style="height: 2000px">Test page</div>
            <script>addEventListener("scroll", () => {
                if (scrollY > 1000) document.title = "Scrolled";
            });</script>)" };
    });
    auto page_url = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/", server.port()))).release_value();
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    bool loaded = false;
    bool scrolled = false;
    view->on_load_finish = [&](URL::URL const& url) { loaded |= url == page_url; };
    view->on_title_change = [&](Utf16String const& title) { scrolled |= title == "Scrolled"_utf16; };
    view->load_from_browser_ui(page_url);
    Core::EventLoop::current().spin_until([&] { return loaded; });

    EXPECT_EQ(manager.invoke_action_for_tab(extension.extension_id, view->view_id(), 1), WebView::Extensions::ExtensionActionController::InvokeOutcome::DispatchedOnClicked);
    Core::EventLoop::current().spin_until([&] { return scrolled; });
    EXPECT(scrolled);
}
