/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <UI/Qt/ExtensionButtons.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Theme.h>

namespace Ladybird {

// The view sits inside the card's rounded edge.
static constexpr int popup_content_inset = 2;
static constexpr int popup_scrollbar_allowance = 16;

ExtensionPopup::ExtensionPopup(QWidget* parent, QSize available_size)
    : Popover(parent)
    , m_available_size(available_size)
{
    // Until the document reports the size it needs, the card has room for a typical popup.
    set_content_size({ 380, 420 });
}

QSize ExtensionPopup::clamped_content_size(QSize size, QSize available_size)
{
    // The popover adds transparent space around its card for the shadow, so leave room for it on the screen.
    auto screen_limit = available_size - QSize(32, 32) - QSize(popup_content_inset * 2, popup_content_inset * 2);
    auto limit = maximum_content_size.boundedTo(screen_limit.expandedTo(minimum_content_size));
    // A document taller than the popup scrolls, and its scrollbar takes width from the content. As Firefox does, widen
    // the popup by the scrollbar rather than make the content scroll sideways too.
    if (size.height() > limit.height())
        size.rwidth() += popup_scrollbar_allowance;
    return size.expandedTo(minimum_content_size).boundedTo(limit);
}

void ExtensionPopup::set_content_size(QSize size)
{
    auto content = clamped_content_size(size, m_available_size);
    card().setFixedSize(content + QSize(popup_content_inset * 2, popup_content_inset * 2));
    adjustSize();
}

QIcon ExtensionButtons::package_icon(Optional<Gfx::ShareableBitmap> const& icon)
{
    if (!icon.has_value() || !icon->is_valid())
        return {};
    auto const& bitmap = *icon->bitmap();
    if (bitmap.width() > 512 || bitmap.height() > 512)
        return {};
    auto format = bitmap.alpha_type() == Gfx::AlphaType::Premultiplied ? QImage::Format_ARGB32_Premultiplied : QImage::Format_ARGB32;
    QImage image(bitmap.width(), bitmap.height(), format);
    for (int y = 0; y < bitmap.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < bitmap.width(); ++x)
            row[x] = bitmap.get_pixel(x, y).value();
    }
    return QIcon(QPixmap::fromImage(image));
}

static void setup_button(QToolButton& button, QString const& title)
{
    button.setAccessibleName(title);
    button.setToolTip(title.toHtmlEscaped());
    button.setAutoRaise(true);
    button.setFocusPolicy(Qt::StrongFocus);
    button.setIconSize({ TOOLBAR_ICON_SIZE, TOOLBAR_ICON_SIZE });
    button.setFixedSize(32, 28);
    button.setAttribute(Qt::WA_LayoutUsesWidgetRect);
}

class ActionButton final : public QToolButton {
public:
    explicit ActionButton(QWidget* parent)
        : QToolButton(parent)
    {
    }

    QString badge;
    QColor badge_color;
    QIcon package_icon;
    QIcon light_icon;
    QIcon dark_icon;

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        QStyleOptionToolButton option;
        initStyleOption(&option);
        QStylePainter painter(this);
        ChromeStyle::paint_control_frame(painter, *this);
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
        if (hasFocus())
            ChromeStyle::paint_focus_ring(painter, QRectF(rect()).adjusted(1, 1, -1, -1), ChromeStyle::CONTROL_FRAME_RADIUS);
        if (badge.isEmpty())
            return;
        painter.setRenderHint(QPainter::Antialiasing);
        auto font = this->font();
        font.setPixelSize(9);
        font.setBold(true);
        painter.setFont(font);
        auto text = painter.fontMetrics().elidedText(badge, Qt::ElideRight, width() - 7);
        auto width = painter.fontMetrics().horizontalAdvance(text) + 4;
        QRect rect(this->width() - width, height() - 12, width, 12);
        auto color = badge_color.isValid() ? badge_color : palette().color(QPalette::Highlight);
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(rect, 3, 3);
        painter.setPen(qGray(color.rgb()) < 145 ? Qt::white : Qt::black);
        painter.drawText(rect, Qt::AlignCenter, text);
    }
};

ExtensionButtons::ExtensionButtons(QWidget* parent)
    : QWidget(parent)
{
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(2);
    m_menu_button = new ActionButton(this);
    m_menu_button->setObjectName("LadybirdExtensionsButton");
    setup_button(*m_menu_button, tr("Extensions"));
    m_layout->addWidget(m_menu_button);
    QObject::connect(m_menu_button, &QToolButton::clicked, this, [this] { open_menu(); });
    update_icons();
    hide();
}

void ExtensionButtons::set_items(Vector<Hub::ExtensionItem> items)
{
    m_items = AK::move(items);
    std::sort(m_items.begin(), m_items.end(), [](auto const& left, auto const& right) {
        return left.name == right.name ? left.id < right.id : left.name < right.name;
    });
    update_buttons();
}

void ExtensionButtons::set_pin_capacity(int capacity)
{
    if (m_pin_capacity == capacity)
        return;
    m_pin_capacity = capacity;
    update_buttons();
}

void ExtensionButtons::update_buttons()
{
    int index = 0;
    for (auto const& item : m_items) {
        if (!item.has_action || !item.pinned || index >= m_pin_capacity)
            continue;
        ActionButton* button;
        if (index == m_layout->count() - 1) {
            button = new ActionButton(this);
            m_layout->insertWidget(index, button);
            QObject::connect(button, &QToolButton::clicked, this, [this, button] {
                if (on_invoke)
                    on_invoke(ak_string_from_qstring(button->property("extensionId").toString()));
            });
        } else {
            button = static_cast<ActionButton*>(m_layout->itemAt(index)->widget());
        }
        ++index;
        auto title = qstring_from_ak_string(item.title.is_empty() ? item.name : item.title);
        setup_button(*button, title);
        button->badge = qstring_from_ak_string(item.badge);
        auto badge_color = Gfx::Color::from_string(item.badge_color);
        button->badge_color = badge_color.has_value() ? QColor::fromRgba(badge_color->value()) : QColor {};
        button->setAccessibleDescription(button->badge);
        button->setProperty("extensionId", qstring_from_ak_string(item.id));
        button->package_icon = package_icon(item.icon);
        button->light_icon = package_icon(item.light_icon);
        button->dark_icon = package_icon(item.dark_icon);
        button->setEnabled(item.enabled);
        button->update();
    }
    while (m_layout->count() > index + 1)
        delete m_layout->itemAt(index)->widget();
    update_icons();
}

static QIcon menu_icon(Hub::ExtensionItem const& item, QPalette const& palette)
{
    auto color = palette.color(QPalette::WindowText);
    auto icon = ExtensionButtons::package_icon(color.lightness() >= 128 ? item.light_icon : item.dark_icon);
    if (icon.isNull())
        icon = ExtensionButtons::package_icon(item.icon);
    return icon.isNull() ? create_chrome_icon(ChromeIcon::Extensions, color) : icon;
}

void ExtensionButtons::update_icons()
{
    auto icon = create_chrome_icon(ChromeIcon::Extensions, Theme::chrome_palette(*this), ChromeStyle::window_activity(*this), TOOLBAR_ICON_SIZE);
    m_menu_button->setIcon(icon);
    auto dark = Theme::is_dark(*this);
    if (m_menu) {
        for (auto* entry : m_menu->actions()) {
            if (!entry->menu())
                continue;
            auto id = ak_string_from_qstring(entry->data().toString());
            auto item = m_items.find_if([&](auto const& item) { return item.id == id; });
            if (item != m_items.end())
                entry->setIcon(menu_icon(*item, m_menu->palette()));
        }
    }
    for (int i = 0; i < m_layout->count() - 1; ++i) {
        auto* button = static_cast<ActionButton*>(m_layout->itemAt(i)->widget());
        auto const& themed_icon = dark ? button->light_icon : button->dark_icon;
        auto const& package_icon = themed_icon.isNull() ? button->package_icon : themed_icon;
        button->setIcon(package_icon.isNull() ? icon : package_icon);
    }
}

QWidget& ExtensionButtons::anchor_for(String const& id) const
{
    for (int i = 0; i < m_layout->count() - 1; ++i) {
        auto* button = m_layout->itemAt(i)->widget();
        if (button->property("extensionId").toString() == qstring_from_ak_string(id))
            return *button;
    }
    return *m_menu_button;
}

static QString menu_label(QString text)
{
    return text.replace('&', "&&");
}

void ExtensionButtons::close_menu()
{
    if (m_menu)
        m_menu->close();
}

void ExtensionButtons::open_menu()
{
    close_menu();
    auto* menu = new QMenu(this);
    m_menu = menu;
    menu->setAttribute(Qt::WA_DeleteOnClose);
    for (auto const& item : m_items) {
        auto name = menu_label(qstring_from_ak_string(item.name));
        auto* actions = menu->addMenu(menu_icon(item, menu->palette()), name);
        actions->menuAction()->setData(qstring_from_ak_string(item.id));
        actions->menuAction()->setIconVisibleInMenu(true);
        auto id = item.id;
        if (item.has_action) {
            auto title = menu_label(qstring_from_ak_string(item.title));
            auto badge = menu_label(qstring_from_ak_string(item.badge));
            auto* run = actions->addAction(title.isEmpty() || title == name ? tr("Run extension") : title);
            if (!badge.isEmpty())
                run->setText(run->text() + "  " + badge);
            run->setEnabled(item.enabled);
            QObject::connect(run, &QAction::triggered, this, [this, id] { if (on_invoke) on_invoke(id); });
            auto* pin = actions->addAction(tr("Pin to toolbar"));
            pin->setCheckable(true);
            pin->setChecked(item.pinned);
            QObject::connect(pin, &QAction::triggered, this, [this, id](bool pinned) { if (on_pin) on_pin(id, pinned); });
        }
        if (item.has_sidebar) {
            auto* sidebar = actions->addAction(item.sidebar_url.has_value() ? tr("Close sidebar") : tr("Open sidebar"));
            QObject::connect(sidebar, &QAction::triggered, this, [this, id] { if (on_sidebar) on_sidebar(id); });
        }
        if (item.has_options) {
            auto* options = actions->addAction(tr("Options"));
            QObject::connect(options, &QAction::triggered, this, [this, id] { if (on_options) on_options(id); });
        }
        if (actions->isEmpty())
            actions->addAction(tr("Manage extension"), this, [this] { if (on_manage) on_manage(); });
    }
    if (m_items.is_empty())
        menu->addAction(tr("No enabled extensions"))->setEnabled(false);
    menu->addSeparator();
    menu->addAction(tr("Manage extensions"), this, [this] { if (on_manage) on_manage(); });
    menu->popup(m_menu_button->mapToGlobal(m_menu_button->rect().bottomLeft()));
}

}
