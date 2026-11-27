/*
 * Copyright (c) 2026, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibWebView/BrowsingSession.h>

#include <QPointer>
#include <QSplitter>
#include <QWidget>

class QCloseEvent;

namespace Ladybird {

class BrowserWindow;
class WebContentView;

// Hosts a tab's page WebContentView alongside its optional local Inspector view (docked in a
// QSplitter on the right/bottom/left, or floated in its own native top-level window).
class InspectorHost final : public QWidget {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    enum class DockMode : u8 {
        Right,
        Bottom,
        Left,
        Window,
    };

    InspectorHost(QWidget* parent, BrowserWindow& browser_window, WebContentView& content_view);
    virtual ~InspectorHost() override;

    static DockMode dock_mode_from_string(StringView);
    static String dock_mode_to_string(DockMode);

    void open(String const& initial_panel = "elements"_string);
    void toggle();
    void close_inspector();

    bool is_open() const { return m_inspector_view != nullptr; }
    DockMode dock_mode() const { return m_dock_mode; }
    void set_dock_mode(DockMode, bool explicit_user_choice = true);
    void set_fifty_fifty_split();
    double split_ratio() const { return m_split_ratio; }

    WebContentView* inspector_view() const { return m_inspector_view; }
    QWidget* undocked_window() const { return m_undocked_window; }
    QSplitter& splitter() { return *m_splitter; }

    bool inspector_has_focus() const;
    void focus_inspector();
    void set_browser_window(BrowserWindow&);
    void update_target_title(QString const& title);

private:
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;

    DockMode choose_initial_dock_mode() const;
    void apply_dock_layout();
    void apply_splitter_sizes();
    void update_splitter_theme();
    void update_undocked_window_title();
    void record_split_ratio_from_splitter();

    BrowserWindow* m_browser_window { nullptr };
    WebContentView& m_content_view;
    QSplitter* m_splitter { nullptr };
    QPointer<WebContentView> m_inspector_view;
    QPointer<QWidget> m_undocked_window;

    DockMode m_dock_mode { DockMode::Right };
    bool m_has_explicit_dock_choice { false };
    double m_split_ratio { 0.38 };
    QString m_target_title;
};

}
