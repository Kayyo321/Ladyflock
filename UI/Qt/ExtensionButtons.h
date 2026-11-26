/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <Hub/HubClientEndpoint.h>
#include <QPointer>
#include <QWidget>
#include <UI/Qt/Popover.h>

class QHBoxLayout;
class QMenu;
class QToolButton;

namespace Ladybird {

class ExtensionPopup final : public Popover {
public:
    ExtensionPopup(QWidget* parent, QSize available_size);

    // Firefox sizes a popup to its document's preferred size, within these bounds in CSS pixels.
    static constexpr QSize minimum_content_size { 25, 25 };
    static constexpr QSize maximum_content_size { 800, 600 };

    // Fits the card around a document of the given size, clamped to the bounds above and the screen.
    void set_content_size(QSize);
    static QSize clamped_content_size(QSize, QSize available_size);

    // A popup opens at most once, when its size is first known, so a late size report can't reopen a closed one.
    bool has_been_revealed() const { return m_revealed; }
    void mark_revealed() { m_revealed = true; }

private:
    QSize m_available_size;
    bool m_revealed { false };
};

class ExtensionButtons final : public QWidget {
public:
    ExtensionButtons(QWidget* parent);
    void set_items(Vector<Hub::ExtensionItem>);
    void set_pin_capacity(int);
    void update_icons();
    static QIcon package_icon(Optional<Gfx::ShareableBitmap> const&);
    // A placeholder for an extension without an icon Ladyflock can show: the first letter of its name on a tile, so
    // that it can't be mistaken for the Extensions button or for another extension.
    static QIcon monogram_icon(QString const& name, QColor tile, QColor letter);
    // Marks a button or icon whose extension has a newer version: an up arrow in an accent circle at its top-right,
    // cut out of the surface it sits on.
    static void paint_update_hint(QPainter&, QPointF center, qreal radius, QColor surface);
    void close_menu();
    QWidget& anchor_for(String const&) const;

    Function<void(String const&)> on_invoke;
    Function<void(String const&, bool)> on_pin;
    Function<void(String const&)> on_sidebar;
    Function<void(String const&)> on_options;
    Function<void()> on_manage;
    // Opens the Add-ons page to review and install available updates.
    Function<void()> on_update;

private:
    void update_buttons();
    void open_menu();
    int count_updates() const;

    QHBoxLayout* m_layout { nullptr };
    QToolButton* m_menu_button { nullptr };
    QPointer<QMenu> m_menu;
    Vector<Hub::ExtensionItem> m_items;
    int m_pin_capacity { 0 };
};

}
