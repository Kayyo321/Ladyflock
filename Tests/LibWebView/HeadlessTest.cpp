/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGamepad/SDLGamepadSource.h>
#include <LibWebView/GamepadManager.h>
#include <time.h>
#include <unistd.h>

namespace Test {

HeadlessApplication::HeadlessApplication(Optional<ByteString> ladybird_binary_path)
    : WebView::Application(move(ladybird_binary_path))
{
    WebView::GamepadManager::the().set_source(make<Gamepad::SDLGamepadSource>(Gamepad::IgnoreSystemGamepads::Yes));
}

void HeadlessApplication::create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options)
{
    browser_options.headless_mode = WebView::HeadlessMode::Test;
    browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
    web_content_options.is_test_mode = WebView::IsTestMode::Yes;
}

HeadlessTestEnvironment::HeadlessTestEnvironment(StringView name, unsigned timeout_seconds)
    : m_directory(ByteString::formatted("{}/Ladybird-{}-{}", Core::StandardPaths::tempfile_directory(), name, generate_random_uuid()))
{
    // Each source suite runs in a fresh process. Keep its deadline even if its event loop stops making progress.
    alarm(timeout_seconds);

    MUST(Core::Directory::create(m_directory, Core::Directory::CreateDirectories::Yes));

    // A test that keeps a persistent profile must keep it here, never in the user's own profile. Each standard
    // location a profile can use is redirected into the test's directory, which goes away with the test.
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, m_directory, Core::Environment::Overwrite::Yes));
    struct Location {
        StringView variable;
        StringView name;
    };
    for (auto const& location : Array { Location { "XDG_DATA_HOME"sv, "data"sv }, Location { "XDG_CACHE_HOME"sv, "cache"sv } }) {
        auto path = LexicalPath::join(m_directory, location.name).string();
        MUST(Core::Directory::create(path, Core::Directory::CreateDirectories::Yes));
        MUST(Core::Environment::set(location.variable, path, Core::Environment::Overwrite::Yes));
    }

    tzset();
    if (auto result = Core::System::set_resource_limits(RLIMIT_NOFILE, 65536); result.is_error())
        warnln("Unable to increase open file limit: {}", result.error());
}

HeadlessTestEnvironment::~HeadlessTestEnvironment()
{
    Core::ThreadEventQueue::destroy_current();
    MUST(FileSystem::remove(m_directory, FileSystem::RecursionMode::Allowed));
    alarm(0);
}

Optional<ByteString> HeadlessTestEnvironment::ladybird_binary_path()
{
#if defined(LADYBIRD_BINARY_PATH)
    return LADYBIRD_BINARY_PATH;
#else
    return {};
#endif
}

}
