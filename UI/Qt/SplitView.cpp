/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/HiddenPageSizing.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/SplitView.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WebContentView.h>

#include <QAccessible>
#include <QAccessibleValueInterface>
#include <QAccessibleWidget>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QVariantAnimation>

namespace Ladybird {

namespace SplitView {

bool shows_side_by_side(int available_width, bool showed_side_by_side_before)
{
    if (showed_side_by_side_before)
        return available_width >= side_by_side_threshold();
    return available_width >= side_by_side_threshold() + FALLBACK_HYSTERESIS;
}

float sanitized_start_fraction(Optional<float> fraction)
{
    if (!fraction.has_value() || !isfinite(*fraction) || *fraction <= 0.0f || *fraction >= 1.0f)
        return 0.5f;
    return *fraction;
}

SideWidths side_widths(int available_width, float start_fraction)
{
    auto room = max(0, available_width - DIVIDER_WIDTH);
    auto start = static_cast<int>(lroundf(room * sanitized_start_fraction(start_fraction)));
    // The preferred share stays as it is. Only what shows is clamped, so that each side keeps its minimum width.
    start = clamp(start, min(MINIMUM_SIDE_WIDTH, room / 2), max(room - MINIMUM_SIDE_WIDTH, room / 2));
    return { start, room - start };
}

float start_fraction_for_divider_position(int start_width, int available_width)
{
    auto room = max(1, available_width - DIVIDER_WIDTH);
    start_width = clamp(start_width, min(MINIMUM_SIDE_WIDTH, room / 2), max(room - MINIMUM_SIDE_WIDTH, room / 2));
    if (abs(start_width - (room / 2)) <= SNAP_DISTANCE)
        return 0.5f;
    return static_cast<float>(start_width) / static_cast<float>(room);
}

}

bool SplitPair::contains(Tab const* tab) const
{
    return tab && (start == tab || end == tab);
}

Tab* SplitPair::partner_of(Tab const* tab) const
{
    if (!tab)
        return nullptr;
    if (start == tab)
        return end;
    if (end == tab)
        return start;
    return nullptr;
}

PageArea::PageArea(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("LadybirdPageArea");
    m_divider = new SideDivider(*this);
    m_divider->hide();
    connect(m_divider, &SideDivider::start_fraction_committed, this, &PageArea::start_fraction_changed);
}

Tab* PageArea::widget(int index) const
{
    if (index < 0 || index >= count())
        return nullptr;
    return m_pages[index];
}

int PageArea::indexOf(Tab const* tab) const
{
    if (!tab)
        return -1;
    for (int i = 0; i < count(); ++i) {
        if (m_pages[i] == tab)
            return i;
    }
    return -1;
}

void PageArea::insertWidget(int index, Tab* tab)
{
    VERIFY(tab);
    if (auto existing = indexOf(tab); existing >= 0)
        m_pages.remove(existing);
    index = clamp(index, 0, count());
    m_pages.insert(index, tab);
    if (tab->parentWidget() != this)
        tab->setParent(this);
    // A page that isn't on display stays hidden until the layout shows it, as in a stacked widget. A page on display, such
    // as a side that only moves in the strip, stays shown.
    if (tab != m_current && !is_displayed(tab))
        tab->hide();
    relayout();
}

void PageArea::removeWidget(Tab* tab)
{
    auto index = indexOf(tab);
    if (index < 0)
        return;
    m_pages.remove(index);
    if (m_current == tab)
        m_current = nullptr;
    if (m_fullscreen_tab == tab)
        m_fullscreen_tab = nullptr;
    if (m_shown_pair.has_value() && m_shown_pair->contains(tab))
        m_shown_pair.clear();
    tab->set_split_presentation(Tab::SplitPresentation::None);
    relayout();
}

void PageArea::setCurrentIndex(int index)
{
    auto* tab = widget(index);
    if (!tab || tab == m_current)
        return;
    m_current = tab;
    relayout();
}

int PageArea::currentIndex() const
{
    return indexOf(m_current);
}

Tab* PageArea::currentWidget() const
{
    return m_current;
}

void PageArea::set_shown_pair(SplitPair const* pair)
{
    if (pair && pair->start && pair->end)
        m_shown_pair = *pair;
    else
        m_shown_pair.clear();
    relayout();
}

void PageArea::set_page_fullscreen_tab(Tab* tab)
{
    if (m_fullscreen_tab == tab)
        return;
    m_fullscreen_tab = tab;
    relayout();
}

void PageArea::set_layout_enabled(bool enabled)
{
    m_layout_enabled = enabled;
    if (enabled && m_relayout_pending)
        relayout();
}

bool PageArea::is_right_to_left() const
{
    return layoutDirection() == Qt::RightToLeft;
}

QRect PageArea::contents_rect_for_pages() const
{
    return rect();
}

bool PageArea::shows_fallback() const
{
    return m_shown_pair.has_value() && m_shown_pair->contains(m_current) && !m_fullscreen_tab && !m_shows_side_by_side;
}

Vector<Tab*> PageArea::displayed_tabs() const
{
    Vector<Tab*> tabs;
    if (!m_current)
        return tabs;
    if (m_fullscreen_tab && m_fullscreen_tab == m_current) {
        tabs.append(m_current);
        return tabs;
    }
    if (m_shows_side_by_side && m_shown_pair.has_value() && m_shown_pair->contains(m_current)) {
        tabs.append(m_shown_pair->start);
        tabs.append(m_shown_pair->end);
        return tabs;
    }
    tabs.append(m_current);
    return tabs;
}

bool PageArea::is_displayed(Tab const* tab) const
{
    return tab && displayed_tabs().contains_slow(const_cast<Tab*>(tab));
}

QRect PageArea::slot_rect_for(Tab const& tab, SplitPair const* pair_of_tab) const
{
    auto area = contents_rect_for_pages();
    if (!pair_of_tab || !pair_of_tab->contains(&tab) || !SplitView::shows_side_by_side(area.width(), true))
        return { QPoint(), area.size() };
    auto widths = SplitView::side_widths(area.width(), pair_of_tab->start_fraction);
    auto width = pair_of_tab->start == &tab ? widths.start : widths.end;
    return { 0, 0, width, area.height() };
}

void PageArea::relayout()
{
    if (!m_layout_enabled) {
        m_relayout_pending = true;
        return;
    }
    m_relayout_pending = false;

    auto area = contents_rect_for_pages();
    auto pair_is_shown = m_shown_pair.has_value() && m_shown_pair->contains(m_current) && m_shown_pair->start && m_shown_pair->end
        && !(m_fullscreen_tab && m_fullscreen_tab == m_current);
    m_shows_side_by_side = pair_is_shown && SplitView::shows_side_by_side(area.width(), m_shows_side_by_side);

    auto displayed = displayed_tabs();

    // The pages on display take their places first, so the focus can move to one of them before any other page hides.
    if (m_shows_side_by_side) {
        auto widths = SplitView::side_widths(area.width(), m_shown_pair->start_fraction);
        auto rtl = is_right_to_left();
        auto start_left = rtl ? area.right() + 1 - widths.start : area.left();
        auto end_left = rtl ? area.left() : area.left() + widths.start + SplitView::DIVIDER_WIDTH;
        auto divider_left = rtl ? area.left() + widths.end : area.left() + widths.start;

        for (auto* tab : { m_shown_pair->start.data(), m_shown_pair->end.data() })
            tab->set_split_presentation(tab == m_current ? Tab::SplitPresentation::FocusedSide : Tab::SplitPresentation::OtherSide);
        m_shown_pair->start->setGeometry(start_left, area.top(), widths.start, area.height());
        m_shown_pair->end->setGeometry(end_left, area.top(), widths.end, area.height());
        m_divider->setGeometry(divider_left, area.top(), SplitView::DIVIDER_WIDTH, area.height());
        m_divider->set_split_geometry(area.left(), area.width(), widths.start, m_shown_pair->start_fraction, rtl);
        m_divider->show();
        m_divider->raise();
        // NB: The divider is a keyboard stop between the start side's page and the end side.
        if (m_shown_pair->start->is_loaded() && m_shown_pair->end->is_loaded()) {
            QWidget::setTabOrder(&m_shown_pair->start->view(), m_divider);
            QWidget::setTabOrder(m_divider, &m_shown_pair->end->view());
        }
    } else {
        m_divider->hide();
        if (m_current) {
            m_current->set_split_presentation(pair_is_shown ? Tab::SplitPresentation::Fallback : Tab::SplitPresentation::None);
            m_current->setGeometry(area);
        }
    }

    for (auto* tab : displayed) {
        if (!tab->isVisibleTo(this))
            tab->show();
    }

    // As in a stacked widget, the focus moves onto a page on display if it was on one that hides.
    auto* focus_widget = window() ? window()->focusWidget() : nullptr;
    for (auto& page : m_pages) {
        if (!page || displayed.contains_slow(page.data()))
            continue;
        if (focus_widget && page->isAncestorOf(focus_widget) && m_current) {
            if (auto* next_focus = m_current->focusWidget(); next_focus && m_current->isAncestorOf(next_focus))
                next_focus->setFocus();
            else
                m_current->setFocus();
        }
        // NB: A hidden page keeps how it was presented. A side of a pair keeps the room for its pane bar, so it is sized
        //     as it will show, and the tab widget sets the presentation of the pages that join or leave a pair.
        if (!page->isHidden())
            page->hide();
    }

    Vector<QPointer<Tab>> displayed_pointers;
    for (auto* tab : displayed)
        displayed_pointers.append(tab);
    if (displayed_pointers != m_last_displayed) {
        m_last_displayed = AK::move(displayed_pointers);
        emit displayed_tabs_changed();
    }
}

void PageArea::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    relayout();
}

bool PageArea::event(QEvent* event)
{
    if (event->type() == QEvent::LayoutDirectionChange)
        relayout();
    return QWidget::event(event);
}

// The divider is a separator that assistive technology can read the position of and move.
class SideDividerAccessibility final
    : public QAccessibleWidget
    , public QAccessibleValueInterface {
public:
    explicit SideDividerAccessibility(SideDivider* divider)
        : QAccessibleWidget(divider, QAccessible::Separator, "Split view divider")
    {
    }

    virtual void* interface_cast(QAccessible::InterfaceType type) override
    {
        if (type == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }

    virtual QAccessible::State state() const override
    {
        auto state = QAccessibleWidget::state();
        state.focusable = true;
        return state;
    }

    virtual QVariant currentValue() const override { return qRound(divider().start_fraction() * 100); }
    virtual void setCurrentValue(QVariant const& value) override
    {
        auto percent = clamp(value.toDouble(), 0.0, 100.0);
        emit static_cast<SideDivider*>(object())->start_fraction_committed(static_cast<float>(percent / 100.0));
    }
    virtual QVariant maximumValue() const override { return 100; }
    virtual QVariant minimumValue() const override { return 0; }
    virtual QVariant minimumStepSize() const override { return 1; }

private:
    SideDivider const& divider() const { return *static_cast<SideDivider const*>(object()); }
};

static QAccessibleInterface* side_divider_accessibility_factory(QString const&, QObject* object)
{
    if (auto* divider = qobject_cast<SideDivider*>(object))
        return new SideDividerAccessibility(divider);
    return nullptr;
}

static constexpr int GRIP_WIDTH = 4;
static constexpr int GRIP_HEIGHT = 32;
static constexpr int DRAG_START_DISTANCE = 4;
static constexpr int DRAG_LINE_WIDTH = 2;

// The line that shows where the sides will meet while the divider is dragged. Native page surfaces sit above the
// widgets beside them, so the line is a window of its own that floats over the pages.
class DividerDragLine final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit DividerDragLine(QWidget* parent)
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowTransparentForInput)
    {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Theme::accent());
    }
};

class DividerEscapeFilter final : public QObject {
public:
    explicit DividerEscapeFilter(Function<void()> on_escape, QObject* parent)
        : QObject(parent)
        , m_on_escape(AK::move(on_escape))
    {
    }

protected:
    virtual bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            m_on_escape();
            return true;
        }
        return false;
    }

private:
    Function<void()> m_on_escape;
};

SideDivider::SideDivider(PageArea& page_area)
    : QWidget(&page_area)
    , m_page_area(page_area)
{
    static bool accessibility_factory_installed = false;
    if (!accessibility_factory_installed) {
        QAccessible::installFactory(side_divider_accessibility_factory);
        accessibility_factory_installed = true;
    }

    setObjectName("LadybirdSplitViewDivider");
    setAccessibleName("Split view divider");
    setCursor(Qt::SplitHCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
#if defined(AK_OS_MACOS)
    // NB: The pages beside the divider have native surfaces, which would otherwise take the pointer over the gutter.
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
#endif

    m_grip_animation = new QVariantAnimation(this);
    m_grip_animation->setDuration(Motion::FAST);
    m_grip_animation->setEasingCurve(Motion::enter_easing());
    connect(m_grip_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_grip_opacity = value.toReal();
        update();
    });
}

QSize SideDivider::sizeHint() const
{
    return { SplitView::DIVIDER_WIDTH, GRIP_HEIGHT };
}

void SideDivider::set_split_geometry(int pages_left, int available_width, int start_width, float start_fraction, bool right_to_left)
{
    m_pages_left = pages_left;
    m_available_width = available_width;
    m_start_width = start_width;
    m_start_fraction = start_fraction;
    m_right_to_left = right_to_left;
    update();
}

void SideDivider::set_grip_opacity(qreal opacity)
{
    if (SystemAppearance::prefers_reduced_motion()) {
        m_grip_animation->stop();
        m_grip_opacity = opacity;
        update();
        return;
    }
    m_grip_animation->stop();
    m_grip_animation->setStartValue(m_grip_opacity);
    m_grip_animation->setEndValue(opacity);
    m_grip_animation->start();
}

void SideDivider::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    auto const& chrome = Theme::chrome(*this);
    painter.fillRect(rect(), chrome.strip);
    // A line of the divider color where the gutter meets each page.
    painter.fillRect(QRect(0, 0, 1, height()), chrome.divider);
    painter.fillRect(QRect(width() - 1, 0, 1, height()), chrome.divider);

    auto grip_opacity = m_is_dragging || m_has_keyboard_focus ? 1.0 : m_grip_opacity;
    QRectF grip { (width() - GRIP_WIDTH) / 2.0, (height() - GRIP_HEIGHT) / 2.0, static_cast<qreal>(GRIP_WIDTH), static_cast<qreal>(GRIP_HEIGHT) };
    if (grip_opacity > 0) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setOpacity(grip_opacity);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_is_dragging ? Theme::accent() : chrome.muted);
        painter.drawRoundedRect(grip, GRIP_WIDTH / 2.0, GRIP_WIDTH / 2.0);
        painter.restore();
    }
    if (m_has_keyboard_focus)
        ChromeStyle::paint_focus_ring(painter, grip.adjusted(-1, 0, 1, 0), GRIP_WIDTH / 2.0 + 1);
}

void SideDivider::enterEvent(QEnterEvent* event)
{
    QWidget::enterEvent(event);
    m_is_hovered = true;
    set_grip_opacity(1.0);
}

void SideDivider::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    m_is_hovered = false;
    if (!m_is_dragging)
        set_grip_opacity(0.0);
}

int SideDivider::start_width_for_global_x(int global_x) const
{
    auto delta = global_x - m_press_global_x;
    if (m_right_to_left)
        delta = -delta;
    auto room = max(0, m_available_width - SplitView::DIVIDER_WIDTH);
    return clamp(m_drag_start_width + delta, min(SplitView::MINIMUM_SIDE_WIDTH, room / 2), max(room - SplitView::MINIMUM_SIDE_WIDTH, room / 2));
}

void SideDivider::update_drag_line(int start_width)
{
    if (!m_drag_line)
        m_drag_line = new DividerDragLine(this);
    auto room = max(0, m_available_width - SplitView::DIVIDER_WIDTH);
    // The line snaps to the middle as the released divider will.
    if (abs(start_width - (room / 2)) <= SplitView::SNAP_DISTANCE)
        start_width = room / 2;
    auto gutter_left = m_right_to_left ? m_pages_left + m_available_width - start_width - SplitView::DIVIDER_WIDTH : m_pages_left + start_width;
    auto line_x = gutter_left + ((SplitView::DIVIDER_WIDTH - DRAG_LINE_WIDTH) / 2);
    auto top_left = m_page_area.mapToGlobal(QPoint(line_x, 0));
    m_drag_line->setGeometry(top_left.x(), top_left.y(), DRAG_LINE_WIDTH, m_page_area.height());
    if (!m_drag_line->isVisible())
        m_drag_line->show();
    m_drag_line->raise();
}

void SideDivider::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_is_pressed = true;
    m_press_global_x = event->globalPosition().toPoint().x();
    m_drag_start_width = m_start_width;
    event->accept();
}

void SideDivider::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_is_pressed) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    auto global_x = event->globalPosition().toPoint().x();
    if (!m_is_dragging) {
        if (abs(global_x - m_press_global_x) < DRAG_START_DISTANCE)
            return;
        m_is_dragging = true;
        m_escape_filter = new DividerEscapeFilter([this] { cancel_drag(); }, this);
        qApp->installEventFilter(m_escape_filter);
        update();
    }
    update_drag_line(start_width_for_global_x(global_x));
    event->accept();
}

void SideDivider::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_is_pressed) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    auto was_dragging = m_is_dragging;
    auto start_width = start_width_for_global_x(event->globalPosition().toPoint().x());
    cancel_drag();
    if (was_dragging)
        commit(SplitView::start_fraction_for_divider_position(start_width, m_available_width));
    event->accept();
}

void SideDivider::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        cancel_drag();
        commit(0.5f);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void SideDivider::cancel_drag()
{
    m_is_pressed = false;
    m_is_dragging = false;
    if (m_escape_filter) {
        qApp->removeEventFilter(m_escape_filter);
        m_escape_filter->deleteLater();
        m_escape_filter = nullptr;
    }
    if (m_drag_line)
        m_drag_line->hide();
    if (!m_is_hovered)
        set_grip_opacity(0.0);
    update();
}

void SideDivider::commit(float start_fraction)
{
    start_fraction = SplitView::sanitized_start_fraction(start_fraction);
    m_start_fraction = start_fraction;
    emit start_fraction_committed(start_fraction);
    QAccessibleValueChangeEvent event(this, qRound(start_fraction * 100));
    QAccessible::updateAccessibility(&event);
}

void SideDivider::keyPressEvent(QKeyEvent* event)
{
    auto room = max(1, m_available_width - SplitView::DIVIDER_WIDTH);
    auto minimum = min(0.5f, static_cast<float>(SplitView::MINIMUM_SIDE_WIDTH) / room);
    auto maximum = 1.0f - minimum;
    auto step = event->modifiers().testFlag(Qt::ShiftModifier) ? SplitView::FINE_KEYBOARD_STEP : SplitView::KEYBOARD_STEP;
    // NB: The arrow toward the end always moves the divider toward the end, which is on the left in right-to-left.
    auto toward_end = m_right_to_left ? Qt::Key_Left : Qt::Key_Right;
    auto toward_start = m_right_to_left ? Qt::Key_Right : Qt::Key_Left;

    auto current = clamp(m_start_fraction, minimum, maximum);
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right:
        commit(clamp(current + (event->key() == toward_end ? step : (event->key() == toward_start ? -step : 0.0f)), minimum, maximum));
        event->accept();
        return;
    case Qt::Key_Home:
        commit(minimum);
        event->accept();
        return;
    case Qt::Key_End:
        commit(maximum);
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        commit(0.5f);
        event->accept();
        return;
    case Qt::Key_Escape:
        if (m_is_dragging) {
            cancel_drag();
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void SideDivider::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    // The focus ring is only for keyboard focus.
    m_has_keyboard_focus = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason || event->reason() == Qt::ShortcutFocusReason;
    update();
}

void SideDivider::focusOutEvent(QFocusEvent* event)
{
    QWidget::focusOutEvent(event);
    m_has_keyboard_focus = false;
    update();
}

bool SideDivider::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        update();
    return QWidget::event(event);
}

}
