/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>
#include <LibWebView/Export.h>

namespace WebView::Extensions {

// Non-UI controller for sidebar_action extension state, global/window/tab precedence,
// gesture-gated open/toggle, open_at_install, and persisted sidebar width.
class WEBVIEW_API ExtensionSidebarController {
public:
    static constexpr u32 default_width_px = 320;
    static constexpr u32 min_width_px = 200;
    static constexpr u32 max_width_px = 640;

    struct WindowSidebarState {
        bool is_open { false };
        u32 width_px { default_width_px };
        Optional<String> selected_extension_id;
    };

    void configure_from_manifest(String const& extension_id, Web::Extensions::ExtensionManifest const&, bool first_install = false, u64 default_window_id = 1);
    void remove_extension(StringView extension_id);
    void clear_tab_state(u64 tab_id);
    void clear_window_state(u64 window_id);

    void set_icon(String const&, Optional<String>, Optional<u64> tab_id = {}, Optional<u64> window_id = {});
    void set_panel(String const& extension_id, Optional<String> panel_url, Optional<u64> tab_id = {}, Optional<u64> window_id = {});
    Optional<String> get_panel(StringView extension_id, Optional<u64> tab_id = {}, Optional<u64> window_id = {}) const;

    void set_title(String const& extension_id, Optional<String> title, Optional<u64> tab_id = {}, Optional<u64> window_id = {});
    String get_title(StringView extension_id, Optional<u64> tab_id = {}, Optional<u64> window_id = {}) const;

    ErrorOr<void> open(String const& extension_id, u64 window_id, bool has_user_gesture);
    void close(StringView extension_id, u64 window_id);
    ErrorOr<bool> toggle(String const& extension_id, u64 window_id, bool has_user_gesture);
    bool is_open(StringView extension_id, u64 window_id) const;

    Web::Extensions::SidebarActionState resolved_state(StringView extension_id, Optional<u64> tab_id = {}, Optional<u64> window_id = {}) const;

    WindowSidebarState window_state(u64 window_id) const;
    void set_width(u64 window_id, u32 width_px);
    void set_persisted_default_width(u32 width_px);
    u32 persisted_default_width() const { return m_default_width_px; }

private:
    struct StateOverride {
        Optional<String> icon_path;
        Optional<String> panel_url;
        Optional<String> title;
    };

    struct ExtensionEntry {
        Web::Extensions::SidebarActionState global_state;
        HashMap<u64, StateOverride> window_overrides;
        HashMap<u64, StateOverride> tab_overrides;
    };

    HashMap<String, ExtensionEntry> m_entries;
    HashMap<u64, WindowSidebarState> m_windows;
    u32 m_default_width_px { default_width_px };
};

}
