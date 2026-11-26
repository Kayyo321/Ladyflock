/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <QApplication>
#include <QMenu>
#include <QToolButton>
#include <UI/Qt/ExtensionButtons.h>
#include <UI/Qt/StringUtils.h>

static Hub::ExtensionItem toolbar_item(String id, String title)
{
    return {
        .id = move(id),
        .name = title,
        .title = move(title),
        .badge = "12"_string,
        .badge_color = "#207740"_string,
        .icon = {},
        .has_action = true,
        .enabled = true,
        .pinned = true,
        .has_options = true,
        .has_sidebar = true,
        .sidebar_title = "Sidebar"_string,
        .sidebar_icon = {},
        .sidebar_url = {},
    };
}

TEST_CASE(pinned_actions_show_the_selected_tabs_title_badge_and_enabled_state)
{
    Ladybird::ExtensionButtons buttons(nullptr);
    buttons.set_pin_capacity(1);
    Vector<Hub::ExtensionItem> items;
    items.append(toolbar_item("beta"_string, "Beta"_string));
    items.append(toolbar_item("alpha"_string, "Alpha"_string));
    buttons.set_items(items);
    auto* alpha = qobject_cast<QToolButton*>(&buttons.anchor_for("alpha"_string));
    VERIFY(alpha);
    EXPECT_EQ(alpha->accessibleName(), QString("Alpha"));
    EXPECT_EQ(alpha->accessibleDescription(), QString("12"));
    EXPECT(alpha->focusPolicy() == Qt::StrongFocus);
    EXPECT(!alpha->icon().isNull());
    EXPECT_EQ(buttons.anchor_for("beta"_string).objectName(), QString("LadybirdExtensionsButton"));
    String invoked;
    buttons.on_invoke = [&](String const& id) { invoked = id; };
    alpha->click();
    EXPECT_EQ(invoked, "alpha"sv);

    items[1].title = "Unavailable on this tab"_string;
    items[1].enabled = false;
    items[1].badge = "3"_string;
    buttons.set_items(items);
    EXPECT_EQ(alpha->accessibleName(), QString("Unavailable on this tab"));
    EXPECT_EQ(alpha->accessibleDescription(), QString("3"));
    EXPECT(!alpha->isEnabled());
    buttons.set_pin_capacity(0);
    EXPECT_EQ(buttons.anchor_for("alpha"_string).objectName(), QString("LadybirdExtensionsButton"));
}

TEST_CASE(extensions_menu_keeps_actions_accessible_when_pins_overflow)
{
    Ladybird::ExtensionButtons buttons(nullptr);
    buttons.set_pin_capacity(0);
    buttons.set_items({ toolbar_item("addon"_string, "Search & save"_string) });
    buttons.show();
    auto* launcher = buttons.findChild<QToolButton*>("LadybirdExtensionsButton");
    VERIFY(launcher);
    launcher->click();
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    VERIFY(menu);
    auto* submenu = menu->actions().first()->menu();
    VERIFY(submenu);
    EXPECT_EQ(submenu->title(), QString("Search && save"));
    EXPECT_EQ(submenu->actions().size(), 4);
    String invoked;
    bool pinned = true;
    buttons.on_pin = [&](String const& id, bool state) {
        invoked = id;
        pinned = state;
    };
    auto* pin = submenu->actions().at(1);
    EXPECT(pin->isChecked());
    pin->trigger();
    EXPECT_EQ(invoked, "addon"sv);
    EXPECT(!pinned);

    bool managed = false;
    buttons.on_manage = [&] { managed = true; };
    menu->actions().last()->trigger();
    EXPECT(managed);
    menu->close();
}
