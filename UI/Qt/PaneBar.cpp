/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/URL.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/PaneBar.h>
#include <UI/Qt/SplitView.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/Theme.h>

#include <QAction>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVariantAnimation>
#include <QtMath>

namespace Ladybird {

static constexpr int PANE_BAR_FAVICON_INSET = 8;
static constexpr int TAB_ICON_SIZE_FOR_PANE_BAR = 16;
static constexpr int PANE_BAR_ITEM_GAP = 8;
static constexpr int PANE_BAR_BUTTON_HIT_SIZE = 32;
static constexpr int PANE_BAR_BUTTON_SIZE = 28;
static constexpr int PANE_BAR_BUTTON_RADIUS = 6;
static constexpr int PANE_BAR_TEXT_PIXEL_SIZE = 12;
static constexpr int PANE_BAR_CHIP_HEIGHT = 22;
static constexpr int PANE_BAR_CHIP_RADIUS = 5;
static constexpr int PANE_BAR_CHIP_PADDING = 8;
static constexpr int PANE_BAR_ACCENT_HEIGHT = 2;

static QFont pane_bar_font(QFont font, QFont::Weight weight = QFont::Normal)
{
    font.setPixelSize(PANE_BAR_TEXT_PIXEL_SIZE);
    font.setWeight(weight);
    return font;
}

// The buttons on a pane bar: the side menu, the button that shows the other side, and the chip of what the side waits
// for. Each one's hit area is the whole height of the bar.
class PaneBarButton final : public QToolButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    enum class Glyph : u8 {
        More,
        ShowOtherSide,
        Chip,
    };

    PaneBarButton(Glyph glyph, QWidget* parent)
        : QToolButton(parent)
        , m_glyph(glyph)
    {
        setAutoRaise(true);
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::ArrowCursor);
        setAttribute(Qt::WA_Hover);
    }

    void set_chip_text(QString text)
    {
        m_chip_text = AK::move(text);
        updateGeometry();
        update();
    }

    virtual QSize sizeHint() const override
    {
        if (m_glyph == Glyph::Chip) {
            // NB: The chip is inset 2 px at each end of its button, and its dot and the gap after it come before the words.
            auto width = qCeil(QFontMetricsF(pane_bar_font(font(), QFont::Medium)).horizontalAdvance(m_chip_text)) + (2 * PANE_BAR_CHIP_PADDING) + 6 + 4 + 4 + 1;
            return { width, PANE_BAR_BUTTON_HIT_SIZE };
        }
        return { PANE_BAR_BUTTON_HIT_SIZE, PANE_BAR_BUTTON_HIT_SIZE };
    }

protected:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto const& chrome = Theme::chrome(*this);
        auto hovered = underMouse() && isEnabled();
        auto pressed = isDown() || (menu() && menu()->isVisible());

        if (m_glyph == Glyph::Chip) {
            QRectF chip { 2, (height() - PANE_BAR_CHIP_HEIGHT) / 2.0, width() - 4.0, static_cast<qreal>(PANE_BAR_CHIP_HEIGHT) };
            auto fill = chrome.hover;
            if (hovered || pressed)
                fill = ChromeStyle::mix(chrome.hover, chrome.text, pressed ? 0.16 : 0.08);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(chip, PANE_BAR_CHIP_RADIUS, PANE_BAR_CHIP_RADIUS);
            // A dot of the accent before the words says the side wants the person.
            painter.setBrush(Theme::accent());
            painter.drawEllipse(QPointF(chip.left() + PANE_BAR_CHIP_PADDING + 3, chip.center().y()), 3, 3);
            painter.setPen(chrome.text);
            painter.setFont(pane_bar_font(font(), QFont::Medium));
            painter.drawText(chip.adjusted(PANE_BAR_CHIP_PADDING + 6 + 4, 0, -PANE_BAR_CHIP_PADDING, 0), Qt::AlignVCenter | Qt::AlignLeft, m_chip_text);
            if (hasFocus() && focusPolicy() != Qt::NoFocus)
                ChromeStyle::paint_focus_ring(painter, chip, PANE_BAR_CHIP_RADIUS);
            return;
        }

        QRectF frame { (width() - PANE_BAR_BUTTON_SIZE) / 2.0, (height() - PANE_BAR_BUTTON_SIZE) / 2.0, static_cast<qreal>(PANE_BAR_BUTTON_SIZE), static_cast<qreal>(PANE_BAR_BUTTON_SIZE) };
        if (hovered || pressed) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(pressed ? ChromeStyle::mix(chrome.hover, chrome.text, 0.08) : chrome.hover);
            painter.drawRoundedRect(frame, PANE_BAR_BUTTON_RADIUS, PANE_BAR_BUTTON_RADIUS);
        }

        auto ink = isEnabled() ? chrome.icon : chrome.disabled;
        auto center = frame.center();
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        switch (m_glyph) {
        case Glyph::More:
            for (int i = -1; i <= 1; ++i)
                painter.drawEllipse(QPointF(center.x() + (i * 5), center.y()), 1.5, 1.5);
            break;
        case Glyph::ShowOtherSide: {
            // Two panes side by side, the other one filled.
            QRectF outline { center.x() - 7.5, center.y() - 5.5, 15, 11 };
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(ink, 1.25));
            painter.drawRoundedRect(outline, 2, 2);
            painter.drawLine(QPointF(center.x(), outline.top()), QPointF(center.x(), outline.bottom()));
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            painter.drawRoundedRect(QRectF(center.x() + 1.5, outline.top() + 1.5, 4.5, outline.height() - 3), 1, 1);
            break;
        }
        case Glyph::Chip:
            break;
        }
        if (hasFocus() && focusPolicy() != Qt::NoFocus)
            ChromeStyle::paint_focus_ring(painter, frame, PANE_BAR_BUTTON_RADIUS);
    }

    virtual bool event(QEvent* event) override
    {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave)
            update();
        return QToolButton::event(event);
    }

private:
    Glyph m_glyph { Glyph::More };
    QString m_chip_text;
};

PaneBar::PaneBar(Tab& tab)
    : QWidget(&tab)
    , m_tab(tab)
{
    setObjectName("LadybirdPaneBar");
    setFixedHeight(SplitView::PANE_BAR_HEIGHT);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    // NB: A click on the bar focuses its side without moving the keyboard into the page.
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_Hover);
    setAccessibleName("Side");

    m_menu = new QMenu("Side", this);
    auto* swap_action = m_menu->addAction("Swap Sides");
    auto* separate_action = m_menu->addAction("Separate Tabs");
    auto* move_action = m_menu->addAction("Move Split View to New Window");
    m_menu->addSeparator();
    auto* close_action = m_menu->addAction("Close Tab");
    connect(swap_action, &QAction::triggered, this, &PaneBar::swap_sides_requested);
    connect(separate_action, &QAction::triggered, this, &PaneBar::separate_tabs_requested);
    connect(move_action, &QAction::triggered, this, &PaneBar::move_to_new_window_requested);
    connect(close_action, &QAction::triggered, this, &PaneBar::close_tab_requested);

    m_menu_button = new PaneBarButton(PaneBarButton::Glyph::More, this);
    m_menu_button->setToolTip("Split view");
    m_menu_button->setAccessibleName("Split view");
    m_menu_button->setPopupMode(QToolButton::InstantPopup);
    m_menu_button->setMenu(m_menu);
    // NB: The menu button draws no arrow, as its glyph already says it opens a menu.
    m_menu_button->setStyleSheet("QToolButton::menu-indicator { image: none; width: 0; }");

    m_show_other_side_button = new PaneBarButton(PaneBarButton::Glyph::ShowOtherSide, this);
    m_show_other_side_button->setAccessibleName("Show other side");
    m_show_other_side_button->hide();
    connect(m_show_other_side_button, &QToolButton::clicked, this, &PaneBar::show_other_side_requested);

    auto* chip = new PaneBarButton(PaneBarButton::Glyph::Chip, this);
    m_pending_chip = chip;
    m_pending_chip->hide();
    connect(m_pending_chip, &QToolButton::clicked, this, &PaneBar::pending_chip_clicked);

    m_loading_timer = new QTimer(this);
    m_loading_timer->setInterval(16);
    connect(m_loading_timer, &QTimer::timeout, this, [this] { update(); });

    // The bar fades in as the pair shows.
    m_fade_in = new QVariantAnimation(this);
    m_fade_in->setDuration(Motion::FAST);
    m_fade_in->setEasingCurve(Motion::enter_easing());
    m_fade_in->setStartValue(0.0);
    m_fade_in->setEndValue(1.0);
    connect(m_fade_in, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_opacity = value.toReal();
        update();
    });

    connect(&tab, &Tab::favicon_changed, this, [this] { page_changed(); });
    connect(&tab, &Tab::title_changed, this, [this] { page_changed(); });
    connect(&tab, &Tab::loading_changed, this, [this] { page_changed(); });
    connect(&tab, &Tab::url_changed, this, [this] { page_changed(); });

    hide();
}

QSize PaneBar::sizeHint() const
{
    return { 0, SplitView::PANE_BAR_HEIGHT };
}

QSize PaneBar::minimumSizeHint() const
{
    return { 0, SplitView::PANE_BAR_HEIGHT };
}

void PaneBar::set_state(State state)
{
    if (m_state == state)
        return;
    if (m_state == State::OtherSide && state != State::OtherSide) {
        m_just_became_focused = true;
        QTimer::singleShot(0, this, [this] { m_just_became_focused = false; });
    }
    m_state = state;
    m_show_other_side_button->setVisible(state == State::Fallback);
    setAccessibleName(state == State::OtherSide ? "Other side" : "Focused side");
    layout_buttons();
    update();
}

void PaneBar::set_pending(Optional<Pending> pending)
{
    if (m_pending == pending)
        return;
    m_pending = pending;
    if (pending.has_value())
        static_cast<PaneBarButton*>(m_pending_chip)->set_chip_text(pending_text());
    layout_buttons();
    update();
}

QString PaneBar::pending_text() const
{
    if (!m_pending.has_value())
        return {};
    switch (*m_pending) {
    case Pending::PermissionRequest:
        return "Permission request";
    case Pending::Dialog:
        return "Dialog";
    case Pending::Crashed:
        return "Crashed";
    }
    VERIFY_NOT_REACHED();
}

void PaneBar::set_toolbar_in_sidebar(bool in_sidebar)
{
    if (m_toolbar_in_sidebar == in_sidebar)
        return;
    m_toolbar_in_sidebar = in_sidebar;
    update();
}

void PaneBar::set_partner_title(QString const& title)
{
    auto tooltip = title.isEmpty() ? QString("Show other side") : QString("Show %1").arg(title);
    m_show_other_side_button->setToolTip(tooltip);
}

void PaneBar::page_changed()
{
    update_loading_timer();
    setAccessibleDescription(m_tab.title());
    update();
}

void PaneBar::update_loading_timer()
{
    auto animate = isVisible() && m_tab.is_loading();
    if (animate && !m_loading_timer->isActive())
        m_loading_timer->start();
    else if (!animate && m_loading_timer->isActive())
        m_loading_timer->stop();
}

void PaneBar::layout_buttons()
{
    auto right = width();
    auto place = [&](QWidget& button, bool shown) {
        button.setVisible(shown);
        if (!shown)
            return;
        auto size = button.sizeHint();
        right -= size.width();
        button.setGeometry(right, (height() - size.height()) / 2, size.width(), size.height());
    };
    place(*m_menu_button, true);
    place(*m_show_other_side_button, m_state == State::Fallback);
    // The chip only shows on the side that isn't focused. The focused side shows what it waits for itself.
    place(*m_pending_chip, m_pending.has_value() && m_state == State::OtherSide);
    if (layoutDirection() == Qt::RightToLeft) {
        for (auto* button : { static_cast<QWidget*>(m_menu_button), static_cast<QWidget*>(m_show_other_side_button), static_cast<QWidget*>(m_pending_chip) }) {
            if (button->isVisible())
                button->move(width() - button->x() - button->width(), button->y());
        }
    }
}

void PaneBar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layout_buttons();
}

QRect PaneBar::origin_rect() const
{
    auto left = PANE_BAR_FAVICON_INSET + TAB_ICON_SIZE_FOR_PANE_BAR + PANE_BAR_ITEM_GAP;
    auto right = width();
    for (auto* button : { static_cast<QWidget const*>(m_menu_button), static_cast<QWidget const*>(m_show_other_side_button), static_cast<QWidget const*>(m_pending_chip) }) {
        if (button->isVisible())
            right = min(right, layoutDirection() == Qt::RightToLeft ? width() - button->x() - button->width() : button->x());
    }
    if (m_state == State::Fallback)
        right -= QFontMetrics(pane_bar_font(font())).horizontalAdvance("1 of 2") + PANE_BAR_ITEM_GAP;
    return { left, 0, max(0, right - left - PANE_BAR_ITEM_GAP), height() };
}

void PaneBar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(m_opacity);
    auto palette = Theme::chrome_palette(*this);
    auto const& chrome = Theme::chrome(palette);
    auto is_focused_side = m_state != State::OtherSide;
    auto rtl = layoutDirection() == Qt::RightToLeft;
    auto mirrored = [&](QRect rect) {
        if (rtl)
            rect.moveLeft(width() - rect.left() - rect.width());
        return rect;
    };

    // The focused side joins the toolbar as the active tab does. The other side sits back in the strip's color, with a
    // line above it, so the two read apart even without their colors.
    if (m_toolbar_in_sidebar) {
        painter.fillRect(rect(), chrome.sidebar);
        if (is_focused_side && m_state != State::Fallback)
            painter.fillRect(QRect(0, height() - PANE_BAR_ACCENT_HEIGHT, width(), PANE_BAR_ACCENT_HEIGHT), Theme::accent());
    } else {
        painter.fillRect(rect(), is_focused_side ? chrome.toolbar : chrome.strip);
        if (!is_focused_side)
            painter.fillRect(QRect(0, 0, width(), 1), chrome.divider);
    }
    // NB: The bar meets the page at the chrome's edge.
    if (!(m_toolbar_in_sidebar && is_focused_side && m_state != State::Fallback))
        painter.fillRect(QRect(0, height() - 1, width(), 1), chrome.divider);

    auto is_inactive = ChromeStyle::window_activity(*this) == ChromeStyle::WindowActivity::Inactive;
    auto text_color = is_inactive || !is_focused_side ? chrome.muted : chrome.text;
    if (!is_inactive && !is_focused_side)
        text_color = ChromeStyle::mix(chrome.muted, chrome.text, 0.35);

    // The favicon, or the loading ring around a shrunken one.
    QRect icon_rect = mirrored({ PANE_BAR_FAVICON_INSET, (height() - TAB_ICON_SIZE_FOR_PANE_BAR) / 2, TAB_ICON_SIZE_FOR_PANE_BAR, TAB_ICON_SIZE_FOR_PANE_BAR });
    auto icon = m_tab.tab_icon();
    if (m_tab.is_loading()) {
        ChromeStyle::paint_loading_ring(painter, palette, QRectF(icon_rect).center(), ChromeStyle::LoadingRingSize::Favicon,
            m_tab.is_waiting_for_response() ? ChromeStyle::LoadingPhase::WaitingForResponse : ChromeStyle::LoadingPhase::Receiving);
        icon_rect = icon_rect.adjusted(3, 3, -3, -3);
    }
    if (icon.isNull())
        icon = create_chrome_icon(ChromeIcon::Globe, chrome.icon);
    icon.paint(&painter, icon_rect);

    auto origin = origin_rect();
    auto x = origin.left();
    auto url = m_tab.url();

    // A site reached over plain HTTP is marked as it is in the address field.
    if (url.scheme() == "http"sv) {
        auto pill_font = pane_bar_font(font(), QFont::Medium);
        auto pill_text = QStringLiteral("Not secure");
        auto pill_width = QFontMetrics(pill_font).horizontalAdvance(pill_text) + 14 + 5 + 14;
        if (pill_width < origin.width()) {
            QRect pill = mirrored({ x, (height() - PANE_BAR_CHIP_HEIGHT) / 2, pill_width, PANE_BAR_CHIP_HEIGHT });
            painter.setPen(Qt::NoPen);
            painter.setBrush(chrome.warning_fill);
            painter.drawRoundedRect(QRectF(pill), PANE_BAR_CHIP_RADIUS, PANE_BAR_CHIP_RADIUS);
            auto warning = create_chrome_icon(ChromeIcon::Warning, chrome.warning_ink);
            warning.paint(&painter, QRect(pill.left() + 7, pill.top() + (pill.height() - 14) / 2, 14, 14));
            painter.setPen(chrome.warning_ink);
            painter.setFont(pill_font);
            painter.drawText(pill.adjusted(7 + 14 + 5, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, pill_text);
            x += pill_width + PANE_BAR_ITEM_GAP;
        }
    }

    // The site's origin: its registrable domain in full strength, and the rest of its host in the muted color.
    auto serialized = url.serialize();
    QString secondary;
    QString primary;
    if (url.scheme().is_one_of("http"sv, "https"sv)) {
        if (auto parts = WebView::break_url_into_parts(serialized); parts.has_value()) {
            auto subdomain = parts->scheme_and_subdomain;
            auto scheme_prefix_length = url.scheme().length() + "://"sv.length();
            if (subdomain.length() >= scheme_prefix_length)
                subdomain = subdomain.substring_view(scheme_prefix_length);
            if (subdomain.starts_with("www."sv, CaseSensitivity::CaseInsensitive))
                subdomain = subdomain.substring_view(4);
            secondary = qstring_from_ak_string(subdomain);
            primary = qstring_from_ak_string(parts->effective_tld_plus_one);
        } else {
            primary = qstring_from_ak_string(url.serialized_host());
        }
    } else if (url.scheme() == "file"sv) {
        primary = m_tab.title();
        secondary = QStringLiteral("File · ");
    } else {
        primary = m_tab.title().isEmpty() ? qstring_from_ak_string(serialized) : m_tab.title();
    }

    auto text_font = pane_bar_font(font());
    painter.setFont(text_font);
    QFontMetrics metrics(text_font);
    auto available = origin.right() - x;
    auto secondary_width = metrics.horizontalAdvance(secondary);
    auto secondary_ink = text_color;
    secondary_ink.setAlpha(ChromeStyle::is_dark(palette) ? 178 : 148);
    if (!secondary.isEmpty() && secondary_width < available) {
        painter.setPen(secondary_ink);
        painter.drawText(mirrored({ x, 0, secondary_width, height() }), Qt::AlignVCenter | Qt::AlignLeft, secondary);
        x += secondary_width;
        available -= secondary_width;
    }
    if (available > 0) {
        painter.setPen(text_color);
        auto elided = metrics.elidedText(primary, Qt::ElideRight, available);
        painter.drawText(mirrored({ x, 0, available, height() }), Qt::AlignVCenter | Qt::AlignLeft, elided);
    }

    if (m_state == State::Fallback) {
        // "1 of 2" says another side waits behind this one, beside the button that shows it.
        auto label = QStringLiteral("1 of 2");
        auto label_width = metrics.horizontalAdvance(label);
        auto label_right = m_show_other_side_button->isVisible() ? (rtl ? width() - m_show_other_side_button->x() - m_show_other_side_button->width() : m_show_other_side_button->x()) : width();
        painter.setPen(chrome.muted);
        painter.drawText(mirrored({ label_right - label_width - 2, 0, label_width, height() }), Qt::AlignVCenter | Qt::AlignRight, label);
    }
}

void PaneBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        // A second click on the origin of the focused side goes to the address field, as the origin is the address.
        m_press_was_on_focused_origin = m_state != State::OtherSide && !m_just_became_focused && origin_rect().contains(layoutDirection() == Qt::RightToLeft ? QPoint(width() - event->pos().x(), event->pos().y()) : event->pos());
        emit focus_side_requested();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PaneBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_press_was_on_focused_origin && rect().contains(event->pos())) {
        m_press_was_on_focused_origin = false;
        emit focus_address_requested();
        event->accept();
        return;
    }
    m_press_was_on_focused_origin = false;
    QWidget::mouseReleaseEvent(event);
}

bool PaneBar::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::Show:
        update_loading_timer();
        if (SystemAppearance::prefers_reduced_motion()) {
            m_opacity = 1;
        } else {
            m_opacity = 0;
            m_fade_in->start();
        }
        break;
    case QEvent::Hide:
        update_loading_timer();
        m_fade_in->stop();
        m_opacity = 1;
        break;
    case QEvent::PaletteChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::WindowActivate:
    case QEvent::WindowDeactivate:
        layout_buttons();
        update();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

}
