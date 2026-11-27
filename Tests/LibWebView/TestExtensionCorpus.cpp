/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Starts real extensions one at a time, each in a fresh profile, and reports what goes wrong as each extension's own
// code sees it: the errors its background page, its popup and its content scripts report to their consoles.
//
// The extensions come from LADYFLOCK_EXTENSION_CORPUS, a folder holding one unpacked extension per subfolder; without
// it, the case does nothing. Every request goes through a proxy on the loopback interface, which serves one test page
// with a sign-in form and refuses everything else, so no extension reaches the network. The report is one JSON object
// per extension on standard output, with the hosts each extension tried to reach.

#include "HeadlessTest.h"
#include <AK/Atomic.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/Mutex.h>
#include <AK/QuickSort.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>
#include <LibWebView/Application.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

class CorpusApp : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(CorpusApp)

public:
    explicit CorpusApp(Optional<ByteString> ladybird_binary_path)
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

constexpr StringView test_page = R"html(<!DOCTYPE html>
<html><head><title>Sign in</title></head>
<body>
<form id="login" action="/signed-in" method="post">
  <label>Email <input type="email" name="username" autocomplete="username"></label>
  <label>Password <input type="password" name="password" autocomplete="current-password"></label>
  <button type="submit">Sign in</button>
</form>
<p>Lorem ipsum dolor sit amet, <a href="/other">a link</a>.</p>
<video width="320" height="180"></video>
</body></html>)html"sv;

// A forward proxy that answers only for the test site and refuses every other request, including every CONNECT.
class RefusingProxy {
public:
    RefusingProxy()
    {
        m_listen_fd = MUST(Core::System::socket(AF_INET, SOCK_STREAM, 0));
        int reuse = 1;
        MUST(Core::System::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        MUST(Core::System::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
        MUST(Core::System::listen(m_listen_fd, 64));
        MUST(Core::System::fcntl(m_listen_fd, F_SETFL, O_NONBLOCK));
        socklen_t length = sizeof(address);
        VERIFY(::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        m_port = ntohs(address.sin_port);
        m_thread = Threading::Thread::construct("RefusingProxy"sv, [this] -> intptr_t {
            serve();
            return 0;
        });
        m_thread->start();
    }

    ~RefusingProxy()
    {
        m_stop.store(true);
        (void)m_thread->join();
        (void)Core::System::close(m_listen_fd);
    }

    u16 port() const { return m_port; }

    Vector<ByteString> take_refused_hosts()
    {
        AK::MutexLocker locker(m_mutex);
        Vector<ByteString> hosts;
        for (auto const& host : m_refused_hosts)
            hosts.append(host);
        m_refused_hosts.clear();
        quick_sort(hosts);
        return hosts;
    }

private:
    void serve()
    {
        while (!m_stop.load()) {
            pollfd listener { .fd = m_listen_fd, .events = POLLIN, .revents = 0 };
            if (::poll(&listener, 1, 25) <= 0)
                continue;
            int client = ::accept(m_listen_fd, nullptr, nullptr);
            if (client < 0)
                continue;
            ByteString request;
            char chunk[4096];
            for (;;) {
                pollfd input { .fd = client, .events = POLLIN, .revents = 0 };
                if (::poll(&input, 1, 1000) <= 0)
                    break;
                auto count = ::read(client, chunk, sizeof(chunk));
                if (count <= 0)
                    break;
                request = ByteString::formatted("{}{}", request, StringView { chunk, static_cast<size_t>(count) });
                if (request.contains("\r\n\r\n"sv) || request.length() > 64 * KiB)
                    break;
            }
            auto line_end = request.find("\r\n"sv);
            auto line = line_end.has_value() ? request.view().substring_view(0, *line_end) : request.view();
            auto parts = line.split_view(' ');
            auto target = parts.size() >= 2 ? parts[1] : StringView {};
            ByteString response;
            auto url = URL::Parser::basic_parse(target);
            auto has_host = url.has_value() && url->host().has_value() && !url->host()->is_empty_host();
            if (parts.size() >= 2 && parts[0] != "CONNECT"sv && has_host && url->serialized_host() == "login.test"sv) {
                response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", test_page.length(), test_page);
            } else {
                auto host = has_host ? url->serialized_host().to_byte_string() : ByteString { target.find(':').has_value() ? target.substring_view(0, *target.find(':')) : target };
                {
                    AK::MutexLocker locker(m_mutex);
                    m_refused_hosts.set(host);
                }
                response = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"sv;
            }
            (void)::write(client, response.characters(), response.length());
            ::close(client);
        }
    }

    int m_listen_fd { -1 };
    u16 m_port { 0 };
    Atomic<bool> m_stop { false };
    RefPtr<Threading::Thread> m_thread;
    AK::Mutex m_mutex;
    HashTable<ByteString> m_refused_hosts;
};

String describe(WebView::ConsoleOutput const& output)
{
    return output.output.visit(
        [](WebView::ConsoleError const& error) {
            auto location = error.trace.is_empty() ? String {} : MUST(String::formatted(" ({}:{})", error.trace.first().file.value_or({}), error.trace.first().line.value_or(0)));
            return MUST(String::formatted("{}{}: {}{}", error.inside_promise ? "(in promise) "sv : ""sv, error.name, error.message, location));
        },
        [](WebView::ConsoleLog const& log) {
            StringBuilder builder;
            for (auto const& argument : log.arguments) {
                if (!builder.is_empty())
                    builder.append(' ');
                builder.append(argument.is_string() ? argument.as_string() : argument.serialized());
            }
            return MUST(String::formatted("console.error: {}", builder.string_view().substring_view(0, min(builder.length(), 300uz))));
        },
        [](auto const&) { return String {}; });
}

bool is_error(WebView::ConsoleOutput const& output)
{
    return output.output.visit(
        [](WebView::ConsoleError const&) { return true; },
        [](WebView::ConsoleLog const& log) { return log.level == JS::ConsoleLogLevel::Error; },
        [](auto const&) { return false; });
}

void listen(WebView::ViewImplementation& view, JsonArray& errors)
{
    view.on_console_message = [&errors](WebView::ConsoleOutput output) {
        if (is_error(output) && errors.size() < 40)
            errors.must_append(describe(output));
    };
    view.start_sending_console_output(true, true);
}

void wait(int milliseconds)
{
    bool done = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(milliseconds), [&] { done = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return done; });
}

}

TEST_CASE(real_extensions_start_and_report_their_errors)
{
    auto corpus = Core::Environment::get("LADYFLOCK_EXTENSION_CORPUS"sv);
    if (!corpus.has_value()) {
        warnln("LADYFLOCK_EXTENSION_CORPUS is not set; no extensions to start");
        return;
    }
    Vector<ByteString> directories;
    MUST(Core::Directory::for_each_entry(*corpus, Core::DirIterator::SkipParentAndBaseDir, [&](auto const& entry, auto const& directory) -> ErrorOr<IterationDecision> {
        if (entry.type == Core::DirectoryEntry::Type::Directory)
            directories.append(LexicalPath::join(directory.path().string(), entry.name).string());
        return IterationDecision::Continue;
    }));
    quick_sort(directories);
    auto only = Core::Environment::get("LADYFLOCK_EXTENSION_CORPUS_ONLY"sv);

    RefusingProxy proxy;
    auto proxy_argument = ByteString::formatted("--privacy-proxy=http://127.0.0.1:{}", proxy.port());
    auto theme = MUST(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));

    for (auto const& directory : directories) {
        auto name = LexicalPath::basename(directory);
        if (only.has_value() && name != *only)
            continue;
        JsonObject report;
        report.set("extension"sv, MUST(String::from_byte_string(name)));
        // Renderers log every unhandled exception, including a content script's, which a reader of the log can
        // attribute to the extension started after this line.
        warnln("=== corpus extension: {}", name);

        Test::HeadlessTestEnvironment env { ByteString::formatted("Corpus-{}", name), 120 };
        auto app = TRY_OR_FAIL(env.create_application<CorpusApp>(Array { "--temporary-profile"sv, "--privacy-network-mode=proxy-only"sv, proxy_argument.view() }.span()));
        auto& manager = *app->default_session().extension_manager;
        auto installed = manager.load_unpacked(directory);
        if (installed.is_error()) {
            report.set("install_error"sv, MUST(String::formatted("{}", installed.error())));
            warnln("REPORT {}", report.serialized());
            continue;
        }
        auto extension = installed.release_value();
        report.set("compatibility"sv, MUST(String::from_utf8(Web::Extensions::to_string(extension.compatibility_status))));

        // The background page: its startup, which may already be over, comes from the console's buffer.
        JsonArray background_errors;
        bool has_background = extension.manifest.background().has_value() && extension.manifest.background()->has_background();
        if (has_background) {
            bool started = false;
            manager.ensure_background_started(extension.extension_id, [&](bool ready) { started = ready; });
            bool expired = false;
            auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(8), [&] { expired = true; });
            timer->start();
            Core::EventLoop::current().spin_until([&] { return expired || started; });
            report.set("background_started"sv, started);
            if (auto* view = manager.background_view_for_testing(extension.extension_id))
                listen(*view, background_errors);
        }

        // A page with a sign-in form, for the content scripts.
        JsonArray content_errors;
        auto tab = WebView::HeadlessWebView::create(theme, { 1024, 768 }, WebView::IsPrivate::No);
        listen(*tab, content_errors);
        tab->load_from_browser_ui(URL::Parser::basic_parse("http://login.test/"sv).release_value());
        wait(3000);
        auto const& document = tab->traversable().active_document();
        auto page_url = document.creation_url();
        report.set("page_loaded"sv, page_url.host().has_value() && page_url.serialized_host() == "login.test"sv && document.is_completely_loaded());

        // The toolbar popup, as the user opens it.
        JsonArray popup_errors;
        if (auto action = extension.manifest.action(); action.has_value() && action->default_popup.has_value()) {
            auto popup = WebView::HeadlessWebView::create(theme, { 400, 600 }, WebView::IsPrivate::No);
            listen(*popup, popup_errors);
            popup->load_from_browser_ui(URL::Parser::basic_parse(MUST(String::formatted("moz-extension://{}/{}", extension.uuid, *action->default_popup))).release_value());
            wait(3000);
            popup->on_console_message = nullptr;
        }
        wait(500);
        tab->on_console_message = nullptr;
        if (auto* view = manager.background_view_for_testing(extension.extension_id))
            view->on_console_message = nullptr;

        report.set("background_errors"sv, move(background_errors));
        report.set("content_errors"sv, move(content_errors));
        report.set("popup_errors"sv, move(popup_errors));
        JsonArray hosts;
        for (auto const& host : proxy.take_refused_hosts())
            hosts.must_append(MUST(String::from_byte_string(host)));
        report.set("refused_hosts"sv, move(hosts));
        warnln("REPORT {}", report.serialized());
    }
}
