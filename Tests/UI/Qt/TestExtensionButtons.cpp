/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGfx/Bitmap.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Settings.h>
#include <QApplication>
#include <QMenu>
#include <QToolButton>
#include <UI/Qt/ExtensionButtons.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Theme.h>

static Hub::ExtensionItem toolbar_item(String id, String title)
{
    return {
        .id = move(id),
        .name = title,
        .title = move(title),
        .badge = "12"_string,
        .badge_color = "#207740"_string,
        .icon = {},
        .light_icon = {},
        .dark_icon = {},
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
    EXPECT(!submenu->icon().isNull());
    EXPECT(submenu->menuAction()->isIconVisibleInMenu());
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

static Gfx::ShareableBitmap solid_icon(Gfx::Color premultiplied_color)
{
    auto bitmap = MUST(Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 16, 16 }));
    for (int y = 0; y < bitmap->height(); ++y) {
        for (int x = 0; x < bitmap->width(); ++x)
            bitmap->set_pixel(x, y, premultiplied_color);
    }
    return bitmap->to_shareable_bitmap();
}

TEST_CASE(pinned_and_menu_icons_preserve_translucent_publisher_colors)
{
    Ladybird::ExtensionButtons buttons(nullptr);
    buttons.set_pin_capacity(1);
    auto item = toolbar_item("addon"_string, "Paint"_string);
    item.icon = solid_icon(Gfx::Color(128, 0, 64, 128));
    buttons.set_items({ item });
    buttons.show();
    buttons.update_icons();
    auto& pinned = static_cast<QToolButton&>(buttons.anchor_for(item.id));
    auto color = pinned.icon().pixmap(16, 16).toImage().pixelColor(8, 8);
    EXPECT_EQ(color.rgba(), qRgba(255, 0, 128, 128));
    buttons.findChild<QToolButton*>("LadybirdExtensionsButton")->click();
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    VERIFY(menu);
    EXPECT_EQ(menu->actions().first()->icon().pixmap(16, 16).toImage().pixelColor(8, 8), color);
    item.icon = solid_icon(Gfx::Color::Blue);
    buttons.set_items({ item });
    EXPECT_EQ(menu->actions().first()->icon().pixmap(16, 16).toImage().pixelColor(8, 8), QColor(Qt::blue));
    menu->close();
}

TEST_CASE(publisher_theme_icons_follow_the_toolbar_appearance)
{
    Ladybird::ExtensionButtons buttons(nullptr);
    buttons.set_pin_capacity(1);
    auto item = toolbar_item("addon"_string, "Contrast"_string);
    item.light_icon = solid_icon(Gfx::Color::White);
    item.dark_icon = solid_icon(Gfx::Color::Black);
    Ladybird::Theme::set_interface_theme(WebView::ThemePreference::Light);
    ScopeGuard restore_theme = [] { Ladybird::Theme::set_interface_theme(WebView::ThemePreference::System); };
    buttons.set_items({ item });
    auto& pinned = static_cast<QToolButton&>(buttons.anchor_for(item.id));
    EXPECT_EQ(pinned.icon().pixmap(16, 16).toImage().pixelColor(8, 8), QColor(Qt::black));
    Ladybird::Theme::set_interface_theme(WebView::ThemePreference::Dark);
    buttons.update_icons();
    EXPECT_EQ(pinned.icon().pixmap(16, 16).toImage().pixelColor(8, 8), QColor(Qt::white));
    buttons.show();
    buttons.findChild<QToolButton*>("LadybirdExtensionsButton")->click();
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    VERIFY(menu);
    auto palette = menu->palette();
    palette.setColor(QPalette::WindowText, Qt::black);
    menu->setPalette(palette);
    buttons.update_icons();
    EXPECT_EQ(menu->actions().first()->icon().pixmap(16, 16).toImage().pixelColor(8, 8), QColor(Qt::black));
    menu->close();
}
