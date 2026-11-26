/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/JsonArray.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebCommon/Extensions/SupportMatrix.h>

namespace Web::Extensions {

static constexpr Array s_supported_manifest_keys {
    "manifest_version"sv,
    "name"sv,
    "version"sv,
    "description"sv,
    "icons"sv,
    "author"sv,
    "homepage_url"sv,
    "default_locale"sv,
    "short_name"sv,
    "minimum_chrome_version"sv,
    "browser_specific_settings"sv,
    "applications"sv,
    "permissions"sv,
    "optional_permissions"sv,
    "host_permissions"sv,
    "optional_host_permissions"sv,
    "content_scripts"sv,
    "background"sv,
    "browser_action"sv,
    "action"sv,
    "sidebar_action"sv,
    "options_ui"sv,
    "options_page"sv,
    "web_accessible_resources"sv,
    "content_security_policy"sv,
};

static constexpr Array s_supported_permissions {
    "activeTab"sv,
    "scripting"sv,
    "storage"sv,
    "tabs"sv,
    "webRequest"sv,
    "webRequestBlocking"sv,
};

static constexpr Array s_supported_api_methods {
    "runtime.id"sv,
    "runtime.getURL"sv,
    "runtime.getManifest"sv,
    "runtime.sendMessage"sv,
    "runtime.onMessage"sv,
    "runtime.onInstalled"sv,
    "runtime.openOptionsPage"sv,
    "runtime.lastError"sv,
    "storage.local.get"sv,
    "storage.local.set"sv,
    "storage.local.remove"sv,
    "storage.local.clear"sv,
    "storage.local.getBytesInUse"sv,
    "storage.onChanged"sv,
    "tabs.query"sv,
    "tabs.get"sv,
    "tabs.create"sv,
    "tabs.update"sv,
    "tabs.remove"sv,
    "tabs.sendMessage"sv,
    "tabs.executeScript"sv,
    "tabs.insertCSS"sv,
    "tabs.removeCSS"sv,
    "tabs.onUpdated"sv,
    "tabs.onActivated"sv,
    "tabs.onCreated"sv,
    "tabs.onRemoved"sv,
    "windows.getCurrent"sv,
    "scripting.executeScript"sv,
    "scripting.insertCSS"sv,
    "scripting.removeCSS"sv,
    "action.setTitle"sv,
    "action.getTitle"sv,
    "action.setBadgeText"sv,
    "action.getBadgeText"sv,
    "action.setBadgeBackgroundColor"sv,
    "action.getBadgeBackgroundColor"sv,
    "action.setPopup"sv,
    "action.getPopup"sv,
    "action.setIcon"sv,
    "action.enable"sv,
    "action.disable"sv,
    "action.onClicked"sv,
    "browserAction.setTitle"sv,
    "browserAction.getTitle"sv,
    "browserAction.setBadgeText"sv,
    "browserAction.getBadgeText"sv,
    "browserAction.setBadgeBackgroundColor"sv,
    "browserAction.getBadgeBackgroundColor"sv,
    "browserAction.setPopup"sv,
    "browserAction.getPopup"sv,
    "browserAction.setIcon"sv,
    "browserAction.enable"sv,
    "browserAction.disable"sv,
    "browserAction.onClicked"sv,
    "sidebarAction.setPanel"sv,
    "sidebarAction.getPanel"sv,
    "sidebarAction.setTitle"sv,
    "sidebarAction.getTitle"sv,
    "sidebarAction.setIcon"sv,
    "sidebarAction.open"sv,
    "sidebarAction.close"sv,
    "sidebarAction.toggle"sv,
    "sidebarAction.isOpen"sv,
    "permissions.contains"sv,
    "permissions.getAll"sv,
    "permissions.remove"sv,
    "permissions.onAdded"sv,
    "permissions.onRemoved"sv,
    "webRequest.onBeforeRequest"sv,
};

// Each entry must have a verified package and a passing behavior test for the same bytes.
static constexpr Array s_tested_extension_versions {
    TestedExtensionVersion {
        .addon_id = "scrolltotop-lite@pratikabu"sv,
        .version = "1.0"sv,
        .sha256 = "3641b7d768842222e4b0c8dfc391504e53a5512a6b067406ba110bc3d7f9c97e"sv,
    },
};

ReadonlySpan<StringView> SupportMatrix::supported_manifest_keys()
{
    return s_supported_manifest_keys.span();
}

ReadonlySpan<StringView> SupportMatrix::supported_permissions()
{
    return s_supported_permissions.span();
}

ReadonlySpan<StringView> SupportMatrix::supported_api_methods()
{
    return s_supported_api_methods.span();
}

ReadonlySpan<TestedExtensionVersion> SupportMatrix::tested_extension_versions()
{
    return s_tested_extension_versions.span();
}

bool SupportMatrix::is_manifest_key_supported(StringView key)
{
    return s_supported_manifest_keys.span().contains_slow(key);
}

bool SupportMatrix::is_permission_supported(StringView permission)
{
    if (MatchPattern::is_valid_host_permission_pattern(permission))
        return true;
    return s_supported_permissions.span().contains_slow(permission);
}

static bool is_tested_exact_version(
    Optional<StringView> addon_id,
    Optional<StringView> version,
    Optional<StringView> sha256,
    ReadonlySpan<TestedExtensionVersion> extra_tested_versions = {})
{
    if (!addon_id.has_value() || !version.has_value() || !sha256.has_value())
        return false;
    for (auto const& entry : s_tested_extension_versions) {
        if (entry.addon_id == *addon_id && entry.version == *version && entry.sha256.equals_ignoring_ascii_case(*sha256))
            return true;
    }
    for (auto const& entry : extra_tested_versions) {
        if (entry.addon_id == *addon_id && entry.version == *version && entry.sha256.equals_ignoring_ascii_case(*sha256))
            return true;
    }
    return false;
}

CompatibilityEvaluation SupportMatrix::evaluate_manifest(
    ExtensionManifest const& manifest,
    Optional<StringView> sha256,
    ReadonlySpan<TestedExtensionVersion> extra_tested_versions)
{
    CompatibilityEvaluation evaluation;

    if (manifest.has_invalid_csp())
        evaluation.unsupported_reasons.append("Invalid or unsafe content_security_policy"_string);

    for (auto const& key : manifest.top_level_keys()) {
        if (!is_manifest_key_supported(key))
            evaluation.unsupported_reasons.append(MUST(String::formatted("Unsupported manifest key '{}'", key)));
    }

    auto const source = MUST(JsonValue::from_string(manifest.raw_json()));
    auto const& root = source.as_object();
    auto reject = [&](StringView field) {
        evaluation.unsupported_reasons.append(MUST(String::formatted("Unsupported manifest setting '{}'", field)));
    };

    if (auto const& bg = manifest.background(); bg.has_value()) {
        if (bg->service_worker.has_value() && bg->scripts.is_empty() && !bg->page.has_value())
            evaluation.unsupported_reasons.append("Background service_worker without scripts or page is not supported"_string);
    }
    if (auto background = root.get_object("background"sv); background.has_value()) {
        if (background->has("type"sv) && background->get_string("type"sv) != "classic"sv)
            reject("background.type"sv);
    }
    if (auto scripts = root.get_array("content_scripts"sv); scripts.has_value()) {
        for (auto const& script : scripts->values()) {
            auto const& options = script.as_object();
            if (options.has("match_about_blank"sv) && options.get_bool("match_about_blank"sv) != false)
                reject("content_scripts.match_about_blank"sv);
            if (options.has("match_origin_as_fallback"sv) && options.get_bool("match_origin_as_fallback"sv) != false)
                reject("content_scripts.match_origin_as_fallback"sv);
            if (options.has("world"sv) && options.get_string("world"sv) != "ISOLATED"sv)
                reject("content_scripts.world"sv);
            if (options.has("css_origin"sv)) {
                auto css_origin = options.get_string("css_origin"sv);
                if (!css_origin.has_value() || !css_origin->equals_ignoring_ascii_case("author"sv))
                    reject("content_scripts.css_origin"sv);
            }
        }
    }
    constexpr Array action_keys { "action"sv, "browser_action"sv };
    for (auto key : action_keys) {
        if (auto action = root.get_object(key); action.has_value() && action->has("theme_icons"sv))
            reject("action.theme_icons"sv);
    }

    for (auto const& perm : manifest.permissions()) {
        if (!is_permission_supported(perm))
            evaluation.unsupported_reasons.append(MUST(String::formatted("Unsupported permission '{}'", perm)));
    }

    if (manifest.has_permission("webRequestBlocking"sv) && !manifest.has_permission("webRequest"sv))
        evaluation.unsupported_reasons.append("webRequestBlocking requires webRequest permission"_string);

    if (!evaluation.unsupported_reasons.is_empty()) {
        evaluation.status = CompatibilityStatus::Incompatible;
        return evaluation;
    }

    auto addon_id = manifest.gecko_id().map([](auto const& s) { return s.bytes_as_string_view(); });
    if (is_tested_exact_version(addon_id, manifest.version().bytes_as_string_view(), sha256, extra_tested_versions))
        evaluation.status = CompatibilityStatus::TestedInLadyflock;
    else
        evaluation.status = CompatibilityStatus::ManifestCompatibleUntested;

    return evaluation;
}

CompatibilityEvaluation SupportMatrix::evaluate_catalog_metadata(
    Optional<StringView> addon_id,
    Optional<StringView> version,
    Optional<StringView> sha256,
    ReadonlySpan<String> permissions,
    bool has_service_worker_only)
{
    CompatibilityEvaluation evaluation;

    if (has_service_worker_only)
        evaluation.unsupported_reasons.append("Background service_worker without scripts or page is not supported"_string);

    for (auto const& perm : permissions) {
        if (!is_permission_supported(perm))
            evaluation.unsupported_reasons.append(MUST(String::formatted("Unsupported permission '{}'", perm)));
    }

    if (!evaluation.unsupported_reasons.is_empty()) {
        evaluation.status = CompatibilityStatus::Incompatible;
        return evaluation;
    }

    if (is_tested_exact_version(addon_id, version, sha256))
        evaluation.status = CompatibilityStatus::TestedInLadyflock;
    else
        evaluation.status = CompatibilityStatus::ManifestCompatibleUntested;

    return evaluation;
}

JsonObject SupportMatrix::to_json()
{
    JsonObject obj;
    JsonArray keys;
    for (auto key : s_supported_manifest_keys)
        keys.must_append(key);
    obj.set("manifest_keys"sv, move(keys));

    JsonArray permissions;
    for (auto perm : s_supported_permissions)
        permissions.must_append(perm);
    obj.set("permissions"sv, move(permissions));

    JsonArray methods;
    for (auto method : s_supported_api_methods)
        methods.must_append(method);
    obj.set("api_methods"sv, move(methods));

    JsonArray tested;
    for (auto const& entry : s_tested_extension_versions) {
        JsonObject item;
        item.set("addon_id"sv, entry.addon_id);
        item.set("version"sv, entry.version);
        item.set("sha256"sv, entry.sha256);
        tested.must_append(move(item));
    }
    obj.set("tested_versions"sv, move(tested));
    return obj;
}

}
