/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>
#include <LibWebView/Export.h>

namespace WebView::Extensions {

// Non-UI controller for MV2 browser_action / MV3 action state, precedence (tab > window > global),
// and gesture-scoped action invocations (popup or onClicked + activeTab grant).
class WEBVIEW_API ExtensionActionController {
public:
    void configure_from_manifest(String const& extension_id, Web::Extensions::ExtensionManifest const&);
    void remove_extension(StringView extension_id);
    void clear_tab_state(u64 tab_id);
    void clear_window_state(u64 window_id);

    void set_title(String const& extension_id, Optional<String> title, Optional<u64> tab_id = {}, Optional<u64> window_id = {});
    String get_title(StringView extension_id, Optional<u64> tab_id = {}, Optional<u64> window_id = {}) const;

    void set_badge_text(String const& extension_id, Optional<String> text, Optional<u64> tab_id = {}, Optional<u64> window_id = {});
    String get_badge_text(StringView extension_id, Optional<u64> tab_id = {}, Optional<u64> window_id = {}) const;

    void set_badge_background_color(String const& extension_id, Optional<String> color, Optional<u64> tab_id = {}, Optional<u64> window_id = {});
    String get_badge_background_color(StringView extension_id, Optional<u64> tab_id = {}, Optional<u64> window_id = {}) const;

    void set_popup(String const& extension_id, Optional<String> popup_path, Optional<u64> tab_id = {}, Optional<u64> window_id = {});
    Optional<String> get_popup(StringView extension_id, Optional<u64> tab_id = {}, Optional<u64> window_id = {}) const;

    void set_icon(String const& extension_id, Optional<String> icon_path, Optional<u64> tab_id = {}, Optional<u64> window_id = {});
    void set_enabled(String const& extension_id, bool enabled, Optional<u64> tab_id = {});
    bool is_enabled(StringView extension_id, Optional<u64> tab_id = {}) const;

    void set_pinned(String const& extension_id, bool pinned);

    Web::Extensions::ExtensionActionState resolved_state(StringView extension_id, Optional<u64> tab_id = {}, Optional<u64> window_id = {}) const;

    enum class InvokeOutcome : u8 {
        Disabled,
        MissingExtension,
        OpenedPopup,
        DispatchedOnClicked,
    };
    InvokeOutcome invoke_action(
        StringView extension_id,
        u64 tab_id,
        u64 window_id,
        Function<void(Optional<String> const& popup_path)> const& on_open_popup,
        Function<void()> const& on_dispatch_clicked) const;

private:
    struct StateOverride {
        Optional<String> title;
        Optional<String> badge_text;
        Optional<String> badge_background_color;
        Optional<Optional<String>> popup;
        Optional<String> icon_path;
        Optional<bool> enabled;
    };

    struct ExtensionEntry {
        Web::Extensions::ExtensionActionState global_state;
        Optional<String> default_icon;
        HashMap<u64, StateOverride> window_overrides;
        HashMap<u64, StateOverride> tab_overrides;
    };

    HashMap<String, ExtensionEntry> m_entries;
};

}
