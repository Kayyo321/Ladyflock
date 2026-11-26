/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/Utf16String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

// A view that sizes to its content, resized to each size it reports within Firefox's 800x600 popup bounds, the way the
// toolbar's popup follows its document.
struct PopupHarness {
    NonnullOwnPtr<WebView::HeadlessWebView> view;
    Vector<Gfx::IntSize> reports;
    Optional<Utf16String> title;

    static ErrorOr<PopupHarness> create()
    {
        auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
        auto theme = TRY(Gfx::load_system_theme(theme_path.string()));
        PopupHarness harness { WebView::HeadlessWebView::create(move(theme), { 380, 420 }), {}, {} };
        return harness;
    }

    void start()
    {
        size_t loads_finished = 0;
        view->on_load_finish = [&](auto const&) { ++loads_finished; };
        Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });
        view->on_load_finish = nullptr;
        view->on_title_change = [this](Utf16String const& new_title) { title = new_title; };
        view->on_preferred_size_change = [this](Gfx::IntSize size) {
            reports.append(size);
            view->reset_viewport_size({ min(size.width(), 800), min(size.height(), 600) });
        };
        view->set_sizes_to_content(true);
    }

    void load(StringView html)
    {
        title.clear();
        view->load_html(html);
        Core::EventLoop::current().spin_until([&]() { return title.has_value(); });
    }

    static void wait(int milliseconds)
    {
        bool done = false;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(milliseconds), [&] { done = true; });
        timer->start();
        Core::EventLoop::current().spin_until([&]() { return done; });
    }
};

// A popup whose root is as tall as the viewport overflows it by the body's margins at any height. It grows to fit them
// once and settles, rather than growing step by step until it reaches the 600px limit.
TEST_CASE(full_height_popup_settles_instead_of_growing_to_the_limit)
{
    Test::HeadlessTestEnvironment environment { "TestExtensionPopupSizing"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto harness = TRY_OR_FAIL(PopupHarness::create());
    harness.start();

    harness.load(R"html(<!doctype html>
<style>html, body { height: 100%; } body { width: 260px; }</style>
<p>Full height</p>
<script>document.title = "loaded";</script>)html"sv);
    Core::EventLoop::current().spin_until([&]() { return !harness.reports.is_empty(); });
    PopupHarness::wait(750);

    auto last = harness.reports.last();
    EXPECT_EQ(last.width(), 276);
    EXPECT(last.height() < 600);
    EXPECT(harness.reports.size() <= 3);
}

// Only the popup's own document lays out at its preferred width. A same-origin frame inside it keeps the width the
// popup gives it, so its text wraps there instead of running on and scrolling sideways.
TEST_CASE(frames_in_a_popup_keep_the_width_their_embedder_gives_them)
{
    Test::HeadlessTestEnvironment environment { "TestExtensionPopupSizing"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto harness = TRY_OR_FAIL(PopupHarness::create());
    harness.start();

    harness.load(R"html(<!doctype html>
<style>body { margin: 0; } iframe { display: block; width: 300px; height: 120px; border: 0; }</style>
<iframe srcdoc="<style>body { margin: 0 }</style><p>A paragraph long enough that at its preferred width it would be far wider than the three hundred pixel frame it sits in.</p>"></iframe>
<script>
    const frame = document.querySelector("iframe");
    frame.onload = () => {
        requestAnimationFrame(() => requestAnimationFrame(() => {
            const root = frame.contentDocument.documentElement;
            document.title = "frame " + Math.round(root.getBoundingClientRect().width) + " " + root.scrollWidth;
        }));
    };
</script>)html"sv);
    Core::EventLoop::current().spin_until([&]() { return harness.title.has_value() && harness.title->to_utf8().starts_with_bytes("frame "sv); });

    EXPECT_EQ(harness.title->to_utf8(), "frame 300 300"_string);
    Core::EventLoop::current().spin_until([&]() { return !harness.reports.is_empty(); });
    EXPECT_EQ(harness.reports.last().width(), 300);
}
