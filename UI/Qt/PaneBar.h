/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/kmalloc.h>

#include <QPointer>
#include <QToolButton>
#include <QWidget>

class QMenu;
class QTimer;
class QVariantAnimation;

namespace Ladybird {

class Tab;

// Each side of a shown pair has a bar above its page. The toolbar describes only the focused side, so the bar is where
// the other side's site shows, and where the person can tell the sides apart.
class PaneBar final : public QWidget {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    enum class State : u8 {
        FocusedSide,
        OtherSide,
        // The window is too narrow for both sides, so only this one shows.
        Fallback,
    };

    // What the side waits on the person for, while it is not the focused side.
    enum class Pending : u8 {
        PermissionRequest,
        Dialog,
        Crashed,
    };

    explicit PaneBar(Tab&);

    void set_state(State);
    State state() const { return m_state; }
    void set_pending(Optional<Pending>);
    Optional<Pending> pending() const { return m_pending; }
    void set_toolbar_in_sidebar(bool);
    void set_partner_title(QString const&);
    void page_changed();

    virtual QSize sizeHint() const override;
    virtual QSize minimumSizeHint() const override;

signals:
    void focus_side_requested();
    void focus_address_requested();
    void show_other_side_requested();
    void pending_chip_clicked();
    void swap_sides_requested();
    void separate_tabs_requested();
    void move_to_new_window_requested();
    void close_tab_requested();

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual bool event(QEvent*) override;

private:
    void layout_buttons();
    void update_loading_timer();
    QRect origin_rect() const;
    QString pending_text() const;

    Tab& m_tab;
    State m_state { State::FocusedSide };
    Optional<Pending> m_pending;
    bool m_toolbar_in_sidebar { false };
    bool m_press_was_on_focused_origin { false };
    // NB: A press on the other side's bar focuses the side before the bar sees the press, which is then no second click.
    bool m_just_became_focused { false };

    QToolButton* m_menu_button { nullptr };
    QToolButton* m_show_other_side_button { nullptr };
    QToolButton* m_pending_chip { nullptr };
    QMenu* m_menu { nullptr };
    QTimer* m_loading_timer { nullptr };
    qreal m_opacity { 1 };
    QVariantAnimation* m_fade_in { nullptr };
};

}
