/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The APIs that the most-used Firefox extensions need beyond Ladyflock's first ones, each checked in a real browser
// against what the extension's own code sees. None of them reaches the network.

#include "HeadlessTest.h"
#include "PrivacyTestFixtures.h"
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <LibWebView/Extensions/NativeMessaging.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/NotificationManager.h>
#include <LibWebView/RecordingNotificationBackend.h>
#include <LibWebView/SystemIdle.h>
#include <LibWebView/Utilities.h>

namespace {

class CompatTestApp : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(CompatTestApp)

public:
    explicit CompatTestApp(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    // Stands in for the UI's new window: a view in a window of its own, reported as the Hub reports a window's tab.
    virtual void open_url_in_new_window(URL::URL const& url, WebView::IsPrivate) override
    {
        opened_window_urls.append(url);
        auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
        auto view = WebView::HeadlessWebView::create(MUST(Gfx::load_system_theme(theme_path.string())), { 400, 300 }, WebView::IsPrivate::No);
        view->set_extension_window_id(next_window_id++);
        view->load_from_browser_ui(url);
        default_session().extension_manager->notify_tab_created(*view);
        windows.append(move(view));
    }

    Vector<URL::URL> opened_window_urls;
    Vector<NonnullOwnPtr<WebView::HeadlessWebView>> windows;
    u64 next_window_id { 77 };

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions& request_server_options, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::No;
        browser_options.enable_extensions = true;
        browser_options.enable_content_blocker = WebView::EnableContentBlocker::No;
        request_server_options.http_disk_cache_mode = WebView::HTTPDiskCacheMode::Testing;
    }
};

void write_file(ByteString const& dir, StringView rel, StringView content)
{
    auto path = LexicalPath::join(dir, rel).string();
    MUST(Core::Directory::create(LexicalPath::dirname(path), Core::Directory::CreateDirectories::Yes));
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(content.bytes()));
}

// Waits for the extension's background page to store `done`, and returns what it stored.
JsonObject wait_for_results(WebView::Extensions::ExtensionManager& manager, StringView extension_id, StringView key = "done"sv, int seconds = 10)
{
    bool expired = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(seconds), [&] { expired = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] {
        return expired || manager.storage().get(extension_id, JsonValue { MUST(String::from_utf8(key)) }).has(key);
    });
    timer->stop();
    EXPECT(!expired);
    auto results = manager.storage().get(extension_id, JsonValue {});
    if (expired)
        warnln("Extension stored: {}", results.serialized());
    if (auto error = results.get_string("error"sv); error.has_value())
        warnln("Extension reported: {}", *error);
    return results;
}

void spin_until(Function<bool()> condition, int seconds = 10)
{
    bool expired = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(seconds), [&] { expired = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return expired || condition(); });
    timer->stop();
    EXPECT(!expired);
}

}

TEST_CASE(environment_apis_report_ladyflock_and_keep_sync_storage_on_this_device)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatEnvironment"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;

    auto directory = LexicalPath::join(env.directory(), "environment"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Environment fixture", "version": "2.5", "homepage_url": "https://example.test/home",
        "browser_specific_settings": {"gecko": {"id": "environment@example.test"}},
        "permissions": ["storage"], "background": {"scripts": ["background.js"]},
        "incognito": "not_allowed", "developer": {"name": "Fixture"}, "storage": {"managed_schema": "schema.json"},
        "externally_connectable": {"matches": ["https://example.com/*"]}, "version_name": "2.5 beta",
        "commands": {"toggle": {"suggested_key": {"default": "Ctrl+Shift+Y"}, "description": "Toggle"}}
    })"sv);
    write_file(directory, "background.js"sv, R"(
        async function rejection(promise) {
            try { await promise; return "resolved"; } catch (error) { return error.message; }
        }
        (async () => {
            const results = {};
            results.platform = await browser.runtime.getPlatformInfo();
            results.browser = await browser.runtime.getBrowserInfo();
            results.enums = [browser.runtime.PlatformOs.MAC, browser.runtime.PlatformArch.X86_64, browser.runtime.OnInstalledReason.INSTALL];
            results.incognito = [browser.extension.inIncognitoContext, await browser.extension.isAllowedIncognitoAccess()];
            results.extensionURL = browser.extension.getURL("page.html") === browser.runtime.getURL("page.html");

            const areaChanges = [];
            browser.storage.sync.onChanged.addListener(changes => areaChanges.push(Object.keys(changes)));
            const allChanges = [];
            browser.storage.onChanged.addListener((changes, area) => allChanges.push(area));
            await browser.storage.sync.set({ theme: "dark" });
            results.sync = await browser.storage.sync.get("theme");
            results.syncKept = (await browser.storage.local.get("theme")).theme === undefined;
            results.syncQuota = [browser.storage.sync.QUOTA_BYTES, browser.storage.sync.QUOTA_BYTES_PER_ITEM, browser.storage.sync.MAX_ITEMS];
            results.syncItemLimit = await rejection(browser.storage.sync.set({ big: "x".repeat(9000) }));
            results.managed = await rejection(browser.storage.managed.get());
            await new Promise(resolve => setTimeout(resolve, 100));
            results.areaChanges = areaChanges;
            results.allChanges = allChanges;

            const self = await browser.management.getSelf();
            results.self = [self.id, self.name, self.version, self.installType, self.enabled, self.homepageUrl];
            results.currentTab = await browser.tabs.getCurrent();
            results.windows = Array.isArray(await browser.windows.getAll());
            results.commands = await browser.commands.getAll();
            results.hasManagementList = typeof browser.management.getAll;
            await browser.storage.local.set({ results, done: true });
        })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
    )"sv);

    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    EXPECT_EQ(installed.compatibility_status, Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested);
    auto stored = wait_for_results(manager, installed.extension_id);
    auto results = stored.get_object("results"sv);
    EXPECT(results.has_value());
    if (!results.has_value())
        return;

    auto platform = results->get_object("platform"sv).value();
#if defined(AK_OS_MACOS)
    EXPECT_EQ(platform.get_string("os"sv).value(), "mac"sv);
#endif
#if ARCH(AARCH64)
    EXPECT_EQ(platform.get_string("arch"sv).value(), "aarch64"sv);
#endif
    auto browser_info = results->get_object("browser"sv).value();
    EXPECT_EQ(browser_info.get_string("name"sv).value(), "Ladyflock"sv);
    EXPECT_EQ(browser_info.get_string("vendor"sv).value(), "Ladyflock"sv);
    EXPECT_EQ(browser_info.get_string("version"sv).value(), "156.0"sv);
    EXPECT_EQ(results->get_array("enums"sv)->serialized(), R"(["mac","x86-64","install"])"sv);
    EXPECT_EQ(results->get_array("incognito"sv)->serialized(), "[false,false]"sv);
    EXPECT(results->get_bool("extensionURL"sv) == true);

    EXPECT_EQ(results->get_object("sync"sv)->serialized(), R"({"theme":"dark"})"sv);
    EXPECT(results->get_bool("syncKept"sv) == true);
    EXPECT_EQ(results->get_array("syncQuota"sv)->serialized(), "[102400,8192,512]"sv);
    EXPECT(results->get_string("syncItemLimit"sv)->contains("QuotaExceededError"sv));
    EXPECT_EQ(results->get_string("managed"sv).value(), "Managed storage manifest not found"sv);
    EXPECT_EQ(results->get_array("areaChanges"sv)->serialized(), R"([["theme"]])"sv);
    EXPECT_EQ(results->get_array("allChanges"sv)->serialized(), R"(["sync"])"sv);
    EXPECT_EQ(manager.sync_storage().get(installed.extension_id, JsonValue { "theme"_string }).get_string("theme"sv).value(), "dark"sv);

    EXPECT_EQ(results->get_array("self"sv)->serialized(), R"(["environment@example.test","Environment fixture","2.5","development",true,"https://example.test/home"])"sv);
    EXPECT(!results->has("currentTab"sv) || results->get("currentTab"sv)->is_null());
    EXPECT(results->get_bool("windows"sv) == true);
    EXPECT_EQ(results->get_array("commands"sv)->size(), 1u);
    EXPECT_EQ(results->get_string("hasManagementList"sv).value(), "undefined"sv);

    // Removing the extension removes what it kept in storage.sync.
    TRY_OR_FAIL(manager.remove_extension(installed.extension_id));
    EXPECT(manager.sync_storage().get(installed.extension_id, JsonValue {}).is_empty());
}

TEST_CASE(install_check_ignores_what_firefox_ignores_and_refuses_what_firefox_would_do)
{
    auto evaluate = [](StringView json) {
        auto manifest = MUST(Web::Extensions::ExtensionManifest::parse(json));
        return Web::Extensions::SupportMatrix::evaluate_manifest(manifest);
    };
    // Chrome's keys and permissions are ignored with a warning, as Firefox ignores them.
    auto chrome_only = evaluate(R"({"manifest_version": 2, "name": "A", "version": "1",
        "permissions": ["storage", "background", "offscreen"], "externally_connectable": {}, "key": "abc", "update_url": "https://example.com"})"sv);
    EXPECT_EQ(chrome_only.status, Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested);
    EXPECT_EQ(chrome_only.warnings.size(), 5u);
    EXPECT(chrome_only.unsupported_reasons.is_empty());

    // A permission or key Firefox acts on, and Ladyflock doesn't implement, still refuses the install.
    auto unsupported = evaluate(R"({"manifest_version": 2, "name": "A", "version": "1", "permissions": ["proxy"], "page_action": {}})"sv);
    EXPECT_EQ(unsupported.status, Web::Extensions::CompatibilityStatus::Incompatible);
    EXPECT_EQ(unsupported.unsupported_reasons.size(), 2u);

    // The password managers' permissions are all implemented.
    auto password_manager = evaluate(R"({"manifest_version": 3, "name": "A", "version": "1",
        "permissions": ["alarms", "clipboardWrite", "clipboardRead", "contextMenus", "downloads", "idle", "management", "nativeMessaging",
            "notifications", "privacy", "scripting", "storage", "tabs", "webNavigation", "webRequest", "webRequestBlocking",
            "declarativeNetRequestWithHostAccess", "cookies"],
        "background": {"scripts": ["bg.js"]},
        "content_scripts": [{"matches": ["<all_urls>"], "js": ["a.js"], "world": "MAIN"}],
        "commands": {"_execute_action": {"suggested_key": {"default": "Ctrl+Shift+X"}}}})"sv);
    for (auto const& reason : password_manager.unsupported_reasons)
        warnln("Unexpected refusal: {}", reason);
    EXPECT_EQ(password_manager.status, Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested);

    // An unknown content script world is a manifest error, as in Firefox.
    EXPECT(Web::Extensions::ExtensionManifest::parse(R"({"manifest_version": 2, "name": "A", "version": "1",
        "content_scripts": [{"matches": ["<all_urls>"], "js": ["a.js"], "world": "USER_SCRIPT"}]})"sv)
            .is_error());
}

TEST_CASE(notifications_reach_the_platform_and_report_clicks_and_dismissals)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatNotifications"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;
    WebView::NotificationManager::the().set_backend(make<WebView::RecordingNotificationBackend>());
    auto& backend = *WebView::NotificationManager::the().backend().as_recording_backend();

    auto directory = LexicalPath::join(env.directory(), "notifier"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Notifier", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "notifier@example.test"}},
        "permissions": ["notifications", "storage"], "background": {"scripts": ["background.js"]}
    })"sv);
    write_file(directory, "background.js"sv, R"(
        const events = [];
        const record = async event => { events.push(event); await browser.storage.local.set({ events }); };
        browser.notifications.onShown.addListener(id => record("shown:" + id));
        browser.notifications.onClicked.addListener(id => record("clicked:" + id));
        browser.notifications.onClosed.addListener((id, byUser) => record("closed:" + id + ":" + byUser));
        (async () => {
            const first = await browser.notifications.create("vault", { type: "basic", title: "Vault locked", message: "Unlock to fill" });
            const second = await browser.notifications.create({ type: "basic", title: "Second", message: "Generated id" });
            const all = await browser.notifications.getAll();
            const cleared = await browser.notifications.clear(second);
            const clearedAgain = await browser.notifications.clear(second);
            const invalid = await browser.notifications.create({ type: "basic" }).then(() => "created", error => "rejected");
            await browser.storage.local.set({ first, generated: second.length > 0, all: Object.keys(all).length, cleared, clearedAgain, invalid, done: true });
        })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_string("first"sv).value(), "vault"sv);
    EXPECT(results.get_bool("generated"sv) == true);
    EXPECT_EQ(results.get_u32("all"sv).value(), 2u);
    EXPECT(results.get_bool("cleared"sv) == true);
    EXPECT(results.get_bool("clearedAgain"sv) == false);
    EXPECT_EQ(results.get_string("invalid"sv).value(), "rejected"sv);

    // The platform shows it under the extension's name, and the user's click and the close that follows reach it.
    auto shown = backend.shown().first_matching([](auto const& notification) { return notification.data.title == "Vault locked"_utf16; });
    EXPECT(shown.has_value());
    if (!shown.has_value())
        return;
    EXPECT_EQ(shown->attribution, "Notifier"sv);
    EXPECT_EQ(shown->data.body, "Unlock to fill"_utf16);
    backend.click(shown->id);
    auto events_of = [&] { return manager.storage().get(installed.extension_id, JsonValue { "events"_string }).get_array("events"sv).value_or({}).serialized(); };
    spin_until([&] { return events_of().contains("closed:vault:true"sv); });
    EXPECT(events_of().contains("shown:vault"sv));
    EXPECT(events_of().contains("clicked:vault"sv));
}

TEST_CASE(idle_reports_the_system_state_and_its_changes)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatIdle"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;
    WebView::set_system_idle_status_for_testing(WebView::SystemIdleStatus { .idle_time = AK::Duration::from_seconds(120), .locked = false });
    ScopeGuard restore = [] { WebView::set_system_idle_status_for_testing({}); };

    auto directory = LexicalPath::join(env.directory(), "idle"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Idle", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "idle@example.test"}},
        "permissions": ["idle", "storage"], "background": {"scripts": ["background.js"]}
    })"sv);
    write_file(directory, "background.js"sv, R"(
        browser.idle.onStateChanged.addListener(state => browser.storage.local.set({ changed: state }));
        (async () => {
            const short = await browser.idle.queryState(60);
            const long = await browser.idle.queryState(300);
            const clamped = await browser.idle.queryState(1);
            await browser.idle.setDetectionInterval(60);
            await browser.storage.local.set({ short, long, clamped, done: true });
        })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_string("short"sv).value(), "idle"sv);
    EXPECT_EQ(results.get_string("long"sv).value(), "active"sv);
    EXPECT_EQ(results.get_string("clamped"sv).value(), "idle"sv);

    // The first check only learns the state; a change after it is reported.
    manager.poll_idle_state();
    WebView::set_system_idle_status_for_testing(WebView::SystemIdleStatus { .idle_time = AK::Duration::from_seconds(1), .locked = true });
    manager.poll_idle_state();
    auto changed = wait_for_results(manager, installed.extension_id, "changed"sv);
    EXPECT_EQ(changed.get_string("changed"sv).value(), "locked"sv);
}

TEST_CASE(native_messaging_without_a_host_fails_as_in_firefox)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatNative"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;
    auto hosts = LexicalPath::join(env.directory(), "hosts"sv).string();
    WebView::Extensions::NativeMessaging::set_manifest_directories_for_testing(Vector<ByteString> { hosts });
    ScopeGuard restore = [] { WebView::Extensions::NativeMessaging::set_manifest_directories_for_testing({}); };
    write_file(hosts, "com.example.other.json"sv, R"({"name": "com.example.other", "description": "", "path": "/bin/false", "type": "stdio", "allowed_extensions": ["someone@example.test"]})"sv);
    write_file(hosts, "com.example.allowed.json"sv, R"({"name": "com.example.allowed", "description": "", "path": "/bin/false", "type": "stdio", "allowed_extensions": ["native@example.test"]})"sv);

    auto directory = LexicalPath::join(env.directory(), "native"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Native", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "native@example.test"}},
        "permissions": ["nativeMessaging", "storage"], "background": {"scripts": ["background.js"]}
    })"sv);
    write_file(directory, "background.js"sv, R"(
        function disconnection(name) {
            return new Promise(resolve => {
                const port = browser.runtime.connectNative(name);
                port.onDisconnect.addListener(p => resolve(p.error ? p.error.message : "no error"));
            });
        }
        async function rejection(name) {
            try { await browser.runtime.sendNativeMessage(name, { ping: 1 }); return "resolved"; } catch (error) { return error.message; }
        }
        (async () => {
            await browser.storage.local.set({
                missing: await disconnection("com.example.missing"),
                notAllowed: await disconnection("com.example.other"),
                allowed: await disconnection("com.example.allowed"),
                message: await rejection("com.example.missing"),
                done: true,
            });
        })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_string("missing"sv).value(), "No such native application com.example.missing"sv);
    EXPECT_EQ(results.get_string("notAllowed"sv).value(), "No such native application com.example.other"sv);
    EXPECT_EQ(results.get_string("allowed"sv).value(), "Ladyflock does not start native applications such as com.example.allowed"sv);
    EXPECT_EQ(results.get_string("message"sv).value(), "No such native application com.example.missing"sv);
}

TEST_CASE(cookies_api_reaches_only_the_hosts_the_extension_may_access)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatCookies"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;
    auto& jar = *app->default_session().cookie_jar;
    auto other = URL::Parser::basic_parse("http://other.example/"sv).release_value();
    jar.set_cookie(other, HTTP::Cookie::parse_cookie(other, "hidden=1"sv).release_value(), HTTP::Cookie::Source::Http, {}, HTTP::Cookie::SameSiteContext::same_site());

    auto directory = LexicalPath::join(env.directory(), "cookies"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Cookies", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "cookies@example.test"}},
        "permissions": ["cookies", "storage", "http://site.example/*"], "background": {"scripts": ["background.js"]}
    })"sv);
    write_file(directory, "background.js"sv, R"(
        const changes = [];
        browser.cookies.onChanged.addListener(change => changes.push(change.cookie.name + ":" + change.removed));
        async function rejection(promise) {
            try { await promise; return "resolved"; } catch (error) { return "rejected"; }
        }
        (async () => {
            const set = await browser.cookies.set({ url: "http://site.example/app", name: "session", value: "abc", expirationDate: 4102444800 });
            const got = await browser.cookies.get({ url: "http://site.example/app/page", name: "session" });
            const all = await browser.cookies.getAll({});
            const domain = await browser.cookies.getAll({ domain: "site.example" });
            const deniedSet = await rejection(browser.cookies.set({ url: "http://other.example/", name: "x", value: "y" }));
            const deniedGet = await rejection(browser.cookies.get({ url: "http://other.example/", name: "hidden" }));
            const stores = await browser.cookies.getAllCookieStores();
            const removed = await browser.cookies.remove({ url: "http://site.example/app", name: "session" });
            const after = await browser.cookies.get({ url: "http://site.example/app", name: "session" });
            await new Promise(resolve => setTimeout(resolve, 200));
            await browser.storage.local.set({
                set: [set.name, set.value, set.domain, set.hostOnly, set.path, set.session, set.storeId],
                got: got && got.value, all: all.map(c => c.name), domain: domain.length, deniedSet, deniedGet,
                stores: stores.map(s => s.id), removed: removed && removed.name, after, changes, done: true,
            });
        })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_array("set"sv)->serialized(), R"(["session","abc","site.example",true,"/",false,"firefox-default"])"sv);
    EXPECT_EQ(results.get_string("got"sv).value(), "abc"sv);
    EXPECT_EQ(results.get_array("all"sv)->serialized(), R"(["session"])"sv);
    EXPECT_EQ(results.get_u32("domain"sv).value(), 1u);
    EXPECT_EQ(results.get_string("deniedSet"sv).value(), "rejected"sv);
    EXPECT_EQ(results.get_string("deniedGet"sv).value(), "rejected"sv);
    EXPECT_EQ(results.get_array("stores"sv)->serialized(), R"(["firefox-default"])"sv);
    EXPECT_EQ(results.get_string("removed"sv).value(), "session"sv);
    EXPECT(!results.has("after"sv) || results.get("after"sv)->is_null());
    EXPECT_EQ(results.get_array("changes"sv)->serialized(), R"(["session:false","session:true"])"sv);

    // The cookie the extension may not see is untouched, and the one it removed is gone from the jar.
    EXPECT(jar.get_named_cookie(other, "hidden"sv).has_value());
    EXPECT(!jar.get_named_cookie(URL::Parser::basic_parse("http://site.example/app"sv).release_value(), "session"sv).has_value());
}

TEST_CASE(downloads_save_into_the_downloads_folder_and_report_their_progress)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatDownloads"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;
    auto downloads = Core::StandardPaths::downloads_directory();
    EXPECT(downloads.starts_with(env.directory()));

    auto directory = LexicalPath::join(env.directory(), "exporter"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Exporter", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "exporter@example.test"}},
        "permissions": ["downloads", "storage"], "background": {"scripts": ["background.js"]}
    })"sv);
    write_file(directory, "background.js"sv, R"(
        const events = [];
        browser.downloads.onCreated.addListener(item => events.push("created:" + item.state));
        browser.downloads.onChanged.addListener(delta => { if (delta.state) events.push("state:" + delta.state.current); });
        async function rejection(promise) {
            try { await promise; return "resolved"; } catch (error) { return "rejected"; }
        }
        (async () => {
            const url = URL.createObjectURL(new Blob(["exported vault"], { type: "text/plain" }));
            const id = await browser.downloads.download({ url, filename: "exports/vault.txt" });
            const again = await browser.downloads.download({ url, filename: "exports/vault.txt" });
            const escape = await rejection(browser.downloads.download({ url, filename: "../outside.txt" }));
            const foreign = await rejection(browser.downloads.download({ url: "blob:https://example.com/abc" }));
            await new Promise(resolve => setTimeout(resolve, 300));
            const [item] = await browser.downloads.search({ id });
            const [second] = await browser.downloads.search({ id: again });
            await browser.storage.local.set({
                state: item.state, name: item.filename.split("/").slice(-2).join("/"), second: second.filename.split("/").pop(),
                byExtension: item.byExtensionId, bytes: item.bytesReceived, escape, foreign, events, done: true,
            });
        })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_string("state"sv).value(), "complete"sv);
    EXPECT_EQ(results.get_string("name"sv).value(), "exports/vault.txt"sv);
    EXPECT_EQ(results.get_string("second"sv).value(), "vault(1).txt"sv);
    EXPECT_EQ(results.get_string("byExtension"sv).value(), "exporter@example.test"sv);
    EXPECT_EQ(results.get_u32("bytes"sv).value(), 14u);
    EXPECT_EQ(results.get_string("escape"sv).value(), "rejected"sv);
    EXPECT_EQ(results.get_string("foreign"sv).value(), "rejected"sv);
    EXPECT(results.get_array("events"sv)->serialized().contains("created:"sv));
    EXPECT(results.get_array("events"sv)->serialized().contains("state:complete"sv));

    auto saved = LexicalPath::join(downloads, "exports/vault.txt"sv).string();
    auto file = TRY_OR_FAIL(Core::File::open(saved, Core::File::OpenMode::Read));
    auto contents = TRY_OR_FAIL(file->read_until_eof());
    EXPECT_EQ(StringView { contents.bytes() }, "exported vault"sv);
    EXPECT(!FileSystem::exists(LexicalPath::join(env.directory(), "outside.txt"sv).string()));
}

// A password manager copies a password from its background page, with no click to grant it, and later reads the
// clipboard to clear it only if it still holds that password. Firefox allows both under clipboardWrite and clipboardRead.
TEST_CASE(clipboard_permissions_let_extension_pages_use_the_clipboard_without_a_gesture)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatClipboard"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;
    app->set_clipboard_text("user's own text"_string);

    auto install = [&](StringView name, StringView permissions) {
        auto directory = LexicalPath::join(env.directory(), name).string();
        write_file(directory, "manifest.json"sv, MUST(String::formatted(R"({{
            "manifest_version": 2, "name": "{}", "version": "1",
            "browser_specific_settings": {{"gecko": {{"id": "{}@example.test"}}}},
            "permissions": [{}], "background": {{"scripts": ["background.js"]}}
        }})",
                                                     name, name, permissions)));
        write_file(directory, "background.js"sv, R"(
            async function attempt(promise) {
                try { return await promise; } catch (error) { return "denied"; }
            }
            (async () => {
                const before = await attempt(navigator.clipboard.readText());
                const write = await attempt(navigator.clipboard.writeText("copied password").then(() => "written"));
                const after = await attempt(navigator.clipboard.readText());
                await browser.storage.local.set({ before, write, after, done: true });
            })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
        )"sv);
        return MUST(manager.load_unpacked(directory)).extension_id;
    };

    auto unprivileged = install("unprivileged"sv, R"("storage")"sv);
    auto plain = wait_for_results(manager, unprivileged);
    EXPECT_EQ(plain.get_string("before"sv).value(), "denied"sv);
    EXPECT_EQ(plain.get_string("write"sv).value(), "denied"sv);
    EXPECT_EQ(app->clipboard_text(), "user's own text"_utf16);

    auto privileged = install("privileged"sv, R"("storage", "clipboardWrite", "clipboardRead")"sv);
    auto granted = wait_for_results(manager, privileged);
    EXPECT_EQ(granted.get_string("before"sv).value(), "user's own text"sv);
    EXPECT_EQ(granted.get_string("write"sv).value(), "written"sv);
    EXPECT_EQ(granted.get_string("after"sv).value(), "copied password"sv);
    EXPECT_EQ(app->clipboard_text(), "copied password"_utf16);
}

// A password manager turns off the browser's own password saving, reads its toolbar state, and opens its popup as a
// window of its own.
TEST_CASE(password_manager_startup_settings_and_popout_windows)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatPopout"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;

    auto directory = LexicalPath::join(env.directory(), "popout"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Popout", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "popout@example.test"}},
        "permissions": ["privacy", "storage"], "background": {"scripts": ["background.js"]},
        "browser_action": {"default_popup": "popup.html"}
    })"sv);
    write_file(directory, "popup.html"sv, "<!DOCTYPE html><title>Popup</title>"sv);
    write_file(directory, "background.js"sv, R"(
        const created = [];
        browser.windows.onCreated.addListener(window => created.push(window.id));
        (async () => {
            const saving = await browser.privacy.services.passwordSavingEnabled.get({});
            const turnedOff = await browser.privacy.services.passwordSavingEnabled.set({ value: false });
            const settings = await browser.browserAction.getUserSettings();
            const window = await browser.windows.create({ url: "popup.html", type: "popup", width: 400, height: 600 });
            const refused = await browser.windows.create({ url: "file:///etc/passwd" }).then(() => "opened", () => "refused");
            await new Promise(resolve => setTimeout(resolve, 100));
            await browser.storage.local.set({ saving, turnedOff, toolbar: typeof settings.isOnToolbar, window: window.id, created, refused, done: true });
        })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_object("saving"sv)->serialized(), R"({"value":false,"levelOfControl":"not_controllable"})"sv);
    EXPECT(results.get_bool("turnedOff"sv) == false);
    EXPECT_EQ(results.get_string("toolbar"sv).value(), "boolean"sv);
    EXPECT_EQ(results.get_u32("window"sv).value(), 77u);
    EXPECT_EQ(results.get_array("created"sv)->serialized(), "[77]"sv);
    EXPECT_EQ(results.get_string("refused"sv).value(), "refused"sv);
    EXPECT_EQ(app->opened_window_urls.size(), 1u);
    if (!app->opened_window_urls.is_empty())
        EXPECT_EQ(app->opened_window_urls.first().serialize(), MUST(String::formatted("moz-extension://{}/popup.html", installed.uuid)));
}

TEST_CASE(action_icons_firefox_accepts_but_ladyflock_cannot_draw_keep_the_current_icon)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatIcons"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;

    auto directory = LexicalPath::join(env.directory(), "icons"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Icons", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "icons@example.test"}},
        "permissions": ["storage"], "background": {"page": "background/index.html"},
        "browser_action": {"default_icon": "plain.png"}
    })"sv);
    write_file(directory, "background/index.html"sv, "<!DOCTYPE html><script src=\"background.js\"></script>"sv);
    write_file(directory, "background/background.js"sv, R"(
        const outcome = promise => promise.then(() => "ok", error => error.message);
        (async () => {
            // As in Firefox, a relative path is relative to the calling page.
            const png = await outcome(browser.browserAction.setIcon({ path: { 19: "../icons/lit.png" } }));
            const svg = await outcome(browser.browserAction.setIcon({ path: "/icons/toolbar.svg" }));
            const svgSizes = await outcome(browser.browserAction.setIcon({ path: { 16: "a.svg", 32: "b.svg" } }));
            const imageData = await outcome(browser.browserAction.setIcon({ imageData: new ImageData(16, 16) }));
            const missing = await outcome(browser.browserAction.setIcon({}));
            await browser.storage.local.set({ png, svg, svgSizes, imageData, missing, done: true });
        })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_string("png"sv).value(), "ok"sv);
    EXPECT_EQ(results.get_string("svg"sv).value(), "ok"sv);
    EXPECT_EQ(results.get_string("svgSizes"sv).value(), "ok"sv);
    EXPECT_EQ(results.get_string("imageData"sv).value(), "ok"sv);
    EXPECT_EQ(results.get_string("missing"sv).value(), "setIcon requires a package path or size-to-path object"sv);
    // The SVG and imageData calls left the PNG the extension set first.
    EXPECT_EQ(manager.action_controller().resolved_state(installed.extension_id).icon_path, Optional<String> { "icons/lit.png"_string });
}

TEST_CASE(messages_ports_and_listeners_take_every_form_firefox_accepts)
{
    static Atomic<bool> saw_ignored_header { false };
    PrivacyTest::IndependentHTTPServer server(PrivacyTest::IndependentHTTPServer::Protocol::HTTP, PrivacyTest::IndependentHTTPServer::Family::IPv4, [](ByteString const& request_head) {
        if (request_head.contains("X-Ignored"sv, CaseSensitivity::CaseInsensitive))
            saw_ignored_header = true;
        constexpr auto body = "<!DOCTYPE html><title>Page</title>"sv;
        return ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.length(), body);
    });
    Test::HeadlessTestEnvironment env { "TestExtCompatForms"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;

    auto directory = LexicalPath::join(env.directory(), "forms"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Forms", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "forms@example.test"}},
        "permissions": ["storage", "tabs", "webRequest", "webRequestBlocking", "webNavigation", "menus", "http://127.0.0.1/*"],
        "background": {"scripts": ["background.js"]},
        "content_scripts": [{"matches": ["http://127.0.0.1/*"], "js": ["content.js"]}]
    })"sv);
    write_file(directory, "background.js"sv, R"(
        browser.webRequest.onBeforeSendHeaders.addListener(details => ({ requestHeaders: [...details.requestHeaders, { name: 'X-Ignored', value: '1' }] }),
            { urls: ['http://127.0.0.1/*'] }, ['blocking', 'requestHeaders']);
        browser.runtime.onMessage.addListener((message, sender) => {
            if (message === 'ping' || message.ping)
                return Promise.resolve('pong');
            if (message.forms)
                check(sender.tab.id, message.forms).catch(error => browser.storage.local.set({ error: String(error), done: true }));
        });
        async function check(tabId, forms) {
            const top = await browser.tabs.sendMessage(tabId, 'to-content', { frameId: 0 });
            const plain = await browser.tabs.sendMessage(tabId, 'to-content');
            const missingFrame = await browser.tabs.sendMessage(tabId, 'to-content', { frameId: 99 }).then(() => 'answered', error => error.message);
            const other = await browser.runtime.sendMessage('other@example.test', 'ping', {}).then(() => 'answered', error => error.message);
            // Options with a key aren't valid options, so the string is another extension's ID.
            const otherByOptions = await browser.runtime.sendMessage('other@example.test', { ping: true }).then(() => 'answered', error => error.message);
            let badOptions;
            try { browser.runtime.sendMessage(browser.runtime.id, 'ping', { toProxyScript: true }); } catch (error) { badOptions = error.message; }
            const port = browser.runtime.connect(undefined, { name: 'sync' }).name;
            const enums = [browser.tabs.TabStatus.LOADING, browser.webRequest.ResourceType.CSP_REPORT,
                browser.webRequest.OnBeforeSendHeadersOptions.REQUESTHEADERS, typeof browser.webNavigation.onTabReplaced.addListener,
                typeof browser.menus.onShown.addListener, typeof browser.menus.onHidden.addListener].join(',');
            await browser.storage.local.set({ forms, top, plain, missingFrame, other, otherByOptions, badOptions, port, enums, done: true });
        }
    )"sv);
    write_file(directory, "content.js"sv, R"(
        browser.runtime.onMessage.addListener(message => message === 'to-content' ? Promise.resolve('from-content') : undefined);
        (async () => {
            const id = browser.runtime.id;
            const forms = [
                await browser.runtime.sendMessage('ping'),
                await browser.runtime.sendMessage(id, 'ping'),
                await browser.runtime.sendMessage({ ping: true }, {}),
                // As in Firefox, a string and valid or nullish options are a message, not an extension ID.
                await browser.runtime.sendMessage('ping', {}),
                await browser.runtime.sendMessage('ping', undefined),
                await browser.runtime.sendMessage('ping', null),
                await browser.runtime.sendMessage(id, 'ping', {}),
                await new Promise(resolve => browser.runtime.sendMessage('ping', resolve)),
            ].join(',');
            browser.runtime.sendMessage({ forms });
        })();
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    bool started = false;
    manager.ensure_background_started(installed.extension_id, [&](bool ready) { started = ready; });
    spin_until([&] { return started; });
    // The background page's console says its onBeforeSendHeaders listener can't change headers.
    bool warned = false;
    auto* background = manager.background_view_for_testing(installed.extension_id);
    VERIFY(background);
    background->on_console_message = [&](WebView::ConsoleOutput output) {
        if (auto const* log = output.output.get_pointer<WebView::ConsoleLog>(); log && log->level == JS::ConsoleLogLevel::Warn
            && any_of(log->arguments, [](auto const& argument) { return argument.is_string() && argument.as_string().contains("can't change the headers"sv); }))
            warned = true;
    };
    background->start_sending_console_output(true, true);

    app->open_url_in_new_window(URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/page", server.port())).release_value(), WebView::IsPrivate::No);
    auto results = wait_for_results(manager, installed.extension_id);
    spin_until([&] { return warned; });
    background->on_console_message = nullptr;
    EXPECT_EQ(results.get_string("forms"sv).value_or({}), "pong,pong,pong,pong,pong,pong,pong,pong"sv);
    EXPECT_EQ(results.get_string("top"sv).value_or({}), "from-content"sv);
    EXPECT_EQ(results.get_string("plain"sv).value_or({}), "from-content"sv);
    EXPECT_EQ(results.get_string("missingFrame"sv).value_or({}), "Could not establish connection. Receiving end does not exist."sv);
    EXPECT_EQ(results.get_string("other"sv).value_or({}), "Could not establish connection. Receiving end does not exist."sv);
    EXPECT_EQ(results.get_string("otherByOptions"sv).value_or({}), "Could not establish connection. Receiving end does not exist."sv);
    EXPECT_EQ(results.get_string("badOptions"sv).value_or({}), "runtime.sendMessage's options argument is invalid"sv);
    EXPECT_EQ(results.get_string("port"sv).value_or({}), "sync"sv);
    EXPECT_EQ(results.get_string("enums"sv).value_or({}), "loading,csp_report,requestHeaders,function,function,function"sv);
    EXPECT(server.request_count() > 0);
    EXPECT(!saw_ignored_header.load());
}

TEST_CASE(extension_pages_read_the_hosts_they_may_access_and_nothing_else)
{
    // Three origins on the loopback interface, none of which sends CORS headers: the page's, the API's (another
    // port of the same host, which the extension may access), and one the extension may not access.
    using Server = PrivacyTest::IndependentHTTPServer;
    auto respond = [](StringView body) {
        return ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.length(), body);
    };
    static Atomic<size_t> preflights { 0 };
    static Atomic<bool> default_sent_cookie { false };
    static Atomic<bool> include_sent_cookie { false };
    static Atomic<u16> other_port { 0 };
    Server api(Server::Protocol::HTTP, Server::Family::IPv4, [&](ByteString const& request_head) {
        if (request_head.starts_with("OPTIONS "sv))
            preflights.fetch_add(1);
        bool has_cookie = request_head.contains("\r\nCookie: visited=1"sv, CaseSensitivity::CaseInsensitive);
        if (request_head.starts_with("GET /set-cookie "sv))
            return ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nSet-Cookie: visited=1; Path=/\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
        if (request_head.starts_with("GET /default "sv) && has_cookie)
            default_sent_cookie = true;
        if (request_head.starts_with("GET /include "sv) && has_cookie)
            include_sent_cookie = true;
        if (request_head.starts_with("GET /redirect "sv))
            return ByteString::formatted("HTTP/1.1 302 Found\r\nLocation: http://[::1]:{}/data\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", other_port.load());
        return respond("secret"sv);
    });
    auto api_url = ByteString::formatted("http://127.0.0.1:{}/data", api.port());
    Server other(Server::Protocol::HTTP, Server::Family::IPv6, [&](ByteString const&) { return respond("secret"sv); });
    other_port = other.port();
    auto page_script = ByteString::formatted("<!DOCTYPE html><script>fetch('{}').then(r => r.text(), () => 'blocked').then(t => {{ document.title = 'page:' + t; }});</script>", api_url);
    Server site(Server::Protocol::HTTP, Server::Family::IPv4, [&](ByteString const&) { return respond(page_script); });

    Test::HeadlessTestEnvironment env { "TestExtCompatCORS"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;

    auto directory = LexicalPath::join(env.directory(), "cors"sv).string();
    write_file(directory, "urls.js"sv, ByteString::formatted("const api = '{}'; const other = 'http://[::1]:{}/data';", api_url, other.port()));
    write_file(directory, "background.js"sv, R"(
        const read = (url, init) => fetch(url, init).then(response => response.text(), () => 'blocked');
        browser.runtime.onMessage.addListener(message => { browser.storage.local.set({ content: message.content }); });
        (async () => {
            const allowed = await read(api);
            const unsafe = await read(api, { method: 'PUT', headers: { 'X-Custom': '1' } });
            const unpermitted = await read(other);
            // A permitted host's redirect to one the extension may not access is not readable.
            const redirected = await read(api.replace('/data', '/redirect'));
            await browser.storage.local.set({ allowed, unsafe, unpermitted, redirected, done: true });
            // Keeps reading until the user withdraws the access.
            const timer = setInterval(async () => {
                if (await read(api) === 'blocked') {
                    clearInterval(timer);
                    browser.storage.local.set({ withdrawn: true });
                }
            }, 100);
        })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
    )"sv);
    write_file(directory, "content.js"sv, ByteString::formatted(R"(
        if (location.pathname === '/page')
            fetch('{}').then(response => response.text(), () => 'blocked').then(content => browser.runtime.sendMessage({{ content }}));
    )",
                                              api_url));
    // The background page gets the servers' addresses from urls.js.
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "CORS", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "cors@example.test"}},
        "permissions": ["storage", "http://127.0.0.1/*"],
        "background": {"scripts": ["urls.js", "background.js"]},
        "content_scripts": [{"matches": ["http://127.0.0.1/*"], "js": ["content.js"]}]
    })"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    bool started = false;
    manager.ensure_background_started(installed.extension_id, [&](bool ready) { started = ready; });
    spin_until([&] { return started; });

    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_string("allowed"sv).value_or({}), "secret"sv);
    EXPECT_EQ(results.get_string("unsafe"sv).value_or({}), "secret"sv);
    EXPECT_EQ(results.get_string("unpermitted"sv).value_or({}), "blocked"sv);
    EXPECT_EQ(results.get_string("redirected"sv).value_or({}), "blocked"sv);
    EXPECT_EQ(preflights.load(), 0u);

    // The site's cookie doesn't go with the extension's read by default. Even with credentials: 'include', the
    // request is third-party to the site, so Ladyflock sends no cookie.
    app->open_url_in_new_window(URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/set-cookie", api.port())).release_value(), WebView::IsPrivate::No);
    spin_until([&] { return app->default_session().cookie_jar->get_all_cookies().size() > 0; });
    auto* background = manager.background_view_for_testing(installed.extension_id);
    VERIFY(background);
    background->run_javascript(MUST(String::formatted(R"(
        fetch('http://127.0.0.1:{}/default').then(() => fetch('http://127.0.0.1:{}/include', {{ credentials: 'include' }}))
            .then(() => browser.storage.local.set({{ cookies: 'checked' }}));
    )",
        api.port(), api.port())));
    wait_for_results(manager, installed.extension_id, "cookies"sv);
    EXPECT(!default_sent_cookie.load());
    EXPECT(!include_sent_cookie.load());

    // Neither the page nor the extension's content script in it reads the API's responses.
    app->open_url_in_new_window(URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/page", site.port())).release_value(), WebView::IsPrivate::No);
    spin_until([&] { return !app->windows.is_empty() && app->windows.last()->title().to_utf8().starts_with_bytes("page:"sv); });
    EXPECT_EQ(app->windows.last()->title().to_utf8(), "page:blocked"sv);
    wait_for_results(manager, installed.extension_id, "content"sv);
    EXPECT_EQ(manager.storage().get(installed.extension_id, {}).get_string("content"sv).value_or({}), "blocked"sv);

    TRY_OR_FAIL(manager.set_site_access(installed.extension_id, true, "127.0.0.1"_string, false));
    wait_for_results(manager, installed.extension_id, "withdrawn"sv);
}

TEST_CASE(all_frames_injection_reaches_each_frame_the_extension_may_access)
{
    using Server = PrivacyTest::IndependentHTTPServer;
    auto respond = [](StringView body) {
        return ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.length(), body);
    };
    Server unpermitted(Server::Protocol::HTTP, Server::Family::IPv6, [&](ByteString const&) { return respond("<!DOCTYPE html><body>other"sv); });
    auto page = ByteString::formatted("<!DOCTYPE html><body>top<iframe src=\"/frame\"></iframe><iframe src=\"http://[::1]:{}/\"></iframe>", unpermitted.port());
    Server site(Server::Protocol::HTTP, Server::Family::IPv4, [&](ByteString const& request_head) {
        return request_head.starts_with("GET /frame "sv) ? respond("<!DOCTYPE html><body>frame"sv) : respond(page);
    });

    Test::HeadlessTestEnvironment env { "TestExtCompatAllFrames"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;

    auto directory = LexicalPath::join(env.directory(), "frames"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Frames", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "frames@example.test"}},
        "permissions": ["storage", "tabs", "http://127.0.0.1/*"],
        "background": {"scripts": ["background.js"]},
        "content_scripts": [{"matches": ["http://127.0.0.1/*"], "js": ["content.js"]}]
    })"sv);
    write_file(directory, "content.js"sv, "browser.runtime.sendMessage('loaded');"sv);
    write_file(directory, "background.js"sv, R"(
        // The top frame's content script says when the page has loaded.
        browser.runtime.onMessage.addListener(async (message, sender) => {
            const tabId = sender.tab.id;
            try {
                await new Promise(resolve => setTimeout(resolve, 300));
                const names = await browser.tabs.executeScript(tabId, { code: 'document.body.firstChild.textContent', allFrames: true });
                await browser.tabs.insertCSS(tabId, { code: 'body { color: rgb(1, 2, 3) }', allFrames: true });
                const colors = await browser.tabs.executeScript(tabId, { code: 'getComputedStyle(document.body).color', allFrames: true });
                const top = await browser.tabs.executeScript(tabId, { code: '1' });
                const both = await browser.tabs.executeScript(tabId, { code: '1', allFrames: true, frameId: 0 }).then(() => 'ran', error => error.message);
                await browser.storage.local.set({ names: names.join(','), colors: colors.join(','), top: top.length, both, done: true });
            } catch (error) {
                await browser.storage.local.set({ error: String(error), done: true });
            }
        });
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    bool started = false;
    manager.ensure_background_started(installed.extension_id, [&](bool ready) { started = ready; });
    spin_until([&] { return started; });

    app->open_url_in_new_window(URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/page", site.port())).release_value(), WebView::IsPrivate::No);
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_string("names"sv).value_or({}), "top,frame"sv);
    EXPECT_EQ(results.get_string("colors"sv).value_or({}), "rgb(1, 2, 3),rgb(1, 2, 3)"sv);
    EXPECT_EQ(results.get_u32("top"sv).value_or(0), 1u);
    EXPECT_EQ(results.get_string("both"sv).value_or({}), "Unsupported script injection target or world"sv);
}

TEST_CASE(activating_a_tab_also_reports_it_highlighted)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatHighlight"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;

    auto directory = LexicalPath::join(env.directory(), "highlight"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Highlight", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "highlight@example.test"}},
        "permissions": ["storage"], "background": {"scripts": ["background.js"]}
    })"sv);
    write_file(directory, "background.js"sv, R"(
        let activated;
        browser.tabs.onActivated.addListener(info => { activated = info.tabId; });
        browser.tabs.onHighlighted.addListener(info => {
            browser.storage.local.set({ highlighted: info.tabIds.join(','), window: info.windowId, activated, done: true });
        });
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    bool started = false;
    manager.ensure_background_started(installed.extension_id, [&](bool ready) { started = ready; });
    spin_until([&] { return started; });

    app->open_url_in_new_window(URL::about_blank(), WebView::IsPrivate::No);
    auto tab_id = app->windows.last()->view_id();
    manager.notify_tab_activated(tab_id, 77);
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_string("highlighted"sv).value_or({}), String::number(tab_id));
    EXPECT_EQ(results.get_u32("window"sv).value_or(0), 77u);
    EXPECT_EQ(results.get_u64("activated"sv).value_or(0), tab_id);
}

TEST_CASE(the_background_page_finds_itself_and_other_pages_do_not_look)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatBackgroundPage"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;

    auto directory = LexicalPath::join(env.directory(), "background-page"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Background page", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "background-page@example.test"}},
        "permissions": ["storage"], "background": {"scripts": ["background.js"]}
    })"sv);
    write_file(directory, "background.js"sv, R"(
        (async () => {
            const itself = browser.extension.getBackgroundPage() === window;
            const asynchronously = (await browser.runtime.getBackgroundPage()) === window;
            await browser.storage.local.set({ itself, asynchronously, background: true });
        })().catch(error => browser.storage.local.set({ error: String(error), background: true }));
    )"sv);
    write_file(directory, "page.html"sv, "<!DOCTYPE html><script src=page.js></script>"sv);
    // Other pages don't have them, so extensions that check send messages instead.
    write_file(directory, "page.js"sv, R"(
        browser.storage.local.set({ fromPage: typeof browser.extension.getBackgroundPage, asyncFromPage: typeof browser.runtime.getBackgroundPage, done: true });
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    bool started = false;
    manager.ensure_background_started(installed.extension_id, [&](bool ready) { started = ready; });
    spin_until([&] { return started; });
    wait_for_results(manager, installed.extension_id, "background"sv);

    app->open_url_in_new_window(URL::Parser::basic_parse(MUST(String::formatted("moz-extension://{}/page.html", installed.uuid))).release_value(), WebView::IsPrivate::No);
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT(results.get_bool("itself"sv) == true);
    EXPECT(results.get_bool("asynchronously"sv) == true);
    EXPECT_EQ(results.get_string("fromPage"sv).value_or({}), "undefined"sv);
    EXPECT_EQ(results.get_string("asyncFromPage"sv).value_or({}), "undefined"sv);
}

TEST_CASE(packaged_webassembly_compiles_as_it_streams_in)
{
    Test::HeadlessTestEnvironment env { "TestExtCompatWasm"sv, 60 };
    auto app = TRY_OR_FAIL(env.create_application<CompatTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& manager = *app->default_session().extension_manager;

    auto directory = LexicalPath::join(env.directory(), "wasm"sv).string();
    write_file(directory, "manifest.json"sv, R"({
        "manifest_version": 2, "name": "Wasm", "version": "1",
        "browser_specific_settings": {"gecko": {"id": "wasm@example.test"}},
        "permissions": ["storage"], "background": {"scripts": ["background.js"]}
    })"sv);
    // The smallest module: its magic number and version.
    constexpr u8 module[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
    write_file(directory, "module.wasm"sv, StringView { reinterpret_cast<char const*>(module), sizeof(module) });
    write_file(directory, "background.js"sv, R"(
        (async () => {
            const type = (await fetch('module.wasm')).headers.get('content-type');
            const compiled = await WebAssembly.compileStreaming(fetch('module.wasm')).then(() => 'compiled', error => error.message);
            await browser.storage.local.set({ type, compiled, done: true });
        })().catch(error => browser.storage.local.set({ error: String(error), done: true }));
    )"sv);
    auto installed = TRY_OR_FAIL(manager.load_unpacked(directory));
    bool started = false;
    manager.ensure_background_started(installed.extension_id, [&](bool ready) { started = ready; });
    spin_until([&] { return started; });
    auto results = wait_for_results(manager, installed.extension_id);
    EXPECT_EQ(results.get_string("type"sv).value_or({}), "application/wasm"sv);
    EXPECT_EQ(results.get_string("compiled"sv).value_or({}), "compiled"sv);
}
