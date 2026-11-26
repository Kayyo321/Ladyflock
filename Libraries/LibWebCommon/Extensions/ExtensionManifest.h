/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

namespace Web::Extensions {

struct WEBCOMMON_API ContentScriptRule {
    Vector<MatchPattern> matches;
    Vector<MatchPattern> exclude_matches;
    Vector<String> include_globs;
    Vector<String> exclude_globs;
    Vector<String> js;
    Vector<String> css;
    RunAt run_at { RunAt::DocumentIdle };
    bool all_frames { false };
    bool match_about_blank { false };
};

struct WEBCOMMON_API BackgroundConfig {
    Vector<String> scripts;
    Optional<String> page;
    Optional<String> service_worker;
    bool persistent { true };

    bool has_background() const { return !scripts.is_empty() || page.has_value() || service_worker.has_value(); }
};

struct WEBCOMMON_API ActionConfig {
    Optional<String> default_popup;
    Optional<String> default_title;
    HashMap<String, String> default_icons;
};

struct WEBCOMMON_API SidebarActionConfig {
    Optional<String> default_panel;
    Optional<String> default_title;
    HashMap<String, String> default_icons;
    bool open_at_install { true };
};

struct WEBCOMMON_API OptionsUIConfig {
    String page;
    bool open_in_tab { false };
};

struct WEBCOMMON_API WebAccessibleResourceRule {
    Vector<String> resources;
    Vector<MatchPattern> matches;
    Vector<String> extension_ids;
};

class WEBCOMMON_API ExtensionManifest {
public:
    static ErrorOr<ExtensionManifest> parse(StringView json_text);
    static ErrorOr<String> normalize_package_relative_path(StringView raw_path);
    static bool is_valid_extension_csp(StringView csp, ManifestVersion);
    static String default_extension_csp(ManifestVersion);

    ManifestVersion manifest_version() const { return m_manifest_version; }
    String const& name() const { return m_name; }
    String const& version() const { return m_version; }
    Optional<String> const& description() const { return m_description; }
    Optional<String> const& gecko_id() const { return m_gecko_id; }
    Optional<String> const& strict_min_version() const { return m_strict_min_version; }
    Optional<String> const& strict_max_version() const { return m_strict_max_version; }
    ExtensionDataCollectionPermissions const& data_collection_permissions() const { return m_data_collection_permissions; }

    Vector<String> const& permissions() const { return m_permissions; }
    Vector<String> const& optional_permissions() const { return m_optional_permissions; }
    Vector<MatchPattern> const& host_permissions() const { return m_host_permissions; }
    Vector<MatchPattern> const& optional_host_permissions() const { return m_optional_host_permissions; }

    Vector<ContentScriptRule> const& content_scripts() const { return m_content_scripts; }
    Optional<BackgroundConfig> const& background() const { return m_background; }
    Optional<ActionConfig> const& action() const { return m_action; }
    Optional<SidebarActionConfig> const& sidebar_action() const { return m_sidebar_action; }
    Optional<OptionsUIConfig> const& options_ui() const { return m_options_ui; }
    HashMap<String, String> const& icons() const { return m_icons; }
    Vector<WebAccessibleResourceRule> const& web_accessible_resources() const { return m_web_accessible_resources; }
    String const& content_security_policy() const { return m_content_security_policy; }
    bool has_invalid_csp() const { return m_has_invalid_csp; }
    Vector<String> const& top_level_keys() const { return m_top_level_keys; }
    String const& raw_json() const { return m_raw_json; }

    static ErrorOr<HashMap<String, String>> parse_icon_map(JsonValue const&);
    static Optional<String> preferred_icon(HashMap<String, String> const&);
    bool has_permission(StringView name) const;
    bool has_host_permission_for(URL::URL const&) const;
    bool is_web_accessible(StringView relative_path, Optional<URL::URL> const& initiator_url, Optional<StringView> initiator_extension_id = {}) const;

    JsonObject to_json() const;

private:
    ManifestVersion m_manifest_version { ManifestVersion::V2 };
    String m_name;
    String m_version;
    Optional<String> m_description;
    Optional<String> m_gecko_id;
    Optional<String> m_strict_min_version;
    Optional<String> m_strict_max_version;
    ExtensionDataCollectionPermissions m_data_collection_permissions;

    Vector<String> m_permissions;
    Vector<String> m_optional_permissions;
    Vector<MatchPattern> m_host_permissions;
    Vector<MatchPattern> m_optional_host_permissions;

    Vector<ContentScriptRule> m_content_scripts;
    Optional<BackgroundConfig> m_background;
    Optional<ActionConfig> m_action;
    Optional<SidebarActionConfig> m_sidebar_action;
    Optional<OptionsUIConfig> m_options_ui;
    HashMap<String, String> m_icons;
    Vector<WebAccessibleResourceRule> m_web_accessible_resources;
    String m_content_security_policy;
    bool m_has_invalid_csp { false };
    Vector<String> m_top_level_keys;
    String m_raw_json;
};

}
