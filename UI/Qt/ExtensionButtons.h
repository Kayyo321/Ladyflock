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
};

class ExtensionButtons final : public QWidget {
public:
    ExtensionButtons(QWidget* parent);
    void set_items(Vector<Hub::ExtensionItem>);
    void set_pin_capacity(int);
    void update_icons();
    static QIcon package_icon(Optional<Gfx::ShareableBitmap> const&);
    void close_menu();
    QWidget& anchor_for(String const&) const;

    Function<void(String const&)> on_invoke;
    Function<void(String const&, bool)> on_pin;
    Function<void(String const&)> on_sidebar;
    Function<void(String const&)> on_options;
    Function<void()> on_manage;

private:
    void update_buttons();
    void open_menu();

    QHBoxLayout* m_layout { nullptr };
    QToolButton* m_menu_button { nullptr };
    QPointer<QMenu> m_menu;
    Vector<Hub::ExtensionItem> m_items;
    int m_pin_capacity { 0 };
};

}
