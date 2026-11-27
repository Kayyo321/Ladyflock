/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>

#include <QPointer>
#include <QWidget>

class QVariantAnimation;

namespace Ladybird {

class Tab;

// Split view shows two tabs of a window side by side. Each half of a split is a side, and the two tabs in one split are
// a pair. The start side is on the left in left-to-right locales.
namespace SplitView {

// Each side is at least this wide while both show. Below that, only the focused side shows (the fallback).
constexpr inline int MINIMUM_SIDE_WIDTH = 320;
// The gutter between the sides, which is also the divider's hit area.
constexpr inline int DIVIDER_WIDTH = 8;
// Both sides show again once the area is this much wider than it needs to be, so the layout doesn't flap while the
// window's edge is dragged across the threshold.
constexpr inline int FALLBACK_HYSTERESIS = 24;
// A divider dragged within this distance of the middle settles at the middle.
constexpr inline int SNAP_DISTANCE = 12;
// Each side of a shown pair has a bar above its page.
constexpr inline int PANE_BAR_HEIGHT = 32;
// How far the arrow keys move the divider, as a share of the room for both pages.
constexpr inline float KEYBOARD_STEP = 0.05f;
constexpr inline float FINE_KEYBOARD_STEP = 0.01f;

constexpr inline int side_by_side_threshold() { return 2 * MINIMUM_SIDE_WIDTH + DIVIDER_WIDTH; }

// Whether both sides of a pair show, in an area this wide, given whether they showed before.
bool shows_side_by_side(int available_width, bool showed_side_by_side_before);

// The share of the width for both pages that the start side prefers. It stays as the person left it however the window
// is sized, and only the displayed widths are clamped to it.
float sanitized_start_fraction(Optional<float>);

struct SideWidths {
    int start { 0 };
    int end { 0 };
};
// How wide each side shows in an area this wide, which must be at least side_by_side_threshold().
SideWidths side_widths(int available_width, float start_fraction);

// Where the divider settles when it is released at a position, given as the start side's width.
float start_fraction_for_divider_position(int start_width, int available_width);

}

// The two tabs of a pair, which always sit next to each other in the strip, start first.
struct SplitPair {
    u64 id { 0 };
    float start_fraction { 0.5f };
    QPointer<Tab> start;
    QPointer<Tab> end;

    bool contains(Tab const*) const;
    Tab* partner_of(Tab const* tab) const;
};

class SideDivider;

// The area that shows the window's pages. It holds every tab of the window in strip order, so tab(i) of the strip is
// widget(i) here, and pages move into and out of a pair without being reparented. It shows the current tab across the
// whole area, or the current tab's pair side by side.
class PageArea final : public QWidget {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit PageArea(QWidget* parent);

    int count() const { return static_cast<int>(m_pages.size()); }
    Tab* widget(int index) const;
    int indexOf(Tab const*) const;
    void insertWidget(int index, Tab*);
    void removeWidget(Tab*);
    void setCurrentIndex(int index);
    int currentIndex() const;
    Tab* currentWidget() const;

    // The pair the current tab belongs to, shown side by side where there is room.
    void set_shown_pair(SplitPair const*);
    void set_page_fullscreen_tab(Tab*);

    // While the tabs change in bulk, the pages are laid out once at the end.
    void set_layout_enabled(bool);
    bool layout_enabled() const { return m_layout_enabled; }

    // The pages on display, start side first.
    Vector<Tab*> displayed_tabs() const;
    bool is_displayed(Tab const*) const;
    bool shows_side_by_side() const { return m_shows_side_by_side; }
    // Whether the current tab's pair is narrowed to one side.
    bool shows_fallback() const;

    // The rect a page would get if it were shown, for sizing a page that hasn't been shown yet.
    QRect slot_rect_for(Tab const&, SplitPair const* pair_of_tab) const;

    SideDivider& divider() const { return *m_divider; }

    void relayout();

signals:
    // The pages on display changed, after the area laid them out.
    void displayed_tabs_changed();
    // The divider was released at a new place.
    void start_fraction_changed(float);

protected:
    virtual void resizeEvent(QResizeEvent*) override;
    virtual bool event(QEvent*) override;

private:
    QRect contents_rect_for_pages() const;
    bool is_right_to_left() const;

    Vector<QPointer<Tab>> m_pages;
    QPointer<Tab> m_current;
    QPointer<Tab> m_fullscreen_tab;
    Optional<SplitPair> m_shown_pair;
    bool m_shows_side_by_side { false };
    bool m_layout_enabled { true };
    bool m_relayout_pending { false };
    Vector<QPointer<Tab>> m_last_displayed;
    SideDivider* m_divider { nullptr };
};

// The gutter between the sides of a shown pair. Dragging it shows where the sides will meet, and the pages take their
// new widths once it is released.
class SideDivider final : public QWidget {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit SideDivider(PageArea&);

    // The room both pages share, and where the divider sits in the area, for a drag to be measured against.
    void set_split_geometry(int pages_left, int available_width, int start_width, float start_fraction, bool right_to_left);
    float start_fraction() const { return m_start_fraction; }

    virtual QSize sizeHint() const override;

signals:
    void start_fraction_committed(float);

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void enterEvent(QEnterEvent*) override;
    virtual void leaveEvent(QEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void mouseDoubleClickEvent(QMouseEvent*) override;
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual void focusInEvent(QFocusEvent*) override;
    virtual void focusOutEvent(QFocusEvent*) override;
    virtual bool event(QEvent*) override;

private:
    void commit(float);
    void cancel_drag();
    void update_drag_line(int start_width);
    int start_width_for_global_x(int global_x) const;
    void set_grip_opacity(qreal);

    PageArea& m_page_area;
    int m_pages_left { 0 };
    int m_available_width { 0 };
    int m_start_width { 0 };
    float m_start_fraction { 0.5f };
    bool m_right_to_left { false };

    bool m_is_pressed { false };
    bool m_is_dragging { false };
    bool m_is_hovered { false };
    bool m_has_keyboard_focus { false };
    int m_press_global_x { 0 };
    int m_drag_start_width { 0 };
    // NB: While the divider is dragged, a line shows where the sides will meet. The pages only take that width once the
    //     divider is released, as a page's native surface relays out on every size.
    QWidget* m_drag_line { nullptr };
    QObject* m_escape_filter { nullptr };
    qreal m_grip_opacity { 0 };
    QVariantAnimation* m_grip_animation { nullptr };
};

}
