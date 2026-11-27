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
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibPrivacy/Identity/Types.h>
#include <LibPrivacy/Policy/Compiler.h>
#include <LibPrivacy/Policy/ManagedPolicy.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/IdentityManager.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/Utilities.h>

#include <unistd.h>

// An administrator's managed policy binds every identity, whatever the command line asks for. Outside tests the policy
// must be in the platform's protected location and belong to root; this in-process application points at a file the
// test writes, owned by the account running it. The real protected location is not exercised here.

namespace {

ByteString s_policy_path;

class ManagedApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(ManagedApplication)

public:
    explicit ManagedApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual Optional<ByteString> managed_privacy_policy_path() const override { return s_policy_path; }
    virtual uid_t managed_privacy_policy_owner() const override { return getuid(); }
};

Core::AnonymousBuffer load_theme()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string();
    return MUST(Gfx::load_system_theme(theme_path));
}

ByteString fetch_from_page(WebView::HeadlessWebView& view, StringView target_url)
{
    view.load_html(ByteString::formatted(R"HTML(<!doctype html><title>boot</title><script>
fetch("{}").then(r => r.text()).then(text => {{ document.title = "fetched:" + text; }}).catch(() => {{ document.title = "failed"; }});
</script>)HTML",
        target_url));

    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(15), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] {
        auto title = view.title().to_byte_string();
        return timed_out || title.starts_with("fetched:"sv) || title == "failed"sv;
    });
    VERIFY(!timed_out);
    return view.title().to_byte_string();
}

ByteString make_response(StringView body)
{
    return ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.length(), body);
}

void write_policy(Test::HeadlessTestEnvironment const& environment, Optional<StringView> contents, mode_t mode = 0644)
{
    auto directory = LexicalPath::join(environment.directory(), "managed"sv).string();
    MUST(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    MUST(Core::System::chmod(directory, 0755));
    s_policy_path = LexicalPath::join(directory, "policy.json"sv).string();
    if (!contents.has_value())
        return;
    auto file = MUST(Core::File::open(s_policy_path, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(*contents));
    MUST(Core::System::chmod(s_policy_path, mode));
}

void verify_profile_is_inside(Test::HeadlessTestEnvironment const& environment)
{
    // These applications run in test mode, whose profile is a temporary one in the system's temporary directory.
    auto temporary_directory = MUST(FileSystem::real_path(Core::StandardPaths::tempfile_directory()));
    auto const& paths = WebView::Application::the().profile().paths();
    for (auto const& path : { paths.config, paths.data, paths.cache }) {
        if (!path.starts_with(environment.directory()) && !path.starts_with(temporary_directory)) {
            warnln("Refusing to run: the profile path {} is outside the test directory {}", path, environment.directory());
            VERIFY_NOT_REACHED();
        }
    }
}

bool allows_egress(WebView::BrowsingSession& session)
{
    return WebView::Application::privacy_policy_coordinator().snapshot_for_session(session).network.allows_network_egress();
}

}

TEST_CASE(without_a_managed_policy_the_network_is_the_users_to_choose)
{
    PrivacyTest::IndependentHTTPServer server(PrivacyTest::IndependentHTTPServer::Protocol::HTTP, PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const&) { return make_response("ok"sv); });
    Test::HeadlessTestEnvironment environment { "TestManagedPolicyAbsent"sv };
    write_policy(environment, {});

    auto app = TRY_OR_FAIL(environment.create_application<ManagedApplication>());

    verify_profile_is_inside(environment);
    EXPECT_EQ(WebView::Application::managed_privacy_policy().status, Privacy::ManagedPolicyStatus::Absent);
    EXPECT(!WebView::Application::browser_options().privacy_strip_tracking_parameters);

    // Positive control: the same page and fixture, which the managed cases below must not reach.
    auto view = WebView::HeadlessWebView::create(load_theme(), { 400, 300 });
    EXPECT_EQ(fetch_from_page(*view, ByteString::formatted("http://127.0.0.1:{}/probe", server.port())), "fetched:ok"sv);
    EXPECT(server.request_count() >= 1);
}

TEST_CASE(a_managed_offline_policy_binds_the_command_line_and_every_identity)
{
    PrivacyTest::IndependentHTTPServer server(PrivacyTest::IndependentHTTPServer::Protocol::HTTP, PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const&) { return make_response("ok"sv); });
    Test::HeadlessTestEnvironment environment { "TestManagedPolicyOffline"sv };
    write_policy(environment, R"({"version":1,"network":{"mode":"offline"},"stripTrackingParameters":true})"sv);

    // The command line asks for direct access; the administrator's policy says no network.
    Array arguments { "--privacy-network-mode=allow"sv };
    auto app = TRY_OR_FAIL(environment.create_application<ManagedApplication>(arguments));
    verify_profile_is_inside(environment);
    EXPECT_EQ(WebView::Application::managed_privacy_policy().status, Privacy::ManagedPolicyStatus::Active);
    // The policy turns tracking-parameter stripping on without its command-line option.
    EXPECT(WebView::Application::browser_options().privacy_strip_tracking_parameters);

    auto& default_session = WebView::Application::default_session();
    EXPECT(!allows_egress(default_session));

    auto work = TRY_OR_FAIL(WebView::IdentityManager::the().create_identity(Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string)));
    EXPECT(!allows_egress(*work));

    // Asking the coordinator to open the network, through each way it can be asked, still leaves it closed.
    auto& coordinator = WebView::Application::privacy_policy_coordinator();
    (void)coordinator.set_network_mode(*work, Privacy::NetworkMode::Allow);
    EXPECT(!allows_egress(*work));
    (void)coordinator.apply_policy_input(*work, { .administrative_ceiling = Privacy::NetworkPolicy::allow_default(), .user_preference = Privacy::NetworkPolicy::allow_default() });
    EXPECT(!allows_egress(*work));
    (void)coordinator.apply_policy_input(default_session, { .user_preference = Privacy::NetworkPolicy::allow_default() });
    EXPECT(!allows_egress(default_session));

    auto default_view = WebView::HeadlessWebView::create(load_theme(), { 400, 300 });
    EXPECT_EQ(fetch_from_page(*default_view, ByteString::formatted("http://127.0.0.1:{}/default", server.port())), "failed"sv);
    auto work_view = WebView::HeadlessWebView::create_for_session(load_theme(), { 400, 300 }, work);
    EXPECT_EQ(fetch_from_page(*work_view, ByteString::formatted("http://127.0.0.1:{}/work", server.port())), "failed"sv);
    EXPECT_EQ(server.connection_count(), 0u);
}

TEST_CASE(a_managed_route_replaces_direct_access)
{
    PrivacyTest::IndependentHTTPServer server(PrivacyTest::IndependentHTTPServer::Protocol::HTTP, PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const&) { return make_response("ok"sv); });
    PrivacyTest::MockRouteServer proxy(PrivacyTest::MockRouteServer::Mode::SOCKS5);
    Test::HeadlessTestEnvironment environment { "TestManagedPolicyRoute"sv };
    auto policy = ByteString::formatted(R"({{"version":1,"network":{{"mode":"proxy-only","proxy":"socks5h://127.0.0.1:{}"}}}})", proxy.port());
    write_policy(environment, policy.view());

    auto app = TRY_OR_FAIL(environment.create_application<ManagedApplication>());

    verify_profile_is_inside(environment);
    auto const& snapshot = WebView::Application::privacy_policy_coordinator().snapshot_for_session(WebView::Application::default_session());
    EXPECT(snapshot.network.requires_mandatory_route());
    EXPECT_EQ(snapshot.network.route->port, proxy.port());

    auto view = WebView::HeadlessWebView::create(load_theme(), { 400, 300 });
    (void)fetch_from_page(*view, ByteString::formatted("http://127.0.0.1:{}/routed", server.port()));
    EXPECT(proxy.connection_count() >= 1);
    EXPECT_EQ(server.connection_count(), 0u);
}

TEST_CASE(a_managed_policy_that_cannot_be_trusted_closes_the_network)
{
    PrivacyTest::IndependentHTTPServer server(PrivacyTest::IndependentHTTPServer::Protocol::HTTP, PrivacyTest::IndependentHTTPServer::Family::IPv4,
        [](ByteString const&) { return make_response("ok"sv); });

    struct Case {
        StringView contents;
        mode_t mode;
    };
    // A file another account could have written, and a field this version doesn't know.
    for (auto const& rejected : Array { Case { R"({"version":1,"network":{"mode":"allow"}})"sv, 0666 }, Case { R"({"version":1,"futureRestriction":true})"sv, 0644 } }) {
        Test::HeadlessTestEnvironment environment { "TestManagedPolicyRejected"sv };
        write_policy(environment, rejected.contents, rejected.mode);

        auto app = TRY_OR_FAIL(environment.create_application<ManagedApplication>());

        verify_profile_is_inside(environment);
        EXPECT_EQ(WebView::Application::managed_privacy_policy().status, Privacy::ManagedPolicyStatus::Rejected);
        EXPECT(!allows_egress(WebView::Application::default_session()));

        auto view = WebView::HeadlessWebView::create(load_theme(), { 400, 300 });
        EXPECT_EQ(fetch_from_page(*view, ByteString::formatted("http://127.0.0.1:{}/rejected", server.port())), "failed"sv);
    }
    EXPECT_EQ(server.connection_count(), 0u);
}
