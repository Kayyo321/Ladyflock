/*
 * Copyright (c) 2026, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/InspectorHost.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WebContentView.h>

#include <QApplication>
#include <QCloseEvent>
#include <QVBoxLayout>

namespace Ladybird {

static Optional<InspectorHost::DockMode> s_remembered_regular_dock_mode;
static double s_remembered_regular_split_ratio { 0.38 };

static constexpr int MINIMUM_SIDE_BY_SIDE_WIDTH = 920;

InspectorHost::DockMode InspectorHost::dock_mode_from_string(StringView value)
{
    if (value == "bottom"sv)
        return DockMode::Bottom;
    if (value == "left"sv)
        return DockMode::Left;
    if (value == "window"sv)
        return DockMode::Window;
    return DockMode::Right;
}

String InspectorHost::dock_mode_to_string(DockMode mode)
{
    switch (mode) {
    case DockMode::Right:
        return "right"_string;
    case DockMode::Bottom:
        return "bottom"_string;
    case DockMode::Left:
        return "left"_string;
    case DockMode::Window:
        return "window"_string;
    }
    VERIFY_NOT_REACHED();
}

InspectorHost::InspectorHost(QWidget* parent, BrowserWindow& browser_window, WebContentView& content_view)
    : QWidget(parent)
    , m_browser_window(&browser_window)
    , m_content_view(content_view)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setOpaqueResize(false);
    m_splitter->setHandleWidth(1);
    m_splitter->setAccessibleName("Inspector splitter");
    m_splitter->addWidget(&m_content_view);
    layout->addWidget(m_splitter);

    QObject::connect(m_splitter, &QSplitter::splitterMoved, this, [this](int, int) {
        record_split_ratio_from_splitter();
    });

    update_splitter_theme();
}

InspectorHost::~InspectorHost()
{
    close_inspector();
}

InspectorHost::DockMode InspectorHost::choose_initial_dock_mode() const
{
    if (m_has_explicit_dock_choice)
        return m_dock_mode;
    if (m_browser_window && m_browser_window->is_private() == WebView::IsPrivate::No && s_remembered_regular_dock_mode.has_value())
        return *s_remembered_regular_dock_mode;
    return width() >= MINIMUM_SIDE_BY_SIDE_WIDTH ? DockMode::Right : DockMode::Bottom;
}

void InspectorHost::open(String const& initial_panel)
{
    if (m_inspector_view) {
        HubConnection::the().async_view_inspector_select_panel(m_content_view.view_id(), initial_panel);
        if (m_undocked_window) {
            m_undocked_window->show();
            m_undocked_window->raise();
            m_undocked_window->activateWindow();
        }
        focus_inspector();
        return;
    }

    m_dock_mode = choose_initial_dock_mode();
    if (m_browser_window && m_browser_window->is_private() == WebView::IsPrivate::No)
        m_split_ratio = s_remembered_regular_split_ratio;

    auto is_private = m_browser_window ? m_browser_window->is_private() : WebView::IsPrivate::No;
    WebContentViewInitialState initial_state {
        .is_private = is_private,
        .maximum_frames_per_second = m_browser_window ? m_browser_window->refresh_rate() : 60.0,
        .display_id = m_browser_window ? m_browser_window->display_id() : OptionalNone {},
        .screen_shows_high_dynamic_range = m_browser_window ? m_browser_window->screen_shows_high_dynamic_range() : false,
        .owner_view_id = m_content_view.view_id(),
        .is_tab = false,
    };

    m_inspector_view = new WebContentView(this, {}, AK::move(initial_state));
    m_inspector_view->setAccessibleName("Ladyflock Inspector");
    m_inspector_view->setMinimumSize(220, 140);

    apply_dock_layout();
    HubConnection::the().async_view_attach_inspector(
        m_content_view.view_id(),
        m_inspector_view->view_id(),
        initial_panel,
        dock_mode_to_string(m_dock_mode));
    focus_inspector();
}

void InspectorHost::toggle()
{
    if (is_open())
        close_inspector();
    else
        open("elements"_string);
}

void InspectorHost::close_inspector()
{
    if (!m_inspector_view && !m_undocked_window)
        return;

    bool had_focus = inspector_has_focus();

    if (auto* hub = HubConnection::the_if_connected())
        hub->async_view_detach_inspector(m_content_view.view_id());

    if (m_inspector_view) {
        m_inspector_view->setParent(nullptr);
        delete m_inspector_view;
        m_inspector_view = nullptr;
    }
    if (m_undocked_window) {
        m_undocked_window->removeEventFilter(this);
        m_undocked_window->hide();
        delete m_undocked_window;
        m_undocked_window = nullptr;
    }

    if (had_focus && m_content_view.isVisible())
        m_content_view.setFocus(Qt::OtherFocusReason);
}

void InspectorHost::set_dock_mode(DockMode mode, bool explicit_user_choice)
{
    m_dock_mode = mode;
    if (explicit_user_choice) {
        m_has_explicit_dock_choice = true;
        if (m_browser_window && m_browser_window->is_private() == WebView::IsPrivate::No)
            s_remembered_regular_dock_mode = mode;
    }

    if (m_inspector_view) {
        apply_dock_layout();
        if (auto* hub = HubConnection::the_if_connected())
            hub->async_view_inspector_set_dock_mode(m_content_view.view_id(), dock_mode_to_string(m_dock_mode));
    }
}

void InspectorHost::set_fifty_fifty_split()
{
    m_split_ratio = 0.5;
    if (m_browser_window && m_browser_window->is_private() == WebView::IsPrivate::No)
        s_remembered_regular_split_ratio = m_split_ratio;
    apply_splitter_sizes();
}

void InspectorHost::apply_dock_layout()
{
    if (!m_inspector_view)
        return;

    if (m_dock_mode == DockMode::Window) {
        if (!m_undocked_window) {
            m_undocked_window = new QWidget(nullptr, Qt::Window);
            m_undocked_window->resize(960, 640);
            auto* win_layout = new QVBoxLayout(m_undocked_window);
            win_layout->setContentsMargins(0, 0, 0, 0);
            win_layout->setSpacing(0);
            if (m_browser_window && m_browser_window->is_private() == WebView::IsPrivate::Yes)
                Theme::mark_private_window(*m_undocked_window);
            m_undocked_window->installEventFilter(this);
        }
        m_inspector_view->setParent(m_undocked_window);
        m_undocked_window->layout()->addWidget(m_inspector_view);
        m_inspector_view->show();
        update_undocked_window_title();
        m_undocked_window->show();
        m_undocked_window->raise();
        return;
    }

    // Docked mode: remove any undocked floating window first.
    if (m_undocked_window) {
        m_inspector_view->setParent(m_splitter);
        m_undocked_window->removeEventFilter(this);
        m_undocked_window->hide();
        delete m_undocked_window;
        m_undocked_window = nullptr;
    }

    m_splitter->setOrientation(m_dock_mode == DockMode::Bottom ? Qt::Vertical : Qt::Horizontal);
    if (m_dock_mode == DockMode::Left)
        m_splitter->insertWidget(0, m_inspector_view);
    else
        m_splitter->addWidget(m_inspector_view);

    m_inspector_view->show();
    update_splitter_theme();
    apply_splitter_sizes();
}

void InspectorHost::apply_splitter_sizes()
{
    if (!m_inspector_view || m_dock_mode == DockMode::Window)
        return;

    int total = m_dock_mode == DockMode::Bottom ? height() : width();
    if (total <= 0)
        total = m_dock_mode == DockMode::Bottom ? 600 : 1000;

    int inspector_size = clamp(static_cast<int>(total * m_split_ratio), 220, max(220, total - 220));
    int page_size = max(220, total - inspector_size);
    if (m_dock_mode == DockMode::Left)
        m_splitter->setSizes({ inspector_size, page_size });
    else
        m_splitter->setSizes({ page_size, inspector_size });
}

void InspectorHost::record_split_ratio_from_splitter()
{
    if (!m_inspector_view || m_dock_mode == DockMode::Window)
        return;

    auto sizes = m_splitter->sizes();
    if (sizes.size() != 2)
        return;
    int total = sizes[0] + sizes[1];
    if (total <= 0)
        return;

    int inspector_size = m_dock_mode == DockMode::Left ? sizes[0] : sizes[1];
    m_split_ratio = clamp(static_cast<double>(inspector_size) / static_cast<double>(total), 0.15, 0.85);
    if (m_browser_window && m_browser_window->is_private() == WebView::IsPrivate::No)
        s_remembered_regular_split_ratio = m_split_ratio;
}

void InspectorHost::update_splitter_theme()
{
    auto divider = Theme::chrome(this->palette()).divider;
    m_splitter->setStyleSheet(QString("QSplitter::handle { background-color: %1; }").arg(divider.name(QColor::HexArgb)));
}

void InspectorHost::update_undocked_window_title()
{
    if (!m_undocked_window)
        return;
    QString title = m_target_title.isEmpty() ? QString("Inspector") : QString("Inspector — %1").arg(m_target_title);
    if (m_browser_window && m_browser_window->is_private() == WebView::IsPrivate::Yes)
        title += " (Private)";
    m_undocked_window->setWindowTitle(title);
}

bool InspectorHost::inspector_has_focus() const
{
    auto* focused = QApplication::focusWidget();
    if (!focused || !m_inspector_view)
        return false;
    return focused == m_inspector_view || m_inspector_view->isAncestorOf(focused);
}

void InspectorHost::focus_inspector()
{
    if (!m_inspector_view)
        return;
    m_inspector_view->setFocus(Qt::ShortcutFocusReason);
}

void InspectorHost::set_browser_window(BrowserWindow& window)
{
    m_browser_window = &window;
    update_splitter_theme();
    update_undocked_window_title();
}

void InspectorHost::update_target_title(QString const& title)
{
    m_target_title = title;
    update_undocked_window_title();
}

bool InspectorHost::eventFilter(QObject* object, QEvent* event)
{
    if (object == m_undocked_window && event->type() == QEvent::Close) {
        event->ignore();
        close_inspector();
        return true;
    }
    return QWidget::eventFilter(object, event);
}

void InspectorHost::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_inspector_view && m_dock_mode != DockMode::Window)
        apply_splitter_sizes();
}

}
