/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Math.h>
#include <AK/ScopeGuard.h>
#include <AK/StdLibExtras.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/LadybirdButton.h>
#include <UI/Qt/Theme.h>

#include <QImage>
#include <QPainter>
#include <QPixmap>

static bool has_visible_pixel(QPixmap const& pixmap)
{
    auto image = pixmap.toImage();
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0)
                return true;
        }
    }
    return false;
}

static float wcag_contrast_ratio(QColor const& a, QColor const& b)
{
    auto luminance = [](QColor const& color) {
        auto linear = [](float channel) { return channel <= 0.04045f ? channel / 12.92f : AK::pow((channel + 0.055f) / 1.055f, 2.4f); };
        return 0.2126f * linear(color.redF()) + 0.7152f * linear(color.greenF()) + 0.0722f * linear(color.blueF());
    };
    auto lighter = max(luminance(a), luminance(b));
    auto darker = min(luminance(a), luminance(b));
    return (lighter + 0.05f) / (darker + 0.05f);
}

// Every icon is drawn at each device pixel ratio, which takes the 1x screen's own drawings for the icons that have them.
TEST_CASE(every_chrome_icon_draws_at_every_device_pixel_ratio)
{
    for (auto value = to_underlying(Ladybird::ChromeIcon::Back); value <= to_underlying(Ladybird::ChromeIcon::WindowClose); ++value) {
        auto icon = static_cast<Ladybird::ChromeIcon>(value);
        for (auto device_pixel_ratio : Ladybird::ICON_DEVICE_PIXEL_RATIOS) {
            auto pixmap = Ladybird::create_chrome_icon_pixmap(icon, QColor(0, 0, 0), device_pixel_ratio);
            EXPECT(has_visible_pixel(pixmap));
        }
    }
}

TEST_CASE(ladyflock_logomark_renders_at_every_device_pixel_ratio)
{
    for (auto device_pixel_ratio : Ladybird::ICON_DEVICE_PIXEL_RATIOS) {
        QPixmap pixmap(20 * device_pixel_ratio, 17 * device_pixel_ratio);
        pixmap.setDevicePixelRatio(device_pixel_ratio);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        Ladybird::paint_logomark(painter, QRectF(0, 0, 20, 16.3), QColor(0x4b, 0x22, 0xad));
        painter.end();

        EXPECT(has_visible_pixel(pixmap));
    }
}

TEST_CASE(logomark_and_page_mark_palettes_meet_contrast_across_all_themes)
{
    ScopeGuard restore = [] {
        Ladybird::Theme::set_interface_theme(WebView::ThemePreference::System);
        Ladybird::Theme::set_color_theme(WebView::ColorTheme::Neutral);
    };

    constexpr Array themes {
        WebView::ColorTheme::Neutral,
        WebView::ColorTheme::Harbor,
        WebView::ColorTheme::Glacier,
        WebView::ColorTheme::Lagoon,
        WebView::ColorTheme::Moss,
        WebView::ColorTheme::Saffron,
        WebView::ColorTheme::Copper,
        WebView::ColorTheme::Cherry,
        WebView::ColorTheme::Blossom,
        WebView::ColorTheme::Lavender,
    };

    Ladybird::Theme::set_color_theme(WebView::ColorTheme::Neutral);
    auto neutral_light = Ladybird::Theme::page_mark_colors(false);
    auto neutral_dark = Ladybird::Theme::page_mark_colors(true);
    EXPECT(neutral_light.deep == QColor(0x4d, 0x0b, 0x9e));
    EXPECT(neutral_dark.bird == QColor(0xd3, 0xb8, 0xff));

    for (auto theme : themes) {
        Ladybird::Theme::set_color_theme(theme);
        for (auto preference : { WebView::ThemePreference::Light, WebView::ThemePreference::Dark }) {
            Ladybird::Theme::set_interface_theme(preference);
            auto dark = preference == WebView::ThemePreference::Dark;
            auto const& chrome = Ladybird::Theme::chrome();
            auto mark = Ladybird::Theme::logomark();
            EXPECT(wcag_contrast_ratio(mark, chrome.strip) >= 4.5f);
            EXPECT(wcag_contrast_ratio(mark, chrome.toolbar) >= 4.5f);

            auto page_bg = dark ? QColor(0x1c, 0x1c, 0x1e) : QColor(0xff, 0xff, 0xff);
            auto page_mark = Ladybird::Theme::page_mark_colors(dark);
            EXPECT(wcag_contrast_ratio(page_mark.bird, page_bg) >= 7.0f);
            EXPECT(wcag_contrast_ratio(page_mark.solid, page_bg) >= 7.0f);
            EXPECT(wcag_contrast_ratio(dark ? page_mark.mid : page_mark.deep, page_bg) >= 4.5f);
            if (theme != WebView::ColorTheme::Neutral)
                EXPECT(page_mark.deep != (dark ? neutral_dark.deep : neutral_light.deep));
        }
    }
}

TEST_CASE(ladyflock_menu_button_labels_and_renders)
{
    Ladybird::LadybirdButton regular_button(WebView::IsPrivate::No, nullptr);
    EXPECT(regular_button.accessibleName() == QStringLiteral("Ladyflock menu"));
    EXPECT(regular_button.toolTip() == QStringLiteral("Ladyflock menu"));
    EXPECT(has_visible_pixel(regular_button.grab()));

    Ladybird::LadybirdButton private_button(WebView::IsPrivate::Yes, nullptr);
    EXPECT(private_button.accessibleName() == QStringLiteral("Ladyflock menu, private window"));
    EXPECT(private_button.toolTip() == QStringLiteral("Ladyflock menu, private window"));
    EXPECT(has_visible_pixel(private_button.grab()));
}
