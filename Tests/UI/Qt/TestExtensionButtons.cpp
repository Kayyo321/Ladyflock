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
#include <QFrame>
#include <QMenu>
#include <QToolButton>
#include <UI/Qt/ExtensionButtons.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Theme.h>

// The card is the document's size plus the 2px inset on each side that keeps the view inside its rounded edge.
static QSize card_for_content(QSize content) { return content + QSize(4, 4); }

TEST_CASE(extension_popup_reserves_its_size_for_the_card_and_fits_smaller_screens)
{
    Ladybird::ExtensionPopup popup(nullptr, { 1280, 800 });
    EXPECT_EQ(popup.card().size(), card_for_content({ 380, 420 }));
    EXPECT(popup.width() > popup.card().width());
    EXPECT(popup.height() > popup.card().height());

    // The popover keeps room on the screen for the shadow around its card.
    Ladybird::ExtensionPopup constrained(nullptr, { 320, 360 });
    EXPECT_EQ(constrained.card().size(), card_for_content({ 284, 324 }));
}

TEST_CASE(extension_popup_sizes_to_its_document_within_firefox_bounds)
{
    Ladybird::ExtensionPopup popup(nullptr, { 1440, 900 });
    popup.set_content_size({ 320, 148 });
    EXPECT_EQ(popup.card().size(), card_for_content({ 320, 148 }));
    EXPECT(popup.width() > popup.card().width());

    // Firefox caps a popup at 800x600 CSS pixels; the document scrolls beyond that.
    popup.set_content_size({ 1200, 2400 });
    EXPECT_EQ(popup.card().size(), card_for_content({ 800, 600 }));

    // A scrolling document gets room for its scrollbar, so it doesn't scroll sideways as well.
    popup.set_content_size({ 240, 1800 });
    EXPECT_EQ(popup.card().size(), card_for_content({ 256, 600 }));

    // A nearly empty document still leaves something to see and click.
    popup.set_content_size({ 3, 1 });
    EXPECT_EQ(popup.card().size(), card_for_content(Ladybird::ExtensionPopup::minimum_content_size));

    // A small screen wins over Firefox's maximum.
    Ladybird::ExtensionPopup small(nullptr, { 640, 480 });
    small.set_content_size({ 800, 600 });
    EXPECT_EQ(small.card().size(), card_for_content({ 604, 444 }));
}

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
        .update_version = {},
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

static bool colors_are_close(QColor a, QColor b)
{
    return qAbs(a.red() - b.red()) <= 24 && qAbs(a.green() - b.green()) <= 24 && qAbs(a.blue() - b.blue()) <= 24;
}

TEST_CASE(available_updates_mark_pinned_buttons_and_the_extensions_button)
{
    Ladybird::ExtensionButtons buttons(nullptr);
    buttons.set_pin_capacity(2);
    auto current = toolbar_item("current"_string, "Current"_string);
    current.badge = {};
    auto outdated = toolbar_item("outdated"_string, "Outdated"_string);
    outdated.badge = {};
    outdated.update_version = "2.0"_string;
    buttons.set_items({ current, outdated });
    buttons.show();

    auto& outdated_button = static_cast<QToolButton&>(buttons.anchor_for(outdated.id));
    auto& current_button = static_cast<QToolButton&>(buttons.anchor_for(current.id));
    EXPECT_EQ(outdated_button.accessibleDescription(), QString("Update available: version 2.0"));
    EXPECT(outdated_button.toolTip().contains(QStringLiteral("Update available: version 2.0")));
    EXPECT(current_button.accessibleDescription().isEmpty());

    auto* launcher = buttons.findChild<QToolButton*>("LadybirdExtensionsButton");
    VERIFY(launcher);
    EXPECT_EQ(launcher->accessibleDescription(), QString("1 extension update available"));

    // The hint is an accent circle in the top-right corner, where the extension's badge never goes.
    auto image = outdated_button.grab().toImage();
    QPoint inside_circle(static_cast<int>(outdated_button.width() - 8.5 + 2.5), static_cast<int>(7.5 + 2));
    EXPECT(colors_are_close(image.pixelColor(inside_circle * image.devicePixelRatio()), Ladybird::Theme::accent()));
    auto current_image = current_button.grab().toImage();
    EXPECT(!colors_are_close(current_image.pixelColor(inside_circle * current_image.devicePixelRatio()), Ladybird::Theme::accent()));

    // For visual review: LADYFLOCK_UI_CAPTURE_DIR=<dir> saves what this test draws.
    if (auto directory = qEnvironmentVariable("LADYFLOCK_UI_CAPTURE_DIR"); !directory.isEmpty()) {
        buttons.grab().save(directory + "/extension-buttons-update-hint.png");
        outdated_button.grab().save(directory + "/extension-button-update-hint.png");
    }

    // Updating clears the hint everywhere.
    outdated.update_version = {};
    buttons.set_items({ current, outdated });
    EXPECT(outdated_button.accessibleDescription().isEmpty());
    EXPECT(launcher->accessibleDescription().isEmpty());
}

TEST_CASE(extensions_menu_offers_available_updates)
{
    Ladybird::ExtensionButtons buttons(nullptr);
    buttons.set_pin_capacity(0);
    auto outdated = toolbar_item("outdated"_string, "Outdated"_string);
    outdated.update_version = "2.0"_string;
    auto other = toolbar_item("zulu"_string, "Zulu"_string);
    other.update_version = "3.1"_string;
    buttons.set_items({ outdated, other });
    buttons.show();
    size_t update_requests = 0;
    buttons.on_update = [&] { ++update_requests; };

    buttons.findChild<QToolButton*>("LadybirdExtensionsButton")->click();
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    VERIFY(menu);
    auto* submenu = menu->actions().first()->menu();
    VERIFY(submenu);
    EXPECT(submenu->menuAction()->toolTip().contains(QStringLiteral("Update available: version 2.0")));
    auto* update = submenu->actions().first();
    EXPECT_EQ(update->text(), QString("Update to Version 2.0..."));
    update->trigger();
    EXPECT_EQ(update_requests, 1u);

    // "Manage extensions" stays last, after the action that reviews every update.
    auto actions = menu->actions();
    EXPECT_EQ(actions.at(actions.size() - 2)->text(), QString("Review 2 Updates..."));
    actions.at(actions.size() - 2)->trigger();
    EXPECT_EQ(update_requests, 2u);
    EXPECT_EQ(actions.last()->text(), QString("Manage extensions"));
    menu->close();
}

TEST_CASE(extensions_without_a_usable_icon_get_a_monogram_unlike_the_extensions_button)
{
    Ladybird::ExtensionButtons buttons(nullptr);
    buttons.set_pin_capacity(1);
    buttons.set_items({ toolbar_item("noicon"_string, "Zebra Notes"_string) });
    buttons.show();
    auto& pinned = static_cast<QToolButton&>(buttons.anchor_for("noicon"_string));
    auto* launcher = buttons.findChild<QToolButton*>("LadybirdExtensionsButton");
    VERIFY(launcher);
    EXPECT(!pinned.icon().isNull());
    EXPECT(pinned.icon().pixmap(18, 18).toImage() != launcher->icon().pixmap(18, 18).toImage());

    auto first = Ladybird::ExtensionButtons::monogram_icon(QStringLiteral("Zebra"), Qt::black, Qt::white).pixmap(18, 18).toImage();
    auto second = Ladybird::ExtensionButtons::monogram_icon(QStringLiteral("Apple"), Qt::black, Qt::white).pixmap(18, 18).toImage();
    EXPECT(first != second);
}

TEST_CASE(extensions_menu_elides_long_names_but_keeps_them_in_the_tooltip)
{
    Ladybird::ExtensionButtons buttons(nullptr);
    buttons.set_pin_capacity(0);
    auto name = "An Extension With A Deliberately Very Long Name That Would Otherwise Stretch The Whole Menu"_string;
    buttons.set_items({ toolbar_item("long"_string, name) });
    buttons.show();
    buttons.findChild<QToolButton*>("LadybirdExtensionsButton")->click();
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    VERIFY(menu);
    auto* entry = menu->actions().first();
    EXPECT(entry->text().endsWith(QChar(0x2026)));
    EXPECT(QFontMetrics(menu->font()).horizontalAdvance(entry->text()) <= 280);
    EXPECT(entry->toolTip().contains(qstring_from_ak_string(name)));
    menu->close();
}
