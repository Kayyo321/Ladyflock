/*
 * Copyright (c) 2026, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/InspectorHost.h>
#include <UI/Qt/Menu.h>

#include <QAction>
#include <QApplication>
#include <QKeyEvent>

TEST_CASE(inspector_splitter_can_be_resized_and_equalized_from_the_keyboard)
{
    Ladybird::InspectorSplitter splitter;
    QWidget page;
    QWidget inspector;
    splitter.addWidget(&page);
    splitter.addWidget(&inspector);
    splitter.resize(801, 601);
    splitter.show();
    QApplication::processEvents();

    auto* handle = splitter.handle(1);
    VERIFY(handle);
    EXPECT(handle->focusPolicy() == Qt::StrongFocus);
    for (auto orientation : { Qt::Horizontal, Qt::Vertical }) {
        splitter.setOrientation(orientation);
        splitter.setSizes({ 500, 100 });
        QKeyEvent equalize(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
        QApplication::sendEvent(handle, &equalize);
        auto sizes = splitter.sizes();
        EXPECT(abs(sizes.first() - sizes.last()) <= 1);

        QKeyEvent grow(QEvent::KeyPress, orientation == Qt::Horizontal ? Qt::Key_Right : Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(handle, &grow);
        EXPECT_EQ(splitter.sizes().first(), sizes.first() + 16);
    }
}

TEST_CASE(local_inspector_shortcuts_dispatch_without_toggling_remote_debugging)
{
    QWidget window;
    int inspector_count = 0;
    int picker_count = 0;
    int console_count = 0;
    int remote_count = 0;
    auto install = [&](Ladybird::ActionID id, int& count) {
        auto action = Ladybird::Action::create("Test action"sv, id, [&count] { ++count; });
        auto* native = Ladybird::create_application_action(window, *action, Ladybird::IncludeActionIcon::No);
        window.addAction(native);
        return action;
    };
    auto inspector = install(Ladybird::ActionID::ToggleInspector, inspector_count);
    auto picker = install(Ladybird::ActionID::ToggleInspectorPicker, picker_count);
    auto console = install(Ladybird::ActionID::OpenInspectorConsole, console_count);
    auto remote = install(Ladybird::ActionID::ToggleDevTools, remote_count);
    window.show();
    window.activateWindow();
    QApplication::processEvents();
#if defined(AK_OS_MACOS)
    constexpr auto modifiers = Qt::ControlModifier | Qt::AltModifier;
#else
    constexpr auto modifiers = Qt::ControlModifier | Qt::ShiftModifier;
#endif
    auto press = [&](Qt::Key key, Qt::KeyboardModifiers mods) {
        QKeyEvent event(QEvent::KeyPress, key, mods);
        QApplication::sendEvent(&window, &event);
    };
    press(Qt::Key_F12, Qt::NoModifier);
    press(Qt::Key_I, modifiers);
    press(Qt::Key_C, modifiers);
    press(Qt::Key_J, modifiers);
    EXPECT_EQ(inspector_count, 2);
    EXPECT_EQ(picker_count, 1);
    EXPECT_EQ(console_count, 1);
    EXPECT_EQ(remote_count, 0);
}

TEST_CASE(close_returns_focus_to_the_opener_or_the_page_when_the_opener_is_hidden)
{
    QWidget window;
    QWidget page(&window);
    QWidget opener(&window);
    QWidget inspector(&window);
    window.show();
    window.activateWindow();
    QApplication::processEvents();
    inspector.setFocus();
    EXPECT(inspector.hasFocus());

    Ladybird::InspectorHost::restore_focus_after_close(page, &opener);
    EXPECT(opener.hasFocus());

    inspector.setFocus();
    opener.hide();
    Ladybird::InspectorHost::restore_focus_after_close(page, &opener);
    EXPECT(page.hasFocus());
}
