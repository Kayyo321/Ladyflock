/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibWebView/Extensions/ExtensionSidebarController.h>

namespace WebView::Extensions {

void ExtensionSidebarController::configure_from_manifest(String const& extension_id, Web::Extensions::ExtensionManifest const& manifest, bool first_install, u64 default_window_id)
{
    if (!manifest.sidebar_action().has_value()) {
        m_entries.remove(extension_id);
        return;
    }

    auto const& cfg = *manifest.sidebar_action();
    ExtensionEntry entry;
    entry.global_state.extension_id = extension_id;
    entry.global_state.title = cfg.default_title.value_or(manifest.name());
    entry.global_state.panel_url = cfg.default_panel;
    entry.global_state.icon_path = manifest.preferred_icon(cfg.default_icons);
    entry.global_state.open_at_install = cfg.open_at_install;
    entry.global_state.is_open = false;
    m_entries.set(extension_id, move(entry));

    if (first_install && cfg.open_at_install) {
        auto& win = m_windows.ensure(default_window_id, [&] {
            return WindowSidebarState { .is_open = false, .width_px = m_default_width_px, .selected_extension_id = {} };
        });
        win.is_open = true;
        win.selected_extension_id = extension_id;
    }
}

void ExtensionSidebarController::remove_extension(StringView extension_id)
{
    m_entries.remove(extension_id);
    for (auto& [_, win] : m_windows) {
        if (win.selected_extension_id == extension_id) {
            win.is_open = false;
            win.selected_extension_id.clear();
        }
    }
}

void ExtensionSidebarController::clear_tab_state(u64 tab_id)
{
    for (auto& [_, entry] : m_entries)
        entry.tab_overrides.remove(tab_id);
}

void ExtensionSidebarController::clear_window_state(u64 window_id)
{
    for (auto& [_, entry] : m_entries)
        entry.window_overrides.remove(window_id);
    m_windows.remove(window_id);
}

void ExtensionSidebarController::set_icon(String const& id, Optional<String> path, Optional<u64> tab, Optional<u64> window)
{
    auto it = m_entries.find(id);
    if (it == m_entries.end())
        return;
    if (tab.has_value())
        it->value.tab_overrides.ensure(*tab).icon_path = move(path);
    else if (window.has_value())
        it->value.window_overrides.ensure(*window).icon_path = move(path);
    else
        it->value.global_state.icon_path = move(path);
}

void ExtensionSidebarController::set_panel(String const& extension_id, Optional<String> panel_url, Optional<u64> tab_id, Optional<u64> window_id)
{
    auto it = m_entries.find(extension_id);
    if (it == m_entries.end())
        return;
    if (tab_id.has_value())
        it->value.tab_overrides.ensure(*tab_id).panel_url = move(panel_url);
    else if (window_id.has_value())
        it->value.window_overrides.ensure(*window_id).panel_url = move(panel_url);
    else
        it->value.global_state.panel_url = move(panel_url);
}

Optional<String> ExtensionSidebarController::get_panel(StringView extension_id, Optional<u64> tab_id, Optional<u64> window_id) const
{
    return resolved_state(extension_id, tab_id, window_id).panel_url;
}

void ExtensionSidebarController::set_title(String const& extension_id, Optional<String> title, Optional<u64> tab_id, Optional<u64> window_id)
{
    auto it = m_entries.find(extension_id);
    if (it == m_entries.end())
        return;
    if (tab_id.has_value())
        it->value.tab_overrides.ensure(*tab_id).title = move(title);
    else if (window_id.has_value())
        it->value.window_overrides.ensure(*window_id).title = move(title);
    else
        it->value.global_state.title = title.value_or({});
}

String ExtensionSidebarController::get_title(StringView extension_id, Optional<u64> tab_id, Optional<u64> window_id) const
{
    return resolved_state(extension_id, tab_id, window_id).title;
}

ErrorOr<void> ExtensionSidebarController::open(String const& extension_id, u64 window_id, bool has_user_gesture)
{
    if (!has_user_gesture)
        return Error::from_string_literal("sidebarAction.open requires a user gesture");
    if (!m_entries.contains(extension_id))
        return Error::from_string_literal("Extension has no sidebar_action");

    auto& win = m_windows.ensure(window_id, [&] {
        return WindowSidebarState { .is_open = false, .width_px = m_default_width_px, .selected_extension_id = {} };
    });
    win.is_open = true;
    win.selected_extension_id = extension_id;
    return {};
}

void ExtensionSidebarController::close(StringView extension_id, u64 window_id)
{
    auto it = m_windows.find(window_id);
    if (it == m_windows.end())
        return;
    if (it->value.selected_extension_id == extension_id)
        it->value.is_open = false;
}

ErrorOr<bool> ExtensionSidebarController::toggle(String const& extension_id, u64 window_id, bool has_user_gesture)
{
    if (!has_user_gesture)
        return Error::from_string_literal("sidebarAction.toggle requires a user gesture");
    if (is_open(extension_id, window_id)) {
        close(extension_id, window_id);
        return false;
    }
    TRY(open(extension_id, window_id, true));
    return true;
}

bool ExtensionSidebarController::is_open(StringView extension_id, u64 window_id) const
{
    auto it = m_windows.find(window_id);
    if (it == m_windows.end())
        return false;
    return it->value.is_open && it->value.selected_extension_id == extension_id;
}

Web::Extensions::SidebarActionState ExtensionSidebarController::resolved_state(StringView extension_id, Optional<u64> tab_id, Optional<u64> window_id) const
{
    auto it = m_entries.find(extension_id);
    if (it == m_entries.end())
        return {};

    auto state = it->value.global_state;
    auto merge = [&](StateOverride const& ov) {
        if (ov.title.has_value())
            state.title = *ov.title;
        if (ov.icon_path.has_value())
            state.icon_path = ov.icon_path;
        if (ov.panel_url.has_value())
            state.panel_url = *ov.panel_url;
    };

    if (window_id.has_value()) {
        if (auto win_ov = it->value.window_overrides.get(*window_id); win_ov.has_value())
            merge(*win_ov);
        state.is_open = is_open(extension_id, *window_id);
    }
    if (tab_id.has_value()) {
        if (auto tab_ov = it->value.tab_overrides.get(*tab_id); tab_ov.has_value())
            merge(*tab_ov);
    }
    return state;
}

ExtensionSidebarController::WindowSidebarState ExtensionSidebarController::window_state(u64 window_id) const
{
    return m_windows.get(window_id).value_or(WindowSidebarState {
        .is_open = false,
        .width_px = m_default_width_px,
        .selected_extension_id = {},
    });
}

void ExtensionSidebarController::set_width(u64 window_id, u32 width_px)
{
    u32 clamped = clamp(width_px, min_width_px, max_width_px);
    m_default_width_px = clamped;
    auto& win = m_windows.ensure(window_id, [&] {
        return WindowSidebarState { .is_open = false, .width_px = clamped, .selected_extension_id = {} };
    });
    win.width_px = clamped;
}

void ExtensionSidebarController::set_persisted_default_width(u32 width_px)
{
    m_default_width_px = clamp(width_px, min_width_px, max_width_px);
    for (auto& [_, win] : m_windows)
        win.width_px = m_default_width_px;
}

}
