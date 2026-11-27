/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibTest/TestCase.h>
#include <LibURL/InternalURLs.h>
#include <LibURL/Parser.h>
#include <UI/Qt/LocationEdit.h>

#include <QAction>
#include <QCoreApplication>
#include <QToolButton>
#include <QWidget>

namespace {

class EmptySuggestionProvider final : public WebView::OmniboxSuggestionProvider {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual void query(WebView::AutocompleteQueryID query_id, String, size_t) override
    {
        on_suggestions(query_id, {}, WebView::AutocompleteResultKind::Final);
    }

    virtual void cancel() override { }
};

}

TEST_CASE(zoom_indicator_hides_while_editing_or_when_the_url_is_hidden)
{
    auto settings = WebView::Settings::create_from_json(JsonObject {});
    QWidget window;
    Ladybird::LocationEdit field(&window, settings, make<EmptySuggestionProvider>());
    window.show();
    window.activateWindow();
    QCoreApplication::processEvents();
    field.clearFocus();
    auto* zoom = new QAction("110%", &field);
    field.set_zoom_action(zoom);
    auto* zoom_button = field.findChild<QToolButton*>("LadybirdLocationZoomIndicator");
    VERIFY(zoom_button);
    EXPECT(zoom_button->isVisible());

    field.setFocus();
    EXPECT(field.hasFocus());
    EXPECT(zoom_button->isHidden());
    zoom->setText("120%");
    EXPECT(zoom_button->isHidden());

    field.clearFocus();
    EXPECT(zoom_button->isVisible());
    EXPECT(zoom_button->text() == QStringLiteral("120%"));

    field.set_url_is_hidden(true);
    EXPECT(!field.is_editing_location());
    EXPECT(zoom_button->isHidden());
    zoom->setText("130%");
    EXPECT(zoom_button->isHidden());

    field.set_url_is_hidden(false);
    EXPECT(zoom_button->isVisible());
    EXPECT(zoom_button->text() == QStringLiteral("130%"));
}

TEST_CASE(new_tab_dashboard_hides_address_while_blank_srcdoc_and_websites_stay_visible)
{
    auto settings = WebView::Settings::create_from_json(JsonObject {});
    QWidget window;
    Ladybird::LocationEdit field(&window, settings, make<EmptySuggestionProvider>());
    window.show();
    window.activateWindow();
    QCoreApplication::processEvents();
    field.clearFocus();

    // about:newtab and about:dashboard are trusted internal new tab pages and keep the field empty.
    field.set_url(URL::about_newtab());
    EXPECT(field.text().isEmpty());
    field.setFocus();
    field.set_url(URL::about_newtab());
    EXPECT(field.text().isEmpty());

    field.clearFocus();
    field.set_url(URL::about_dashboard());
    EXPECT(field.text().isEmpty());
    field.setFocus();
    field.set_url(URL::about_dashboard());
    EXPECT(field.text().isEmpty());

    // A configured custom website still displays its origin/URL.
    auto website = URL::Parser::basic_parse("https://example.com/"sv).release_value();
    settings.set_new_tab_page_url(website);
    field.clearFocus();
    field.set_url(website);
    EXPECT(!field.text().isEmpty());
    field.setFocus();
    field.set_url(website);
    EXPECT(field.text() == QStringLiteral("https://example.com/"));

    // about:blank and about:srcdoc remain visible even when explicitly configured as the new tab page.
    field.clearFocus();
    settings.set_new_tab_page_url(URL::about_blank());
    field.set_url(URL::about_blank());
    EXPECT(field.text() == QStringLiteral("about:blank"));

    settings.set_new_tab_page_url(URL::about_srcdoc());
    field.set_url(URL::about_srcdoc());
    EXPECT(field.text() == QStringLiteral("about:srcdoc"));
}
