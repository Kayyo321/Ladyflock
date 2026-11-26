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
#include <LibWebView/Settings.h>
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
        if (path == "/child-documents"sv)
            return { .body = "<!DOCTYPE html><title>Child documents</title><iframe id='blank'></iframe><iframe id='inline' srcdoc='<p>Frame</p>'></iframe>" };
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
        }, {
            "matches": ["http://127.0.0.1/child-documents", "file://*/*"],
            "all_frames": true,
            "match_about_blank": true,
            "run_at": "document_idle",
            "js": ["child.js"]
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
    write_ext_file(ext_dir, "child.js"sv, "document.documentElement.dataset.childScript = 'matched';"sv);

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

    auto child_url = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/child-documents", server.port()))).release_value();
    view->load_from_browser_ui(child_url);
    if (!wait_for_page("the document containing blank and inline child frames"sv, [&] {
            auto const& document = view->traversable().active_document();
            return document.creation_url() == child_url && document.is_completely_loaded();
        }))
        return;
    view->run_javascript(R"(
        const getChildMarker = id => document.getElementById(id).contentDocument.documentElement.dataset.childScript;
        const timer = setInterval(() => {
            if (getChildMarker('blank') === 'matched' && getChildMarker('inline') === 'matched') {
                document.title = 'BOTH CHILD SCRIPTS RAN';
                clearInterval(timer);
            }
        }, 25);
    )"_string);
    if (!wait_for_page("content scripts in both same-origin blank children"sv, [&] { return titles.contains_slow("BOTH CHILD SCRIPTS RAN"_utf16); }))
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
        "permissions": ["activeTab", "scripting", "storage"],
        "optional_host_permissions": ["http://localhost/*"],
        "background": { "scripts": ["bg.js"], "type": "module" },
        "action": { "default_title": "Beastify" },
        "web_accessible_resources": [{
            "resources": ["beasts/frog.txt"],
            "matches": ["http://127.0.0.1/*"]
        }]
    })"sv);
    write_ext_file(mv3_dir, "beasts/frog.txt"sv, "ribbit"sv);
    write_ext_file(mv3_dir, "secret.txt"sv, "internal-only"sv);
    write_ext_file(mv3_dir, "signal.mjs"sv, "export default 'MV3_SCRIPTED';"sv);
    write_ext_file(mv3_dir, "bg.js"sv, R"(
        import title from './signal.mjs';

        chrome.action.onClicked.addListener(async (tab) => {
            const alreadyGranted = await browser.permissions.request({ permissions: ['storage'] });
            const ungranted = await browser.permissions.request({ origins: ['http://localhost/*'] });
            await browser.storage.local.set({ alreadyGranted, ungranted });
            await browser.storage.session.set({ title });
            const saved = await browser.storage.session.get('title');
            await chrome.scripting.insertCSS({
                target: { tabId: tab.id },
                css: "body { border-top: 4px solid rgb(200, 10, 20); }"
            });
            await chrome.scripting.executeScript({
                target: { tabId: tab.id },
                func: (title) => { document.title = title; },
                args: [saved.title],
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
    auto permissions_results = mgr.storage().get(mv3_ext.extension_id, JsonValue {});
    EXPECT_EQ(permissions_results.get_bool("alreadyGranted"sv), Optional<bool> { true });
    EXPECT_EQ(permissions_results.get_bool("ungranted"sv), Optional<bool> { false });
    EXPECT(!mgr.has_host_permission(mv3_ext.extension_id, url_local, view->view_id()));

    // Navigating to a new page clears activeTab grant.
    size_t prev_loads = loads;
    view->load_from_browser_ui(url_local);
    Core::EventLoop::current().spin_until([&] { return loads > prev_loads; });
    EXPECT(!mgr.has_host_permission(mv3_ext.extension_id, url_local, view->view_id()));
}

TEST_CASE(registered_scripts_honor_host_access_world_and_session_storage)
{
    Test::HeadlessTestEnvironment env { "TestRegisteredExtensionScripts"sv, 90 };
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, env.directory(), Core::Environment::Overwrite::Yes));
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());
    LoopbackHttpServer server([](ByteString const&) -> HttpResponseSpec {
        return { .body = R"(<!doctype html><title>Page</title>
            <script>
                new MutationObserver(() => {
                    if (globalThis.mainMarker && document.documentElement.dataset.session) {
                        document.title = `${globalThis.mainMarker}:${document.documentElement.dataset.session}:${typeof globalThis.isolatedMarker}`;
                    }
                }).observe(document.documentElement, {attributes: true});
            </script>)" };
    });

    auto extension_directory = LexicalPath::join(env.directory(), "registered-scripts"sv).string();
    write_ext_file(extension_directory, "manifest.json"sv, R"({
        "manifest_version": 3,
        "name": "Registered Script Fixture",
        "version": "1.0",
        "browser_specific_settings": { "gecko": { "id": "registered-scripts@ladyflock.example" } },
        "permissions": ["scripting", "storage"],
        "host_permissions": ["http://127.0.0.1/*"],
        "background": { "scripts": ["bg.js"], "type": "module" },
        "action": {}
    })"sv);
    write_ext_file(extension_directory, "bg.js"sv, R"(
        browser.action.onClicked.addListener(async () => {
            await browser.storage.session.set({ marker: 'session-ok' });
            await browser.scripting.registerContentScripts([
                { id: 'main', matches: ['<all_urls>'], js: ['main.js'], world: 'MAIN' },
                { id: 'isolated', matches: ['<all_urls>'], js: ['isolated.js'], runAt: 'document_start' },
            ]);
            const registered = await browser.scripting.getRegisteredContentScripts({ ids: ['isolated'] });
            await browser.storage.local.set({ installed: registered[0].id });
        });
        browser.runtime.onConnect.addListener(port => {
            port.onMessage.addListener(async message => {
                const { frameId, url, documentId, tab } = port.sender;
                const valid = port.name === 'session-fixture' && frameId === 0
                    && url?.startsWith('http://127.0.0.1:') && typeof documentId === 'string' && tab?.id > 0
                    && message instanceof Set && message.has('request');
                if (valid) await browser.storage.session.setAccessLevel({ accessLevel: 'TRUSTED_AND_UNTRUSTED_CONTEXTS' });
                port.postMessage({ granted: valid });
                port.disconnect();
            });
        });
    )"sv);
    write_ext_file(extension_directory, "main.js"sv, R"(
        globalThis.mainMarker = typeof browser === 'undefined' ? 'main-ok' : 'leak';
        // Either script can finish first. Notify the page's observer when this one is ready, too.
        document.documentElement.dataset.mainScriptReady = 'true';
    )"sv);
    write_ext_file(extension_directory, "isolated.js"sv, R"(
        (async () => {
            globalThis.isolatedMarker = true;
            const denied = browser.storage.session.get('marker').then(() => false, () => true);
            const { granted, disconnected } = await new Promise(resolve => {
                const port = browser.runtime.connect({ name: 'session-fixture' });
                let granted = false;
                port.onMessage.addListener(message => granted = message.granted);
                port.onDisconnect.addListener(() => resolve({ granted, disconnected: true }));
                port.postMessage(new Set(['request']));
            });
            const { marker } = granted ? await browser.storage.session.get('marker') : {};
            if (document.readyState === 'loading') {
                await new Promise(resolve => document.addEventListener('DOMContentLoaded', resolve, { once: true }));
            }
            document.documentElement.dataset.session = (await denied) && granted && disconnected ? marker : 'denial-failed';
        })();
    )"sv);

    auto& manager = *app->default_session().extension_manager;
    auto extension = TRY_OR_FAIL(manager.load_unpacked(extension_directory));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    size_t loads = 0;
    Vector<Utf16String> titles;
    view->on_load_finish = [&](auto const&) { ++loads; };
    view->on_title_change = [&](auto const& title) { titles.append(title); };
    Core::EventLoop::current().spin_until([&] { return loads >= 1; });
    auto load = [&](StringView host) {
        auto url = URL::Parser::basic_parse(MUST(String::formatted("http://{}:{}/page", host, server.port()))).release_value();
        auto previous_loads = loads;
        view->load_from_browser_ui(url);
        Core::EventLoop::current().spin_until([&] { return loads > previous_loads; });
    };
    load("127.0.0.1"sv);
    EXPECT_EQ(manager.invoke_action_for_tab(extension.extension_id, view->view_id(), 1), WebView::Extensions::ExtensionActionController::InvokeOutcome::DispatchedOnClicked);
    Core::EventLoop::current().spin_until([&] {
        return manager.storage().get(extension.extension_id, {}).get_string("installed"sv) == "isolated"sv;
    });
    load("localhost"sv);
    EXPECT(!titles.contains_slow("main-ok:session-ok:undefined"_utf16));
    load("127.0.0.1"sv);
    Core::EventLoop::current().spin_until([&] {
        if (titles.is_empty())
            return false;
        auto title = titles.last().utf16_view();
        return title.starts_with("main-ok:"sv) || title.starts_with("leak:"sv);
    });
    EXPECT_EQ(titles.last(), "main-ok:session-ok:undefined"_utf16);
}

TEST_CASE(mv2_content_script_handle_registers_ordered_sources_and_unregisters)
{
    Test::HeadlessTestEnvironment env { "TestMV2ScriptHandle"sv, 90 };
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, env.directory(), Core::Environment::Overwrite::Yes));
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());
    LoopbackHttpServer server([](ByteString const&) -> HttpResponseSpec {
        return { .body = "<!doctype html><title>Unmodified</title>" };
    });

    auto directory = LexicalPath::join(env.directory(), "mv2-scripts"sv).string();
    write_ext_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2,
        "name": "MV2 Script Handle Fixture",
        "version": "1.0",
        "permissions": ["storage", "http://127.0.0.1/*"],
        "background": { "scripts": ["bg.js"] },
        "browser_action": {}
    })"sv);
    write_ext_file(directory, "file.js"sv, "globalThis.fromFile = 'MV2';"sv);
    write_ext_file(directory, "bg.js"sv, R"(
        let handle;
        browser.browserAction.onClicked.addListener(async () => {
            if (handle) {
                await handle.unregister();
                handle = undefined;
                await browser.storage.local.set({ state: 'unregistered' });
                return;
            }
            let denied = false;
            try {
                await browser.contentScripts.register({
                    matches: ['http://localhost/*'], js: [{ code: 'document.title = "Unexpected";' }],
                });
            } catch { denied = true; }
            handle = await browser.contentScripts.register({
                allFrames: true, matchOriginAsFallback: true, matches: ['http://127.0.0.1/*'],
                js: [{ file: 'file.js' }, { code: 'document.title = `${fromFile}:${typeof browser}`;' }],
                runAt: 'document_end', world: 'MAIN',
            });
            await browser.storage.local.set({ state: denied ? 'registered' : 'host-denial-failed' });
        });
    )"sv);

    auto& manager = *app->default_session().extension_manager;
    auto extension = TRY_OR_FAIL(manager.load_unpacked(directory));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    size_t loads = 0;
    Utf16String title;
    view->on_load_finish = [&](auto const&) { ++loads; };
    view->on_title_change = [&](auto const& new_title) { title = new_title; };
    Core::EventLoop::current().spin_until([&] { return loads >= 1; });
    auto load = [&](StringView query) {
        auto url = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/page?{}", server.port(), query))).release_value();
        auto previous_loads = loads;
        view->load_from_browser_ui(url);
        Core::EventLoop::current().spin_until([&] { return loads > previous_loads; });
    };
    auto toggle_and_wait = [&](StringView state) {
        EXPECT_EQ(manager.invoke_action_for_tab(extension.extension_id, view->view_id(), 1), WebView::Extensions::ExtensionActionController::InvokeOutcome::DispatchedOnClicked);
        Core::EventLoop::current().spin_until([&] { return manager.storage().get(extension.extension_id, {}).get_string("state"sv) == state; });
    };
    load("before"sv);
    toggle_and_wait("registered"sv);
    load("registered"sv);
    Core::EventLoop::current().spin_until([&] { return title == "MV2:undefined"_utf16; });
    toggle_and_wait("unregistered"sv);
    load("unregistered"sv);
    EXPECT_EQ(title, "Unmodified"_utf16);
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
        if (path == "/observe-headers"sv) {
            return {
                .extra_headers = "Cache-Control: public, max-age=3600\r\n"
                                 "Content-Security-Policy: default-src 'none'\r\n"
                                 "Set-Cookie: hidden=1\r\n",
                .body = "observed",
            };
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
        "permissions": ["webRequest", "webRequestBlocking", "storage", "http://127.0.0.1/*"],
        "background": { "scripts": ["bg.js"] },
        "web_accessible_resources": ["guarded.txt"]
    })"sv);
    write_ext_file(wr_dir, "guarded.txt"sv, "guarded body"sv);
    write_ext_file(wr_dir, "bg.js"sv, R"(
        browser.webRequest.onBeforeRequest.addListener(
            async (details) => {
                if (details.url.includes('/blocked'))
                    return { cancel: true };
                if (details.url.endsWith('/extension-redirect'))
                    return { redirectUrl: browser.runtime.getURL('guarded.txt?allow=1') };
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
        browser.webRequest.onBeforeRequest.addListener(
            details => ({ cancel: !details.url.endsWith('?allow=1') }),
            { urls: [browser.runtime.getURL('guarded.txt*')] },
            ['blocking']
        );
        let sent;
        let observed;
        let started;
        browser.webRequest.onSendHeaders.addListener(details => {
            sent = details.requestId;
        }, { urls: ["http://127.0.0.1/observe-headers"] }, ['requestHeaders']);
        browser.webRequest.onHeadersReceived.addListener(details => {
            observed = sent === details.requestId && Array.isArray(details.responseHeaders)
                && !details.responseHeaders.some(header => header.name.toLowerCase() === 'set-cookie');
            const responseHeaders = details.responseHeaders.filter(header =>
                !['content-security-policy', 'cache-control'].includes(header.name.toLowerCase()));
            responseHeaders.push({ name: 'Cache-Control', value: 'no-cache' });
            return { responseHeaders };
        }, { urls: ["http://127.0.0.1/observe-headers"] }, ['blocking', 'responseHeaders']);
        browser.webRequest.onResponseStarted.addListener(details => {
            started = sent === details.requestId && !('responseHeaders' in details);
        }, { urls: ["http://127.0.0.1/observe-headers"] });
        browser.webRequest.onCompleted.addListener(details => {
            browser.storage.local.set({ goodEvents: observed && started && sent === details.requestId });
        }, { urls: ["http://127.0.0.1/observe-headers"] });
    )"sv);

    auto const& wr_ext = TRY_OR_FAIL(mgr.load_unpacked(wr_dir, true));
    // Ensure background page has started and registered its onBeforeRequest listener.
    bool bg_ready = false;
    mgr.ensure_background_started(wr_ext.extension_id, [&](bool ready) { bg_ready = ready; });
    Core::EventLoop::current().spin_until([&] {
        return bg_ready && mgr.web_request_coordinator().snapshot().entries.size() == 8;
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

    titles.clear();
    view->run_javascript(R"(
        fetch('/observe-headers').then(async response => {
            await response.text();
            document.title = response.headers.get('cache-control') + ':' + response.headers.get('content-security-policy');
        });
    )"_string);
    Core::EventLoop::current().spin_until([&] { return !titles.is_empty(); });
    EXPECT(titles.contains_slow("no-cache:default-src 'none'"_utf16));
    Core::EventLoop::current().spin_until([&] {
        return mgr.storage().get(wr_ext.extension_id, {}).has("goodEvents"sv);
    });
    EXPECT_EQ(mgr.storage().get(wr_ext.extension_id, {}).get_bool("goodEvents"sv), true);

    titles.clear();
    view->run_javascript(R"(
        fetch('/extension-redirect').then(response => response.text()).then(text => {
            document.title = text;
        }).catch(() => { document.title = 'redirect-failed'; });
    )"_string);
    Core::EventLoop::current().spin_until([&] { return !titles.is_empty(); });
    EXPECT(titles.contains_slow("guarded body"_utf16));
    EXPECT_EQ(server.hits("/extension-redirect"sv), 0u);

    titles.clear();
    view->run_javascript(MUST(String::formatted(
        "window.testBlockedFetch('moz-extension://{}/guarded.txt', 'WAR')", wr_ext.uuid)));
    Core::EventLoop::current().spin_until([&] { return !titles.is_empty(); });
    EXPECT(titles.contains_slow("WAR:BLOCKED"_utf16));

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

TEST_CASE(web_navigation_reports_active_child_frames_and_document_replacement)
{
    Test::HeadlessTestEnvironment env { "TestWebNavigation"sv, 45 };
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv, "--allow-popups"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());
    LoopbackHttpServer server([](ByteString const& path) -> HttpResponseSpec {
        if (path == "/parent"sv)
            return { .body = "<!doctype html><iframe src='/child'></iframe>" };
        return { .body = "<!doctype html><title>Child</title><script>onmessage = () => window.open('/popup')</script>" };
    });
    auto dir = LexicalPath::join(env.directory(), "navigation-ext"sv).string();
    write_ext_file(dir, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Frame fixture", "version": "1",
        "permissions": ["webNavigation", "storage"],
        "background": { "scripts": ["bg.js"] }
    })"sv);
    write_ext_file(dir, "bg.js"sv, R"(
        browser.webNavigation.onCreatedNavigationTarget.addListener(d => browser.storage.local.set({target:d}));
        browser.webNavigation.onErrorOccurred.addListener(d => browser.storage.local.set({failed:d}));
        const timeline = new Map();
        for (const event of ["onBeforeNavigate", "onCommitted", "onDOMContentLoaded"]) {
            browser.webNavigation[event].addListener(details => {
                const events = timeline.get(details.url) || [];
                events.push(event);
                timeline.set(details.url, events);
            }, {url: [{schemes: ["http"], hostEquals: "127.0.0.1", pathEquals: "/child"}, {pathEquals: "/second"}]});
        }
        browser.webNavigation.onHistoryStateUpdated.addListener(d => browser.storage.local.set({history: d.url}));
        browser.webNavigation.onReferenceFragmentUpdated.addListener(d => browser.storage.local.set({fragment: d.url}));
        browser.webNavigation.onCompleted.addListener(async details => {
            const path = new URL(details.url).pathname;
            if (path !== "/child" && path !== "/second") return;
            const frames = await browser.webNavigation.getAllFrames({tabId: details.tabId});
            const child = await browser.webNavigation.getFrame({tabId: details.tabId, frameId: details.frameId});
            const byDocument = await browser.webNavigation.getFrame({documentId: child.documentId});
            const parent = frames.find(frame => frame.frameId === 0);
            if (parent && byDocument.url === details.url && parent.parentFrameId === -1 && child.parentFrameId === 0 &&
                !child.errorOccurred && child.documentId === details.documentId && child.parentDocumentId === parent.documentId)
                await browser.storage.local.set({[path]: child, [path + "Events"]: timeline.get(details.url).join(",")});
        });
        browser.storage.local.set({ready: true});
    )"sv);
    auto& manager = *app->default_session().extension_manager;
    auto const& ext = TRY_OR_FAIL(manager.load_unpacked(dir, true));
    auto state = [&] { return manager.storage().get(ext.extension_id, JsonValue {}); };
    Core::EventLoop::current().spin_until([&] { return state().get_bool("ready"sv) == true; });
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    auto parent_url = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/parent", server.port()))).release_value();
    view->load_from_browser_ui(parent_url);
    Core::EventLoop::current().spin_until([&] { return state().has("/child"sv); });
    auto first = state();
    view->run_javascript("document.querySelector('iframe').src = '/second';"_string);
    Core::EventLoop::current().spin_until([&] { return state().has("/second"sv); });
    auto second = state();
    EXPECT_EQ(second.get_string("/childEvents"sv), Optional<String> { "onBeforeNavigate,onCommitted,onDOMContentLoaded"_string });
    EXPECT_EQ(second.get_string("/secondEvents"sv), Optional<String> { "onBeforeNavigate,onCommitted,onDOMContentLoaded"_string });
    view->run_javascript("history.pushState({}, '', '/pushed'); location.hash = 'fragment';"_string);
    Core::EventLoop::current().spin_until([&] { return state().has("fragment"sv); });
    auto updated = state();
    auto history_url = updated.get_string("history"sv);
    auto fragment_url = updated.get_string("fragment"sv);
    EXPECT(history_url.has_value() && history_url->bytes_as_string_view().ends_with("/pushed"sv));
    EXPECT(fragment_url.has_value() && fragment_url->bytes_as_string_view().ends_with("/pushed#fragment"sv));
    auto first_frame = first.get_object("/child"sv);
    auto second_frame = second.get_object("/second"sv);
    VERIFY(first_frame.has_value() && second_frame.has_value());
    EXPECT(first_frame->get_i64("frameId"sv).value_or(0) > 0);
    EXPECT_EQ(first_frame->get_i64("frameId"sv), second_frame->get_i64("frameId"sv));
    EXPECT_NE(first_frame->get_string("documentId"sv), second_frame->get_string("documentId"sv));
    EXPECT_EQ(first_frame->get_string("parentDocumentId"sv), second_frame->get_string("parentDocumentId"sv));
    view->run_javascript(R"JS(frames[0].postMessage("open","*"))JS"_string);
    Core::EventLoop::current().spin_until([&] { return state().has("target"sv); });
    auto stored = state();
    auto target = stored.get_object("target"sv);
    VERIFY(target.has_value());
    EXPECT_EQ(target->get_u64("sourceTabId"sv), view->view_id());
    EXPECT_EQ(target->get_i64("sourceFrameId"sv), second_frame->get_i64("frameId"sv));
    EXPECT(target->get_u64("tabId"sv) != view->view_id());
    auto url = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/popup", server.port()))).release_value();
    EXPECT_EQ(target->get_string("url"sv), url.serialize());
    view->load_from_browser_ui(URL::Parser::basic_parse("http://127.0.0.1:1/error"sv).release_value());
    Core::EventLoop::current().spin_until([&] { return state().has("failed"sv); });

}

TEST_CASE(declarative_static_rules_block_before_cache_and_restore_on_disable)
{
    Test::HeadlessTestEnvironment env { "TestExtDeclarativeRules"sv, 90 };
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, env.directory(), Core::Environment::Overwrite::Yes));
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());
    auto wait_for = [&](StringView operation, Function<bool()> condition) {
        bool expired = false;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [&] { expired = true; });
        timer->start();
        Core::EventLoop::current().spin_until([&] { return expired || condition(); });
        timer->stop();
        EXPECT(!expired);
        if (expired)
            warnln("Timed out waiting for {}", operation);
        return !expired;
    };
    LoopbackHttpServer server([](ByteString const& path) -> HttpResponseSpec {
        if (path == "/dnr-redirect"sv)
            return { .status_code = 302, .status_text = "Found", .extra_headers = "Location: /dnr-block-redirect-target\r\n", .body = "redirect" };
        if (path == "/dnr-csp"sv)
            return { .extra_headers = "Content-Security-Policy: object-src 'none'\r\n", .body = "<!DOCTYPE html><title>DNR policy</title><script>document.title = 'DNR inline script'</script>" };
        if (path.starts_with("/dnr-block"sv))
            return { .extra_headers = "Cache-Control: public, max-age=3600\r\n", .body = "<!DOCTYPE html><title>Block target</title>" };
        return { .body = "<!DOCTYPE html><title>Test document</title>" };
    });
    auto url = [&](StringView path) {
        return URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}{}", server.port(), path))).release_value();
    };
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    size_t loads = 0;
    Utf16String title;
    view->on_load_finish = [&](auto const&) { ++loads; };
    view->on_title_change = [&](auto const& value) { title = value; };
    if (!wait_for("the initial tab"sv, [&] { return loads > 0; }))
        return;
    auto navigate = [&](URL::URL const& target) {
        auto expected_load = loads + 1;
        view->load_from_browser_ui(target);
        return wait_for(target.serialize(), [&] { return loads >= expected_load; });
    };
    if (!navigate(url("/dnr-block-cached"sv)))
        return;
    EXPECT_EQ(server.hits("/dnr-block-cached"sv), 1u);

    auto rules_directory = LexicalPath::join(env.directory(), "dnr-extension"sv).string();
    write_ext_file(rules_directory, "manifest.json"sv, R"({
        "manifest_version": 3,
        "name": "Declarative Request Test",
        "version": "1.0",
        "permissions": ["declarativeNetRequest", "storage"],
        "host_permissions": ["http://127.0.0.1/*"],
        "background": { "scripts": ["background.js"] },
        "declarative_net_request": { "rule_resources": [
            { "id": "blocking", "enabled": true, "path": "rules.json" }
        ] }
    })"sv);
    write_ext_file(rules_directory, "rules.json"sv, R"([
        { "id": 1, "action": { "type": "block" },
          "condition": { "urlFilter": "/dnr-block", "resourceTypes": ["main_frame", "xmlhttprequest"], "requestDomains": ["127.0.0.1"] } },
        { "id": 2, "action": { "type": "allow" },
          "condition": { "urlFilter": "/dnr-block-exception", "resourceTypes": ["xmlhttprequest"] } },
        { "id": 3, "action": { "type": "redirect", "redirect": { "transform": { "queryTransform": { "removeParams": ["tracking"] } } } },
          "condition": { "urlFilter": "/dnr-strip?", "resourceTypes": ["xmlhttprequest"] } },
        { "id": 4, "action": { "type": "modifyHeaders", "responseHeaders": [{ "header": "content-security-policy", "operation": "append", "value": "script-src 'none'" }] },
          "condition": { "urlFilter": "/dnr-csp", "resourceTypes": ["main_frame"] } },
        { "id": 5, "action": { "type": "block" },
          "condition": { "urlFilter": "/dnr-socket", "resourceTypes": ["websocket"] } }
    ])"sv);
    write_ext_file(rules_directory, "background.js"sv, R"(
        browser.runtime.onInstalled.addListener(async () => {
            const api = browser.declarativeNetRequest;
            try {
                await api.updateEnabledRulesets({ disableRulesetIds: ['blocking'], enableRulesetIds: ['missing'] });
                await browser.storage.local.set({ rulesets: 'unexpected success' });
            } catch {
                const enabled = await api.getEnabledRulesets();
                await api.updateEnabledRulesets({ disableRulesetIds: ['blocking'] });
                const disabled = await api.getEnabledRulesets();
                await api.updateEnabledRulesets({ enableRulesetIds: ['blocking'] });
                await api.updateDynamicRules({ addRules: [{ id: 1, action: { type: 'block' }, condition: { urlFilter: '/dnr-dynamic' } }] });
                await api.updateSessionRules({ addRules: [{ id: 1, action: { type: 'allow' }, condition: { urlFilter: '/dnr-dynamic-allowed' } }] });
                const dynamicRules = await api.getDynamicRules({ ruleIds: [1] });
                const sessionRules = await api.getSessionRules();
                await browser.storage.local.set({
                    rulesets: [enabled, disabled, await api.getEnabledRulesets()],
                    mutableRules: [dynamicRules.length, sessionRules.length],
                });
            }
        });
    )"sv);
    auto& manager = *app->default_session().extension_manager;
    auto extension = TRY_OR_FAIL(manager.load_unpacked(rules_directory));
    auto read_ruleset_result = [&]() -> Optional<String> {
        auto values = manager.storage().get(extension.extension_id, JsonValue("rulesets"_string));
        if (auto result = values.get("rulesets"sv); result.has_value())
            return result->serialized();
        return {};
    };
    if (!wait_for("the background ruleset API result"sv, [&] { return read_ruleset_result().has_value(); }))
        return;
    EXPECT_EQ(*read_ruleset_result(), R"([["blocking"],[],["blocking"]])"sv);
    EXPECT_EQ(manager.storage().get(extension.extension_id, JsonValue("mutableRules"_string)).get("mutableRules"sv)->serialized(), "[1,1]"sv);

    title = {};
    if (!navigate(url("/dnr-block-cached"sv)))
        return;
    EXPECT_NE(title, "Block target"_utf16);
    EXPECT_EQ(server.hits("/dnr-block-cached"sv), 1u);
    if (!navigate(url("/dnr-block-new"sv)))
        return;
    EXPECT_EQ(server.hits("/dnr-block-new"sv), 0u);
    if (!navigate(url("/dnr-test"sv)))
        return;
    auto fetch_and_wait = [&](StringView path, StringView wanted) {
        view->run_javascript(MUST(String::formatted("fetch('{}').then(() => document.title = 'allowed:{}', () => document.title = 'blocked:{}')", path, path, path)));
        auto allowed = Utf16String::formatted("allowed:{}", path);
        auto blocked = Utf16String::formatted("blocked:{}", path);
        if (!wait_for(path, [&] { return title == allowed || title == blocked; }))
            return;
        EXPECT_EQ(title, wanted == "allowed"sv ? allowed : blocked);
    };
    fetch_and_wait("/dnr-block-fetch"sv, "blocked"sv);
    EXPECT_EQ(server.hits("/dnr-block-fetch"sv), 0u);
    fetch_and_wait("/dnr-redirect"sv, "blocked"sv);
    EXPECT_EQ(server.hits("/dnr-redirect"sv), 1u);
    EXPECT_EQ(server.hits("/dnr-block-redirect-target"sv), 0u);
    fetch_and_wait("/dnr-block-exception"sv, "allowed"sv);
    EXPECT_EQ(server.hits("/dnr-block-exception"sv), 1u);
    fetch_and_wait("/dnr-dynamic"sv, "blocked"sv);
    EXPECT_EQ(server.hits("/dnr-dynamic"sv), 0u);
    fetch_and_wait("/dnr-dynamic-allowed"sv, "allowed"sv);
    EXPECT_EQ(server.hits("/dnr-dynamic-allowed"sv), 1u);
    fetch_and_wait("/dnr-strip?tracking=1&keep=2"sv, "allowed"sv);
    EXPECT_EQ(server.hits("/dnr-strip?tracking=1&keep=2"sv), 0u);
    EXPECT_EQ(server.hits("/dnr-strip?keep=2"sv), 1u);
    view->run_javascript("new WebSocket('ws://' + location.host + '/dnr-socket').onerror = () => document.title = 'DNR socket error'"_string);
    if (!wait_for("the blocked WebSocket"sv, [&] { return title == "DNR socket error"_utf16; }))
        return;
    EXPECT_EQ(server.hits("/dnr-socket"sv), 0u);
    if (!navigate(url("/dnr-csp"sv)))
        return;
    EXPECT_EQ(title, "DNR policy"_utf16);

    auto private_view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::Yes);
    size_t private_loads = 0;
    private_view->on_load_finish = [&](auto const&) { ++private_loads; };
    if (!wait_for("the initial private tab"sv, [&] { return private_loads > 0; }))
        return;
    private_view->load_from_browser_ui(url("/dnr-block-private"sv));
    if (!wait_for("the private navigation"sv, [&] { return private_loads > 1; }))
        return;
    EXPECT_EQ(server.hits("/dnr-block-private"sv), 1u);

    TRY_OR_FAIL(manager.set_extension_enabled(extension.extension_id, false));
    fetch_and_wait("/dnr-block-after-disable"sv, "allowed"sv);
    EXPECT_EQ(server.hits("/dnr-block-after-disable"sv), 1u);
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

TEST_CASE(alarms_wake_event_pages_and_unlimited_storage_exceeds_default_quota)
{
    Test::HeadlessTestEnvironment env { "TestExtAlarms"sv, 30 };
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, env.directory(), Core::Environment::Overwrite::Yes));
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());
    auto extension_dir = LexicalPath::join(env.directory(), "alarms-extension"sv).string();
    write_ext_file(extension_dir, "manifest.json"sv, R"({
        "manifest_version": 2,
        "name": "Alarm Fixture",
        "version": "1.0",
        "browser_specific_settings": { "gecko": { "id": "alarms@ladyflock.example" } },
        "permissions": ["alarms", "storage", "unlimitedStorage"],
        "background": { "scripts": ["bg.js"], "persistent": false },
        "browser_action": { "default_title": "Schedule alarm" }
    })"sv);
    write_ext_file(extension_dir, "bg.js"sv, R"(
        browser.alarms.onAlarm.addListener(async alarm => {
            const cleared = (await browser.alarms.get(alarm.name)) === undefined;
            await browser.storage.local.set({ alarmFired: cleared ? alarm.name : 'still scheduled' });
        });
        browser.browserAction.onClicked.addListener(async () => {
            try {
                await browser.storage.local.set({ overflow: 'b'.repeat(100) });
                await browser.alarms.create('wakeup', { delayInMinutes: 0.015 });
                const alarm = await browser.alarms.get('wakeup');
                const all = await browser.alarms.getAll();
                await browser.storage.local.set({ status: alarm.name === 'wakeup' && all.length === 1 ? 'ready' : 'invalid' });
            } catch (error) {
                await browser.storage.local.set({ status: String(error) });
            }
        });
    )"sv);

    auto& manager = *app->default_session().extension_manager;
    manager.set_event_page_idle_timeout_ms(100);
    auto extension_id = TRY_OR_FAIL(manager.load_unpacked(extension_dir, true)).extension_id;
    JsonObject near_quota;
    near_quota.set("seed"sv, TRY_OR_FAIL(String::repeated('a', WebView::Extensions::ExtensionStorage::default_quota_bytes - 32)));
    (void)TRY_OR_FAIL(manager.storage().set(extension_id, near_quota));

    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    bool loaded = false;
    view->on_load_finish = [&](auto const&) { loaded = true; };
    bool timed_out = false;
    auto timeout = Core::Timer::create_single_shot(AK::Duration::from_seconds(8), [&] { timed_out = true; });
    timeout->start();
    auto& loop = Core::EventLoop::current();
    loop.spin_until([&] { return timed_out || loaded; });
    EXPECT(loaded);
    if (!loaded)
        return;

    EXPECT_EQ(manager.invoke_action_for_tab(extension_id, view->view_id(), 1), WebView::Extensions::ExtensionActionController::InvokeOutcome::DispatchedOnClicked);
    Optional<String> status;
    loop.spin_until([&] {
        auto value = manager.storage().get(extension_id, JsonValue { "status"_string });
        status = value.get_string("status"sv).map([](String const& text) { return String { text }; });
        return timed_out || status.has_value();
    });
    EXPECT_EQ(status, Optional<String> { "ready"_string });
    if (status != "ready"_string)
        return;

    EXPECT(TRY_OR_FAIL(manager.storage().get_bytes_in_use(extension_id)) > WebView::Extensions::ExtensionStorage::default_quota_bytes);
    loop.spin_until([&] { return timed_out || !manager.is_background_running(extension_id); });
    auto fired = [&] { return manager.storage().get(extension_id, JsonValue { "alarmFired"_string }); };
    EXPECT(!fired().has("alarmFired"sv));
    loop.spin_until([&] { return timed_out || fired().has("alarmFired"sv); });
    EXPECT(!timed_out);
    EXPECT_EQ(fired().get_string("alarmFired"sv), Optional<String> { "wakeup"_string });
}

TEST_CASE(extension_privacy_controls_prefetch_only_in_its_profile)
{
    Test::HeadlessTestEnvironment env { "TestExtensionPrivacy"sv, 30 };
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, env.directory(), Core::Environment::Overwrite::Yes));
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());
    auto& manager = *app->default_session().extension_manager;

    LoopbackHttpServer server([](ByteString const& path) -> HttpResponseSpec {
        if (path != "/privacy"sv)
            return { .content_type = "text/plain"sv, .body = "ok"sv };
        return { .body = R"(<!doctype html><title>Privacy test</title>
            <script>
                function issuePrefetch(path) {
                    const link = document.createElement('link');
                    link.rel = 'prefetch';
                    link.href = path;
                    link.onload = () => { document.title = path; };
                    document.head.append(link);
                }
            </script>)" };
    });
    auto url = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/privacy", server.port()))).release_value();
    auto normal_view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    auto private_view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::Yes);
    bool normal_loaded = false;
    bool private_loaded = false;
    Utf16String normal_title;
    Utf16String private_title;
    normal_view->on_load_finish = [&](auto const& loaded_url) { normal_loaded |= loaded_url == url; };
    private_view->on_load_finish = [&](auto const& loaded_url) { private_loaded |= loaded_url == url; };
    normal_view->on_title_change = [&](auto const& title) { normal_title = title; };
    private_view->on_title_change = [&](auto const& title) { private_title = title; };
    normal_view->load_from_browser_ui(url);
    private_view->load_from_browser_ui(url);

    auto& loop = Core::EventLoop::current();
    auto wait_for = [&](Function<bool()> condition) {
        bool timed_out = false;
        auto timeout = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [&] { timed_out = true; });
        timeout->start();
        loop.spin_until([&] { return timed_out || condition(); });
        EXPECT(!timed_out);
        return !timed_out;
    };
    if (!wait_for([&] { return normal_loaded && private_loaded; }))
        return;
    normal_view->run_javascript("issuePrefetch('/normal-before')"_string);
    if (!wait_for([&] { return normal_title == "/normal-before"_utf16; }))
        return;
    EXPECT_EQ(server.hits("/normal-before"sv), 1u);

    auto extension_dir = LexicalPath::join(env.directory(), "privacy-extension"sv).string();
    write_ext_file(extension_dir, "manifest.json"sv, R"({
        "manifest_version": 2,
        "name": "Privacy Fixture",
        "version": "1.0",
        "browser_specific_settings": { "gecko": { "id": "privacy@ladyflock.example" } },
        "permissions": ["privacy", "storage"],
        "background": { "scripts": ["bg.js"] },
        "browser_action": { "default_title": "Disable prefetch" }
    })"sv);
    write_ext_file(extension_dir, "bg.js"sv, R"(
        const prediction = browser.privacy.network.networkPredictionEnabled;
        const gpc = browser.privacy.network.globalPrivacyControl;
        prediction.onChange.addListener(({ value }) => browser.storage.local.set({ predictionEvent: value }));
        gpc.onChange.addListener(({ value }) => browser.storage.local.set({ gpcEvent: value }));
        browser.browserAction.onClicked.addListener(async () => {
            try {
                const changed = await prediction.set({ value: false, scope: 'regular' });
                const regular = await prediction.get({});
                const privateSetting = await prediction.get({ incognito: true });
                const auditing = browser.privacy.websites.hyperlinkAuditingEnabled;
                const unsupported = await auditing.set({ value: true });
                const currentAuditing = await auditing.get({});
                const currentGpc = await gpc.get({});
                const correct = changed && regular.value === false
                    && regular.levelOfControl === 'controlled_by_this_extension'
                    && privateSetting.value === true && privateSetting.levelOfControl === 'not_controllable'
                    && unsupported === false && currentAuditing.value === false
                    && currentGpc.levelOfControl === 'not_controllable';
                await browser.storage.local.set({ status: correct ? 'applied' : 'incorrect' });
            } catch (error) {
                await browser.storage.local.set({ status: String(error) });
            }
        });
    )"sv);
    auto extension_id = TRY_OR_FAIL(manager.load_unpacked(extension_dir, true)).extension_id;
    EXPECT_EQ(manager.invoke_action_for_tab(extension_id, normal_view->view_id(), 1), WebView::Extensions::ExtensionActionController::InvokeOutcome::DispatchedOnClicked);
    auto storage_value = [&](String const& key) {
        return manager.storage().get(extension_id, JsonValue { key });
    };
    if (!wait_for([&] { return storage_value("status"_string).has("status"sv); }))
        return;
    EXPECT_EQ(storage_value("status"_string).get_string("status"sv), Optional<String> { "applied"_string });
    EXPECT(!manager.network_prediction_enabled());
    if (!wait_for([&] { return storage_value("predictionEvent"_string).get_bool("predictionEvent"sv) == false; }))
        return;
    WebView::Application::settings().set_global_privacy_control(WebView::GlobalPrivacyControl::Yes);
    if (!wait_for([&] { return storage_value("gpcEvent"_string).get_bool("gpcEvent"sv) == true; }))
        return;

    normal_view->run_javascript("issuePrefetch('/normal-blocked'); fetch('/barrier').then(() => { document.title = 'barrier'; });"_string);
    private_view->run_javascript("issuePrefetch('/private-allowed')"_string);
    if (!wait_for([&] { return normal_title == "barrier"_utf16 && private_title == "/private-allowed"_utf16; }))
        return;
    EXPECT_EQ(server.hits("/normal-blocked"sv), 0u);
    EXPECT_EQ(server.hits("/private-allowed"sv), 1u);

    TRY_OR_FAIL(manager.set_extension_enabled(extension_id, false));
    EXPECT(manager.network_prediction_enabled());
    normal_view->run_javascript("issuePrefetch('/normal-after')"_string);
    if (wait_for([&] { return normal_title == "/normal-after"_utf16; })) {
        EXPECT_EQ(server.hits("/normal-after"sv), 1u);
        EXPECT_EQ(server.hits("/normal-blocked"sv), 0u);
    }
}

TEST_CASE(extension_page_menu_registration_click_and_revocation)
{
    Test::HeadlessTestEnvironment env { "TestExtensionPageMenus"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto theme = TRY_OR_FAIL(load_default_theme());
    LoopbackHttpServer server([](auto const& path) -> HttpResponseSpec {
        if (path == "/frame"sv)
            return { .body = "<!DOCTYPE html><title>Menu frame</title>" };
        return { .body = "<!DOCTYPE html><title>Page menu test</title><iframe src='/frame'></iframe>" };
    });
    auto url = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/menu", server.port()))).release_value();
    auto ext_dir = LexicalPath::join(env.directory(), "menu-extension"sv).string();
    write_ext_file(ext_dir, "manifest.json"sv, R"({
        "manifest_version": 2,
        "name": "Example extension",
        "version": "1.0",
        "browser_specific_settings": {"gecko": {"id": "menus@example.test"}},
        "permissions": ["menus", "storage"],
        "background": {"scripts": ["menu.js"]}
    })"sv);
    write_ext_file(ext_dir, "menu.js"sv, R"(
        let clicks = 0;
        browser.menus.onClicked.addListener(async (info, tab) => {
            await browser.storage.local.set({click: info.menuItemId, count: ++clicks, url: info.pageUrl, tab: tab.id,
                link: info.linkUrl || "", frame: info.frameId, frameUrl: info.frameUrl || ""});
            await browser.menus.remove(info.menuItemId);
        });
        browser.menus.create({id: "unsupported", title: "Tab", contexts: ["tab"]}, () => {
            browser.storage.local.set({unsupported: !!browser.runtime.lastError});
        });
        browser.menus.create({id: "subscribe", title: "Subscribe", contexts: ["link"],
            targetUrlPatterns: ["abp:*", "https://subscribe.example.test/*"]});
        browser.menus.create({id: "resource", title: "Resource", contexts: ["audio", "frame", "image", "video"],
            documentUrlPatterns: ["http://*/*", "https://*/*"]});
        browser.menus.create({id: "different-site", title: "Elsewhere", contexts: ["all"],
            documentUrlPatterns: ["https://elsewhere.example.test/*"]});
        const id = browser.menus.create({id: "inspect", title: "First title", contexts: ["all"]}, async () => {
            if (browser.runtime.lastError) return;
            await browser.menus.update("inspect", {title: "Updated title"});
            await browser.storage.local.set({created: id === "inspect"});
        });
    )"sv);
    auto& manager = *app->default_session().extension_manager;
    auto extension = TRY_OR_FAIL(manager.load_unpacked(ext_dir));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);
    view->load_from_browser_ui(url);
    auto wait_until = [](auto condition) {
        bool expired = false;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(8), [&] { expired = true; });
        timer->start();
        Core::EventLoop::current().spin_until([&] { return expired || condition(); });
        timer->stop();
        EXPECT(!expired);
        return !expired;
    };
    auto storage = [&] { return manager.storage().get(extension.extension_id, JsonValue {}); };
    if (!wait_until([&] {
            return view->has_traversable() && view->url() == url && storage().get_bool("created"sv) == true;
        }))
        return;
    EXPECT_EQ(storage().get_bool("unsupported"sv), true);
    auto document = view->traversable().active_document().id();
    auto menu = manager.page_menu_items(*view, document);
    EXPECT_EQ(menu.size(), 1u);
    if (menu.size() != 1)
        return;
    EXPECT_EQ(menu[0].title, "Example extension — Updated title"sv);
    EXPECT(manager.page_menu_items(*view, {}).is_empty());
    manager.activate_page_menu_item(*view, {}, menu[0].token);
    WebView::ExtensionMenuTarget link;
    link.link_url = URL::Parser::basic_parse("https://elsewhere.example.test/ignored"sv);
    EXPECT_EQ(manager.page_menu_items(*view, document, link).size(), 1u);
    link.link_url = URL::Parser::basic_parse("abp:subscribe?location=test"sv);
    EXPECT_EQ(manager.page_menu_items(*view, document, link).size(), 2u);
    WebView::ExtensionMenuTarget image;
    image.media_type = "image"_string;
    EXPECT_EQ(manager.page_menu_items(*view, document, image).size(), 2u);
    Web::HTML::CrossProcessId subdocument {};
    if (!wait_until([&] {
            view->traversable().for_each_in_inclusive_subtree([&](WebView::CanonicalNavigable const& frame) {
                if (frame.parent() && frame.active_document_url().serialize().bytes_as_string_view().ends_with("/frame"sv))
                    subdocument = frame.active_document().id();
                return IterationDecision::Continue;
            });
            return subdocument != Web::HTML::CrossProcessId {};
        }))
        return;
    EXPECT_EQ(manager.page_menu_items(*view, subdocument, link).size(), 3u);
    manager.activate_page_menu_item(*view, subdocument, menu[0].token, link);
    if (!wait_until([&] { return manager.page_menu_items(*view, document).is_empty(); }))
        return;
    auto result = storage();
    EXPECT_EQ(result.get_string("click"sv).value_or(String {}), "inspect"sv);
    EXPECT_EQ(result.get_string("url"sv).value_or(String {}), url.serialize());
    EXPECT_EQ(result.get_u64("tab"sv), view->view_id());
    EXPECT_EQ(result.get_u64("count"sv), 1u);
    EXPECT(result.get_i64("frame"sv).value_or(0) > 0);
    auto frame_url = result.get_string("frameUrl"sv);
    EXPECT(frame_url.has_value() && frame_url->bytes_as_string_view().ends_with("/frame"sv));
    EXPECT_EQ(result.get_string("link"sv).value_or(String {}), link.link_url->serialize());
}

TEST_CASE(extension_dns_resolves_localhost_with_permission)
{
    Test::HeadlessTestEnvironment env { "TestExtensionDNS"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<RuntimeTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;

    auto directory = LexicalPath::join(env.directory(), "dns-allowed"sv).string();
    write_ext_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "DNS fixture", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "dns@example.test"}},
        "permissions": ["dns", "storage"], "background": {"scripts": ["background.js"]}
    })"sv);
    write_ext_file(directory, "background.js"sv, R"(
        async function rejects(host, flags) {
            try { await browser.dns.resolve(host, flags); return false; } catch { return true; }
        }
        (async () => {
            const v4 = await browser.dns.resolve("localhost", ["disable_ipv6", "canonical_name"]);
            const v6 = await browser.dns.resolve("localhost", ["disable_ipv4"]);
            const invalid = await rejects("bad/name", []);
            const missing = await rejects("uncached-dns-fixture.invalid", ["offline"]);
            const disabled = await rejects("localhost", ["disable_ipv4", "disable_ipv6"]);
            await browser.storage.local.set({v4, v6, invalid, missing, disabled, done: true});
        })().catch(error => browser.storage.local.set({error: String(error), done: true}));
    )"sv);
    auto allowed = TRY_OR_FAIL(manager.load_unpacked(directory));

    auto denied_directory = LexicalPath::join(env.directory(), "dns-denied"sv).string();
    write_ext_file(denied_directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Unprivileged DNS fixture", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "no-dns@example.test"}},
        "permissions": ["storage"], "background": {"scripts": ["background.js"]}
    })"sv);
    write_ext_file(denied_directory, "background.js"sv, R"(
        browser.storage.local.set({hidden: typeof browser.dns === "undefined", done: true});
    )"sv);
    auto denied = TRY_OR_FAIL(manager.load_unpacked(denied_directory));

    bool expired = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { expired = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] {
        return expired || (manager.storage().get(allowed.extension_id, "done"_string).has("done"sv) && manager.storage().get(denied.extension_id, "done"_string).has("done"sv));
    });
    timer->stop();
    EXPECT(!expired);
    auto result = manager.storage().get(allowed.extension_id, JsonValue {});
    if (auto error = result.get_string("error"sv); error.has_value())
        warnln("dns.resolve: {}", *error);
    EXPECT(result.get_bool("invalid"sv) == true);
    EXPECT(result.get_bool("missing"sv) == true);
    EXPECT(result.get_bool("disabled"sv) == true);
    auto v4 = result.get_object("v4"sv);
    auto v6 = result.get_object("v6"sv);
    EXPECT(v4.has_value() && v6.has_value());
    if (!v4.has_value() || !v6.has_value())
        return;
    EXPECT_EQ(v4->get_string("canonicalName"sv).value_or(String {}), "localhost"sv);
    EXPECT(v4->get_bool("isTRR"sv) == false);
    EXPECT(!v6->has("canonicalName"sv));
    auto addresses4 = v4->get_array("addresses"sv);
    auto addresses6 = v6->get_array("addresses"sv);
    EXPECT(addresses4.has_value() && addresses6.has_value());
    if (addresses4.has_value() && addresses6.has_value()) {
        EXPECT_EQ(addresses4->serialized(), R"(["127.0.0.1"])"sv);
        EXPECT_EQ(addresses6->serialized(), R"(["::1"])"sv);
    }
    EXPECT(manager.storage().get(denied.extension_id, "hidden"_string).get_bool("hidden"sv) == true);
}
