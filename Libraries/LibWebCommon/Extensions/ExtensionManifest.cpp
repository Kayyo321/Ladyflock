/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>

namespace Web::Extensions {

ErrorOr<String> ExtensionManifest::normalize_package_relative_path(StringView raw_path)
{
    if (raw_path.is_empty() || raw_path.contains('\0') || raw_path.contains('\\'))
        return Error::from_string_literal("Invalid extension resource path");

    // Strip leading '/' (WebExtensions manifest paths like "/popup.html" are package-root relative).
    while (raw_path.starts_with('/'))
        raw_path = raw_path.substring_view(1);

    if (raw_path.is_empty())
        return Error::from_string_literal("Empty extension resource path");

    Vector<StringView> clean_segments;
    for (auto segment : raw_path.split_view('/', SplitBehavior::KeepEmpty)) {
        if (segment.is_empty() || segment == "."sv)
            continue;
        if (segment == ".."sv)
            return Error::from_string_literal("Path traversal ('..') is forbidden in extension resource paths");
        clean_segments.append(segment);
    }

    if (clean_segments.is_empty())
        return Error::from_string_literal("Empty extension resource path");

    StringBuilder builder;
    for (size_t i = 0; i < clean_segments.size(); ++i) {
        if (i > 0)
            builder.append('/');
        builder.append(clean_segments[i]);
    }
    return builder.to_string();
}

String ExtensionManifest::default_extension_csp(ManifestVersion version)
{
    if (version == ManifestVersion::V3)
        return "script-src 'self'; upgrade-insecure-requests;"_string;
    return "script-src 'self'; object-src 'self'"_string;
}

bool ExtensionManifest::is_valid_extension_csp(StringView csp, ManifestVersion version)
{
    if (csp.is_empty())
        return false;

    auto lowered = csp.to_ascii_lowercase_string();
    auto lowered_view = lowered.bytes_as_string_view();

    // Firefox forbids 'unsafe-eval', remote http:/ws:/ftp: script sources, and wildcard '*' in script-src.
    if (lowered_view.contains("'unsafe-eval'"sv))
        return false;
    if (lowered_view.contains("http://"sv) || lowered_view.contains("ws://"sv) || lowered_view.contains("ftp://"sv))
        return false;

    if (version == ManifestVersion::V3) {
        // MV3 extension_pages must not allow remote https:// origins or unsafe-inline in script-src.
        if (lowered_view.contains("https://"sv) || lowered_view.contains("'unsafe-inline'"sv))
            return false;
    }

    bool has_script_src = false;
    for (auto directive : lowered_view.split_view(';')) {
        auto trimmed = directive.trim_whitespace();
        if (!trimmed.starts_with("script-src"sv) && !trimmed.starts_with("default-src"sv))
            continue;
        if (trimmed.starts_with("script-src"sv))
            has_script_src = true;

        auto tokens = trimmed.split_view_if([](char c) { return c == ' ' || c == '\t'; });
        for (size_t i = 1; i < tokens.size(); ++i) {
            auto token = tokens[i];
            if (token == "*"sv || token == "http:"sv || token == "https:"sv || token == "data:"sv)
                return false;
        }
    }

    // Require explicit script-src (or default-src) restriction.
    if (!has_script_src && !lowered_view.contains("default-src"sv))
        return false;

    return true;
}

ErrorOr<HashMap<String, String>> ExtensionManifest::parse_icon_map(JsonValue const& value)
{
    HashMap<String, String> icons;
    if (value.is_string()) {
        icons.set("default"_string, TRY(ExtensionManifest::normalize_package_relative_path(value.as_string())));
        return icons;
    }
    if (!value.is_object())
        return Error::from_string_literal("Icons entry must be string or object");
    TRY(value.as_object().try_for_each_member([&](String const& size_key, JsonValue const& path_val) -> ErrorOr<void> {
        if (!path_val.is_string())
            return Error::from_string_literal("Icon path must be a string");
        icons.set(size_key, TRY(ExtensionManifest::normalize_package_relative_path(path_val.as_string())));
        return {};
    }));
    return icons;
}

Optional<String> ExtensionManifest::preferred_icon(HashMap<String, String> const& icons)
{
    // The isolated image decoder accepts bitmap formats. Some packages declare SVG for their
    // listing and provide alternate bitmap artwork for actions or sidebars.
    auto decodable_path = [](String const& path) {
        auto name = path.bytes_as_string_view();
        return !name.ends_with(".svg"sv, CaseSensitivity::CaseInsensitive) && !name.ends_with(".svgz"sv, CaseSensitivity::CaseInsensitive);
    };
    if (auto path = icons.get("default"sv); path.has_value() && decodable_path(*path))
        return *path;
    // Choose the smallest icon at least as large as the HiDPI toolbar glyph, otherwise the largest available.
    constexpr unsigned desired_size = 48;
    Optional<unsigned> best_size;
    Optional<String> best_path;
    for (auto const& [key, path] : icons) {
        if (!decodable_path(path))
            continue;
        auto size = key.bytes_as_string_view().to_number<unsigned>();
        if (!size.has_value() || !*size)
            continue;
        if (!best_size.has_value() || (*best_size < desired_size ? *size > *best_size : (*size >= desired_size && *size < *best_size))) {
            best_size = size;
            best_path = path;
        }
    }
    return best_path;
}

Optional<String> ExtensionManifest::package_icon_path() const
{
    if (auto icon = preferred_icon(icons()); icon.has_value())
        return icon;
    if (action().has_value()) {
        if (auto icon = preferred_icon(action()->default_icons); icon.has_value())
            return icon;
    }
    if (sidebar_action().has_value())
        return preferred_icon(sidebar_action()->default_icons);
    return {};
}

static ErrorOr<Vector<String>> parse_string_array(JsonArray const& array)
{
    Vector<String> result;
    result.ensure_capacity(array.size());
    for (auto const& item : array.values()) {
        if (!item.is_string())
            return Error::from_string_literal("Expected string in array");
        result.unchecked_append(item.as_string());
    }
    return result;
}

static ErrorOr<Vector<String>> parse_path_array(JsonArray const& array)
{
    Vector<String> result;
    result.ensure_capacity(array.size());
    for (auto const& item : array.values()) {
        if (!item.is_string())
            return Error::from_string_literal("Expected path string in array");
        result.unchecked_append(TRY(ExtensionManifest::normalize_package_relative_path(item.as_string())));
    }
    return result;
}

static ErrorOr<Vector<MatchPattern>> parse_match_pattern_array(JsonArray const& array)
{
    Vector<MatchPattern> result;
    result.ensure_capacity(array.size());
    for (auto const& item : array.values()) {
        if (!item.is_string())
            return Error::from_string_literal("Expected match pattern string in array");
        result.unchecked_append(TRY(MatchPattern::parse(item.as_string())));
    }
    return result;
}

ErrorOr<ExtensionManifest> ExtensionManifest::parse(StringView json_text)
{
    auto root_value = TRY(JsonValue::from_string(json_text));
    if (!root_value.is_object())
        return Error::from_string_literal("manifest.json root must be a JSON object");

    auto const& root = root_value.as_object();
    ExtensionManifest manifest;
    manifest.m_raw_json = TRY(String::from_utf8(json_text));

    root.for_each_member([&](String const& key, JsonValue const&) {
        manifest.m_top_level_keys.append(key);
    });

    auto mv = root.get_integer<u8>("manifest_version"sv);
    if (!mv.has_value() || (*mv != 2 && *mv != 3))
        return Error::from_string_literal("Unsupported or missing manifest_version (must be 2 or 3)");
    manifest.m_manifest_version = static_cast<ManifestVersion>(*mv);

    auto name = root.get_string("name"sv);
    if (!name.has_value() || name->is_empty())
        return Error::from_string_literal("Manifest missing required 'name'");
    manifest.m_name = *name;

    auto version = root.get_string("version"sv);
    if (!version.has_value() || version->is_empty())
        return Error::from_string_literal("Manifest missing required 'version'");
    manifest.m_version = *version;

    if (auto desc = root.get_string("description"sv); desc.has_value())
        manifest.m_description = *desc;

    if (auto icons_val = root.get("icons"sv); icons_val.has_value())
        manifest.m_icons = TRY(parse_icon_map(*icons_val));

    // Gecko metadata: browser_specific_settings.gecko (preferred) or applications.gecko (MV2 legacy).
    JsonObject const* gecko = nullptr;
    if (auto bss = root.get_object("browser_specific_settings"sv); bss.has_value()) {
        if (auto g = bss->get_object("gecko"sv); g.has_value())
            gecko = &(*g);
    }
    if (!gecko) {
        if (auto apps = root.get_object("applications"sv); apps.has_value()) {
            if (auto g = apps->get_object("gecko"sv); g.has_value())
                gecko = &(*g);
        }
    }
    if (gecko) {
        if (auto id = gecko->get_string("id"sv); id.has_value() && !id->is_empty())
            manifest.m_gecko_id = *id;
        if (auto min_ver = gecko->get_string("strict_min_version"sv); min_ver.has_value())
            manifest.m_strict_min_version = *min_ver;
        if (auto max_ver = gecko->get_string("strict_max_version"sv); max_ver.has_value())
            manifest.m_strict_max_version = *max_ver;
        if (auto dcp = gecko->get_object("data_collection_permissions"sv); dcp.has_value()) {
            if (auto req = dcp->get_array("required"sv); req.has_value())
                manifest.m_data_collection_permissions.required = TRY(parse_string_array(*req));
            if (auto opt = dcp->get_array("optional"sv); opt.has_value())
                manifest.m_data_collection_permissions.optional = TRY(parse_string_array(*opt));
        }
    }

    // Permissions (MV2 mixes API names & host patterns in 'permissions'; MV3 separates 'host_permissions').
    auto classify_permission = [&](String const& perm, Vector<String>& api_out, Vector<MatchPattern>& host_out) -> ErrorOr<void> {
        if (perm == "<all_urls>"sv || perm.contains("://"sv)) {
            host_out.append(TRY(MatchPattern::parse(perm)));
        } else {
            api_out.append(perm);
        }
        return {};
    };

    if (auto perms = root.get_array("permissions"sv); perms.has_value()) {
        for (auto const& val : perms->values()) {
            if (!val.is_string())
                return Error::from_string_literal("Permission entry must be a string");
            TRY(classify_permission(val.as_string(), manifest.m_permissions, manifest.m_host_permissions));
        }
    }
    if (auto opt_perms = root.get_array("optional_permissions"sv); opt_perms.has_value()) {
        for (auto const& val : opt_perms->values()) {
            if (!val.is_string())
                return Error::from_string_literal("Optional permission entry must be a string");
            TRY(classify_permission(val.as_string(), manifest.m_optional_permissions, manifest.m_optional_host_permissions));
        }
    }
    if (auto host_perms = root.get_array("host_permissions"sv); host_perms.has_value()) {
        auto parsed = TRY(parse_match_pattern_array(*host_perms));
        manifest.m_host_permissions.extend(move(parsed));
    }
    if (auto opt_host_perms = root.get_array("optional_host_permissions"sv); opt_host_perms.has_value()) {
        auto parsed = TRY(parse_match_pattern_array(*opt_host_perms));
        manifest.m_optional_host_permissions.extend(move(parsed));
    }

    // Content scripts
    if (auto cs_array = root.get_array("content_scripts"sv); cs_array.has_value()) {
        for (auto const& cs_val : cs_array->values()) {
            if (!cs_val.is_object())
                return Error::from_string_literal("content_scripts entry must be an object");
            auto const& cs_obj = cs_val.as_object();
            ContentScriptRule rule;

            auto matches = cs_obj.get_array("matches"sv);
            if (!matches.has_value() || matches->is_empty())
                return Error::from_string_literal("content_scripts entry requires non-empty 'matches'");
            rule.matches = TRY(parse_match_pattern_array(*matches));

            if (auto exclude_matches = cs_obj.get_array("exclude_matches"sv); exclude_matches.has_value())
                rule.exclude_matches = TRY(parse_match_pattern_array(*exclude_matches));
            if (auto include_globs = cs_obj.get_array("include_globs"sv); include_globs.has_value())
                rule.include_globs = TRY(parse_string_array(*include_globs));
            if (auto exclude_globs = cs_obj.get_array("exclude_globs"sv); exclude_globs.has_value())
                rule.exclude_globs = TRY(parse_string_array(*exclude_globs));
            if (auto js = cs_obj.get_array("js"sv); js.has_value())
                rule.js = TRY(parse_path_array(*js));
            if (auto css = cs_obj.get_array("css"sv); css.has_value())
                rule.css = TRY(parse_path_array(*css));

            if (auto run_at_str = cs_obj.get_string("run_at"sv); run_at_str.has_value()) {
                auto parsed_run_at = run_at_from_string(*run_at_str);
                if (!parsed_run_at.has_value())
                    return Error::from_string_literal("Invalid content_scripts.run_at value");
                rule.run_at = *parsed_run_at;
            }
            rule.all_frames = cs_obj.get_bool("all_frames"sv).value_or(false);
            if (cs_obj.has("match_about_blank"sv) && !cs_obj.get_bool("match_about_blank"sv).has_value())
                return Error::from_string_literal("content_scripts.match_about_blank must be a boolean");
            rule.match_about_blank = cs_obj.get_bool("match_about_blank"sv).value_or(false);

            manifest.m_content_scripts.append(move(rule));
        }
    }

    // Background page / event page
    if (auto bg = root.get_object("background"sv); bg.has_value()) {
        BackgroundConfig cfg;
        if (auto scripts = bg->get_array("scripts"sv); scripts.has_value())
            cfg.scripts = TRY(parse_path_array(*scripts));
        if (auto page = bg->get_string("page"sv); page.has_value())
            cfg.page = TRY(normalize_package_relative_path(*page));
        if (auto sw = bg->get_string("service_worker"sv); sw.has_value())
            cfg.service_worker = TRY(normalize_package_relative_path(*sw));
        cfg.is_module = bg->get_string("type"sv) == "module"sv;
        if (manifest.m_manifest_version == ManifestVersion::V3)
            cfg.persistent = false;
        else
            cfg.persistent = bg->get_bool("persistent"sv).value_or(true);
        manifest.m_background = move(cfg);
    }

    // Action (MV3 'action' or MV2 'browser_action')
    auto parse_action = [&](JsonObject const& obj) -> ErrorOr<ActionConfig> {
        ActionConfig cfg;
        if (auto popup = obj.get_string("default_popup"sv); popup.has_value() && !popup->is_empty())
            cfg.default_popup = TRY(normalize_package_relative_path(*popup));
        if (auto title = obj.get_string("default_title"sv); title.has_value())
            cfg.default_title = *title;
        if (auto icon = obj.get("default_icon"sv); icon.has_value())
            cfg.default_icons = TRY(parse_icon_map(*icon));
        if (obj.has("theme_icons"sv)) {
            auto variants = obj.get_array("theme_icons"sv);
            if (!variants.has_value())
                return Error::from_string_literal("theme_icons must be an array");
            HashMap<String, String> light_icons;
            HashMap<String, String> dark_icons;
            for (auto const& variant : variants->values()) {
                if (!variant.is_object())
                    return Error::from_string_literal("Invalid theme icon");
                auto const& entry = variant.as_object();
                auto size = entry.get_u32("size"sv);
                auto light = entry.get_string("light"sv);
                auto dark = entry.get_string("dark"sv);
                if (!size.has_value() || *size == 0 || !light.has_value() || !dark.has_value())
                    return Error::from_string_literal("Theme icon requires size, light and dark");
                auto key = String::number(*size);
                light_icons.set(key, TRY(normalize_package_relative_path(*light)));
                dark_icons.set(key, TRY(normalize_package_relative_path(*dark)));
            }
            cfg.light_icon = preferred_icon(light_icons);
            cfg.dark_icon = preferred_icon(dark_icons);
        }
        return cfg;
    };
    if (auto action_obj = root.get_object("action"sv); action_obj.has_value())
        manifest.m_action = TRY(parse_action(*action_obj));
    else if (auto ba_obj = root.get_object("browser_action"sv); ba_obj.has_value())
        manifest.m_action = TRY(parse_action(*ba_obj));

    // Sidebar action
    if (auto sa_obj = root.get_object("sidebar_action"sv); sa_obj.has_value()) {
        SidebarActionConfig cfg;
        if (auto panel = sa_obj->get_string("default_panel"sv); panel.has_value() && !panel->is_empty())
            cfg.default_panel = TRY(normalize_package_relative_path(*panel));
        if (auto title = sa_obj->get_string("default_title"sv); title.has_value())
            cfg.default_title = *title;
        if (auto icon = sa_obj->get("default_icon"sv); icon.has_value())
            cfg.default_icons = TRY(parse_icon_map(*icon));
        cfg.open_at_install = sa_obj->get_bool("open_at_install"sv).value_or(true);
        manifest.m_sidebar_action = move(cfg);
    }

    // Options UI / Options Page
    if (auto opt_ui = root.get_object("options_ui"sv); opt_ui.has_value()) {
        if (auto page = opt_ui->get_string("page"sv); page.has_value() && !page->is_empty()) {
            OptionsUIConfig cfg;
            cfg.page = TRY(normalize_package_relative_path(*page));
            cfg.open_in_tab = opt_ui->get_bool("open_in_tab"sv).value_or(false);
            manifest.m_options_ui = move(cfg);
        }
    } else if (auto opt_page = root.get_string("options_page"sv); opt_page.has_value() && !opt_page->is_empty()) {
        OptionsUIConfig cfg;
        cfg.page = TRY(normalize_package_relative_path(*opt_page));
        cfg.open_in_tab = true;
        manifest.m_options_ui = move(cfg);
    }

    // Web-accessible resources
    if (auto war = root.get_array("web_accessible_resources"sv); war.has_value()) {
        if (manifest.m_manifest_version == ManifestVersion::V2) {
            WebAccessibleResourceRule rule;
            for (auto const& item : war->values()) {
                if (!item.is_string())
                    return Error::from_string_literal("MV2 web_accessible_resources entries must be strings");
                rule.resources.append(TRY(normalize_package_relative_path(item.as_string())));
            }
            if (!rule.resources.is_empty())
                manifest.m_web_accessible_resources.append(move(rule));
        } else {
            for (auto const& item : war->values()) {
                if (!item.is_object())
                    return Error::from_string_literal("MV3 web_accessible_resources entries must be objects");
                auto const& obj = item.as_object();
                WebAccessibleResourceRule rule;
                if (auto res = obj.get_array("resources"sv); res.has_value())
                    rule.resources = TRY(parse_path_array(*res));
                if (auto matches = obj.get_array("matches"sv); matches.has_value())
                    rule.matches = TRY(parse_match_pattern_array(*matches));
                if (auto ext_ids = obj.get_array("extension_ids"sv); ext_ids.has_value())
                    rule.extension_ids = TRY(parse_string_array(*ext_ids));
                manifest.m_web_accessible_resources.append(move(rule));
            }
        }
    }

    // Content Security Policy
    manifest.m_content_security_policy = default_extension_csp(manifest.m_manifest_version);
    if (auto csp_val = root.get("content_security_policy"sv); csp_val.has_value()) {
        Optional<String> raw_csp;
        if (manifest.m_manifest_version == ManifestVersion::V2 && csp_val->is_string()) {
            raw_csp = csp_val->as_string();
        } else if (manifest.m_manifest_version == ManifestVersion::V3 && csp_val->is_object()) {
            if (auto ext_pages = csp_val->as_object().get_string("extension_pages"sv); ext_pages.has_value())
                raw_csp = *ext_pages;
        } else {
            manifest.m_has_invalid_csp = true;
        }

        if (raw_csp.has_value()) {
            if (is_valid_extension_csp(*raw_csp, manifest.m_manifest_version))
                manifest.m_content_security_policy = *raw_csp;
            else
                manifest.m_has_invalid_csp = true;
        }
    }

    return manifest;
}

bool ExtensionManifest::has_permission(StringView name) const
{
    return m_permissions.contains_slow(name);
}

bool ExtensionManifest::has_host_permission_for(URL::URL const& url) const
{
    for (auto const& pattern : m_host_permissions) {
        if (pattern.matches(url))
            return true;
    }
    return false;
}

bool ExtensionManifest::is_web_accessible(StringView relative_path, Optional<URL::URL> const& initiator_url, Optional<StringView> initiator_extension_id) const
{
    auto normalized_or_error = normalize_package_relative_path(relative_path);
    if (normalized_or_error.is_error())
        return false;
    auto const& normalized = normalized_or_error.value();

    for (auto const& rule : m_web_accessible_resources) {
        bool resource_matches = false;
        for (auto const& pattern : rule.resources) {
            if (MatchPattern::matches_glob(normalized, pattern)) {
                resource_matches = true;
                break;
            }
        }
        if (!resource_matches)
            continue;

        if (m_manifest_version == ManifestVersion::V2)
            return true;

        // MV3 requires either matching 'matches' for initiator_url or matching 'extension_ids'.
        if (initiator_extension_id.has_value() && (rule.extension_ids.contains_slow("*"sv) || rule.extension_ids.contains_slow(*initiator_extension_id)))
            return true;

        if (initiator_url.has_value()) {
            for (auto const& match : rule.matches) {
                if (match.matches(*initiator_url))
                    return true;
            }
        }
    }
    return false;
}

JsonObject ExtensionManifest::to_json() const
{
    auto parsed = JsonValue::from_string(m_raw_json);
    if (!parsed.is_error() && parsed.value().is_object())
        return parsed.value().as_object();

    JsonObject obj;
    obj.set("manifest_version"sv, to_underlying(m_manifest_version));
    obj.set("name"sv, m_name);
    obj.set("version"sv, m_version);
    return obj;
}

}
