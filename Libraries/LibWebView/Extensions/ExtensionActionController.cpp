/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Extensions/ExtensionActionController.h>

namespace WebView::Extensions {

void ExtensionActionController::configure_from_manifest(String const& extension_id, Web::Extensions::ExtensionManifest const& manifest)
{
    ExtensionEntry entry;
    entry.global_state.extension_id = extension_id;
    entry.global_state.title = manifest.action().has_value() && manifest.action()->default_title.has_value()
        ? *manifest.action()->default_title
        : manifest.name();
    entry.global_state.badge_text = {};
    entry.global_state.badge_background_color = "#4A90E2"_string;
    if (manifest.action().has_value()) {
        entry.global_state.popup_url = manifest.action()->default_popup;
        entry.global_state.icon_path = manifest.preferred_icon(manifest.action()->default_icons);
    }
    if (!entry.global_state.icon_path.has_value())
        entry.global_state.icon_path = manifest.preferred_icon(manifest.icons());
    entry.global_state.enabled = true;
    m_entries.set(extension_id, move(entry));
}

void ExtensionActionController::remove_extension(StringView extension_id)
{
    m_entries.remove(extension_id);
}

void ExtensionActionController::clear_tab_state(u64 tab_id)
{
    for (auto& [_, entry] : m_entries)
        entry.tab_overrides.remove(tab_id);
}

void ExtensionActionController::clear_window_state(u64 window_id)
{
    for (auto& [_, entry] : m_entries)
        entry.window_overrides.remove(window_id);
}

template<typename Getter, typename Setter>
static void apply_scoped_override(auto& entry, Optional<u64> tab_id, Optional<u64> window_id, Getter&& get_global, Setter&& set_override)
{
    if (tab_id.has_value()) {
        auto& ov = entry.tab_overrides.ensure(*tab_id);
        set_override(ov);
    } else if (window_id.has_value()) {
        auto& ov = entry.window_overrides.ensure(*window_id);
        set_override(ov);
    } else {
        get_global(entry.global_state);
    }
}

void ExtensionActionController::set_title(String const& extension_id, Optional<String> title, Optional<u64> tab_id, Optional<u64> window_id)
{
    auto it = m_entries.find(extension_id);
    if (it == m_entries.end())
        return;
    apply_scoped_override(
        it->value,
        tab_id,
        window_id,
        [&](Web::Extensions::ExtensionActionState& global) {
            global.title = title.value_or({});
        },
        [&](StateOverride& ov) {
            ov.title = title;
        });
}

String ExtensionActionController::get_title(StringView extension_id, Optional<u64> tab_id, Optional<u64> window_id) const
{
    return resolved_state(extension_id, tab_id, window_id).title;
}

void ExtensionActionController::set_badge_text(String const& extension_id, Optional<String> text, Optional<u64> tab_id, Optional<u64> window_id)
{
    auto it = m_entries.find(extension_id);
    if (it == m_entries.end())
        return;
    apply_scoped_override(
        it->value,
        tab_id,
        window_id,
        [&](Web::Extensions::ExtensionActionState& global) {
            global.badge_text = text.value_or({});
        },
        [&](StateOverride& ov) {
            ov.badge_text = text;
        });
}

String ExtensionActionController::get_badge_text(StringView extension_id, Optional<u64> tab_id, Optional<u64> window_id) const
{
    return resolved_state(extension_id, tab_id, window_id).badge_text;
}

void ExtensionActionController::set_badge_background_color(String const& extension_id, Optional<String> color, Optional<u64> tab_id, Optional<u64> window_id)
{
    auto it = m_entries.find(extension_id);
    if (it == m_entries.end())
        return;
    apply_scoped_override(
        it->value,
        tab_id,
        window_id,
        [&](Web::Extensions::ExtensionActionState& global) {
            global.badge_background_color = color.value_or("#4A90E2"_string);
        },
        [&](StateOverride& ov) {
            ov.badge_background_color = color;
        });
}

String ExtensionActionController::get_badge_background_color(StringView extension_id, Optional<u64> tab_id, Optional<u64> window_id) const
{
    return resolved_state(extension_id, tab_id, window_id).badge_background_color;
}

void ExtensionActionController::set_popup(String const& extension_id, Optional<String> popup_path, Optional<u64> tab_id, Optional<u64> window_id)
{
    auto it = m_entries.find(extension_id);
    if (it == m_entries.end())
        return;
    if (popup_path.has_value() && popup_path->is_empty())
        popup_path.clear();

    apply_scoped_override(
        it->value,
        tab_id,
        window_id,
        [&](Web::Extensions::ExtensionActionState& global) {
            global.popup_url = popup_path;
        },
        [&](StateOverride& ov) {
            ov.popup.emplace(popup_path);
        });
}

Optional<String> ExtensionActionController::get_popup(StringView extension_id, Optional<u64> tab_id, Optional<u64> window_id) const
{
    return resolved_state(extension_id, tab_id, window_id).popup_url;
}

void ExtensionActionController::set_icon(String const& extension_id, Optional<String> icon_path, Optional<u64> tab_id, Optional<u64> window_id)
{
    auto it = m_entries.find(extension_id);
    if (it == m_entries.end())
        return;
    apply_scoped_override(
        it->value,
        tab_id,
        window_id,
        [&](Web::Extensions::ExtensionActionState& global) {
            global.icon_path = icon_path;
        },
        [&](StateOverride& ov) {
            ov.icon_path = icon_path;
        });
}

void ExtensionActionController::set_enabled(String const& extension_id, bool enabled, Optional<u64> tab_id)
{
    auto it = m_entries.find(extension_id);
    if (it == m_entries.end())
        return;
    if (tab_id.has_value())
        it->value.tab_overrides.ensure(*tab_id).enabled = enabled;
    else
        it->value.global_state.enabled = enabled;
}

bool ExtensionActionController::is_enabled(StringView extension_id, Optional<u64> tab_id) const
{
    return resolved_state(extension_id, tab_id, {}).enabled;
}

void ExtensionActionController::set_pinned(String const& extension_id, bool pinned)
{
    if (auto it = m_entries.find(extension_id); it != m_entries.end())
        it->value.global_state.pinned = pinned;
}

Web::Extensions::ExtensionActionState ExtensionActionController::resolved_state(StringView extension_id, Optional<u64> tab_id, Optional<u64> window_id) const
{
    auto it = m_entries.find(extension_id);
    if (it == m_entries.end())
        return {};

    auto state = it->value.global_state;
    auto merge = [&](StateOverride const& ov) {
        if (ov.title.has_value())
            state.title = *ov.title;
        if (ov.badge_text.has_value())
            state.badge_text = *ov.badge_text;
        if (ov.badge_background_color.has_value())
            state.badge_background_color = *ov.badge_background_color;
        if (ov.popup.has_value())
            state.popup_url = *ov.popup;
        if (ov.icon_path.has_value())
            state.icon_path = *ov.icon_path;
        if (ov.enabled.has_value())
            state.enabled = *ov.enabled;
    };

    if (window_id.has_value()) {
        if (auto win_ov = it->value.window_overrides.get(*window_id); win_ov.has_value())
            merge(*win_ov);
    }
    if (tab_id.has_value()) {
        if (auto tab_ov = it->value.tab_overrides.get(*tab_id); tab_ov.has_value())
            merge(*tab_ov);
    }
    return state;
}

ExtensionActionController::InvokeOutcome ExtensionActionController::invoke_action(
    StringView extension_id,
    u64 tab_id,
    u64 window_id,
    Function<void(Optional<String> const& popup_path)> const& on_open_popup,
    Function<void()> const& on_dispatch_clicked) const
{
    if (!m_entries.contains(extension_id))
        return InvokeOutcome::MissingExtension;

    auto state = resolved_state(extension_id, tab_id, window_id);
    if (!state.enabled)
        return InvokeOutcome::Disabled;

    if (state.popup_url.has_value() && !state.popup_url->is_empty()) {
        if (on_open_popup)
            on_open_popup(state.popup_url);
        return InvokeOutcome::OpenedPopup;
    }

    if (on_dispatch_clicked)
        on_dispatch_clicked();
    return InvokeOutcome::DispatchedOnClicked;
}

}
