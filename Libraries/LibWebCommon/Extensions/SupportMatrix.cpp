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
    "commands"sv,
    "browser_action"sv,
    "action"sv,
    "sidebar_action"sv,
    "options_ui"sv,
    "options_page"sv,
    "web_accessible_resources"sv,
    "content_security_policy"sv,
    "declarative_net_request"sv,
    "incognito"sv,
    "developer"sv,
    "storage"sv,
};

// Manifest keys Firefox reads that Ladyflock does not implement. An extension that declares one is refused, since
// Firefox would act on it. Firefox ignores any other key it doesn't know, with a warning, and so does Ladyflock.
static constexpr Array s_firefox_unsupported_manifest_keys {
    "chrome_settings_overrides"sv,
    "chrome_url_overrides"sv,
    "devtools_page"sv,
    "dictionaries"sv,
    "experiment_apis"sv,
    "hidden"sv,
    "l10n_resources"sv,
    "omnibox"sv,
    "page_action"sv,
    "protocol_handlers"sv,
    "telemetry"sv,
    "theme"sv,
    "theme_experiment"sv,
    "user_scripts"sv,
};

// Permissions Firefox knows. Firefox ignores any other permission with a warning, such as one only Chrome has, and so
// does Ladyflock. One Firefox knows and Ladyflock does not implement refuses the install.
static constexpr Array s_firefox_permissions {
    "activeTab"sv,
    "alarms"sv,
    "bookmarks"sv,
    "browserSettings"sv,
    "browsingData"sv,
    "captivePortal"sv,
    "clipboardRead"sv,
    "clipboardWrite"sv,
    "contextMenus"sv,
    "contextualIdentities"sv,
    "cookies"sv,
    "declarativeNetRequest"sv,
    "declarativeNetRequestFeedback"sv,
    "declarativeNetRequestWithHostAccess"sv,
    "devtools"sv,
    "dns"sv,
    "downloads"sv,
    "downloads.open"sv,
    "find"sv,
    "geckoProfiler"sv,
    "geolocation"sv,
    "history"sv,
    "identity"sv,
    "idle"sv,
    "management"sv,
    "menus"sv,
    "menus.overrideContext"sv,
    "mozillaAddons"sv,
    "nativeMessaging"sv,
    "networkStatus"sv,
    "normandyAddonStudy"sv,
    "notifications"sv,
    "pkcs11"sv,
    "privacy"sv,
    "proxy"sv,
    "publicSuffix"sv,
    "scripting"sv,
    "search"sv,
    "sessions"sv,
    "storage"sv,
    "tabGroups"sv,
    "tabHide"sv,
    "tabs"sv,
    "telemetry"sv,
    "theme"sv,
    "topSites"sv,
    "trialML"sv,
    "unlimitedStorage"sv,
    "urlbar"sv,
    "userScripts"sv,
    "webNavigation"sv,
    "webRequest"sv,
    "webRequestAuthProvider"sv,
    "webRequestBlocking"sv,
    "webRequestFilterResponse"sv,
    "webRequestFilterResponse.serviceWorkerScript"sv,
};

static constexpr Array s_supported_permissions {
    "activeTab"sv,
    "alarms"sv,
    "clipboardRead"sv,
    "clipboardWrite"sv,
    "contextMenus"sv,
    "cookies"sv,
    "declarativeNetRequest"sv,
    "declarativeNetRequestWithHostAccess"sv,
    "dns"sv,
    "downloads"sv,
    "idle"sv,
    "management"sv,
    "menus"sv,
    "nativeMessaging"sv,
    "notifications"sv,
    "privacy"sv,
    "publicSuffix"sv,
    "scripting"sv,
    "storage"sv,
    "tabs"sv,
    "unlimitedStorage"sv,
    "webNavigation"sv,
    "webRequest"sv,
    "webRequestBlocking"sv,
};

static constexpr Array s_supported_api_methods {
    "runtime.id"sv,
    "runtime.getURL"sv,
    "runtime.getManifest"sv,
    "runtime.sendMessage"sv,
    "runtime.onMessage"sv,
    "runtime.connect"sv,
    "runtime.onConnect"sv,
    "runtime.Port.postMessage"sv,
    "runtime.Port.disconnect"sv,
    "runtime.Port.onMessage"sv,
    "runtime.Port.onDisconnect"sv,
    "runtime.onInstalled"sv,
    "runtime.openOptionsPage"sv,
    "runtime.lastError"sv,
    "alarms.create"sv,
    "alarms.get"sv,
    "alarms.getAll"sv,
    "alarms.clear"sv,
    "alarms.clearAll"sv,
    "alarms.onAlarm"sv,
    "commands.getAll"sv,
    "commands.onCommand"sv,
    "i18n.getMessage"sv,
    "i18n.getUILanguage"sv,
    "dns.resolve"sv,
    "declarativeNetRequest.getEnabledRulesets"sv,
    "declarativeNetRequest.updateEnabledRulesets"sv,
    "declarativeNetRequest.getAvailableStaticRuleCount"sv,
    "declarativeNetRequest.getDynamicRules"sv,
    "declarativeNetRequest.updateDynamicRules"sv,
    "declarativeNetRequest.getSessionRules"sv,
    "declarativeNetRequest.updateSessionRules"sv,
    "declarativeNetRequest.isRegexSupported"sv,
    "menus.create"sv,
    "menus.update"sv,
    "menus.remove"sv,
    "menus.removeAll"sv,
    "menus.onClicked"sv,
    "privacy.network.networkPredictionEnabled.get"sv,
    "privacy.network.networkPredictionEnabled.set"sv,
    "privacy.network.networkPredictionEnabled.clear"sv,
    "privacy.network.networkPredictionEnabled.onChange"sv,
    "privacy.network.globalPrivacyControl.get"sv,
    "privacy.network.globalPrivacyControl.onChange"sv,
    "privacy.websites.hyperlinkAuditingEnabled.get"sv,
    "webNavigation.getFrame"sv,
    "webNavigation.getAllFrames"sv,
    "webNavigation.onBeforeNavigate"sv,
    "webNavigation.onCommitted"sv,
    "webNavigation.onDOMContentLoaded"sv,
    "webNavigation.onCompleted"sv,
    "webNavigation.onCreatedNavigationTarget"sv,
    "webNavigation.onHistoryStateUpdated"sv,
    "webNavigation.onReferenceFragmentUpdated"sv,
    "webNavigation.onErrorOccurred"sv,
    "storage.local.get"sv,
    "storage.local.set"sv,
    "storage.local.remove"sv,
    "storage.local.clear"sv,
    "storage.local.getBytesInUse"sv,
    "storage.local.getKeys"sv,
    "storage.session.get"sv,
    "storage.session.set"sv,
    "storage.session.remove"sv,
    "storage.session.clear"sv,
    "storage.session.getBytesInUse"sv,
    "storage.session.getKeys"sv,
    "storage.session.setAccessLevel"sv,
    "storage.onChanged"sv,
    "tabs.query"sv,
    "tabs.get"sv,
    "tabs.create"sv,
    "tabs.update"sv,
    "tabs.reload"sv,
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
    "scripting.registerContentScripts"sv,
    "scripting.getRegisteredContentScripts"sv,
    "scripting.updateContentScripts"sv,
    "scripting.unregisterContentScripts"sv,
    "contentScripts.register"sv,
    "contentScripts.RegisteredContentScript.unregister"sv,
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
    "publicSuffix.isKnownSuffix"sv,
    "publicSuffix.getKnownSuffix"sv,
    "publicSuffix.getDomain"sv,
    "webRequest.onBeforeRequest"sv,
    "webRequest.onSendHeaders"sv,
    "webRequest.onHeadersReceived"sv,
    "webRequest.onBeforeRedirect"sv,
    "webRequest.onResponseStarted"sv,
    "webRequest.onCompleted"sv,
    "webRequest.onErrorOccurred"sv,
    "webRequest.handlerBehaviorChanged"sv,
    "runtime.onUpdateAvailable"sv,
    "runtime.getPlatformInfo"sv,
    "runtime.getBrowserInfo"sv,
    "runtime.onStartup"sv,
    "runtime.onSuspend"sv,
    "runtime.onMessageExternal"sv,
    "runtime.onConnectExternal"sv,
    "runtime.setUninstallURL"sv,
    "runtime.reload"sv,
    "extension.getURL"sv,
    "extension.inIncognitoContext"sv,
    "extension.isAllowedIncognitoAccess"sv,
    "extension.isAllowedFileSchemeAccess"sv,
    "i18n.getMessage (content scripts)"sv,
    "storage.sync.get"sv,
    "storage.sync.set"sv,
    "storage.sync.remove"sv,
    "storage.sync.clear"sv,
    "storage.sync.getBytesInUse"sv,
    "storage.sync.getKeys"sv,
    "storage.managed.get"sv,
    "storage.local.onChanged"sv,
    "storage.sync.onChanged"sv,
    "storage.session.onChanged"sv,
    "tabs.getCurrent"sv,
    "tabs.onReplaced"sv,
    "windows.get"sv,
    "windows.getAll"sv,
    "windows.getLastFocused"sv,
    "windows.onFocusChanged"sv,
    "management.getSelf"sv,
    "management.uninstallSelf"sv,
    "management.getAll"sv,
    "management.get"sv,
    "management.onInstalled"sv,
    "management.onUninstalled"sv,
    "management.onEnabled"sv,
    "management.onDisabled"sv,
    "permissions.request"sv,
    "runtime.connectNative"sv,
    "runtime.sendNativeMessage"sv,
    "notifications.create"sv,
    "notifications.clear"sv,
    "notifications.getAll"sv,
    "notifications.onShown"sv,
    "notifications.onClicked"sv,
    "notifications.onClosed"sv,
    "notifications.onButtonClicked"sv,
    "idle.queryState"sv,
    "idle.setDetectionInterval"sv,
    "idle.onStateChanged"sv,
    "cookies.get"sv,
    "cookies.getAll"sv,
    "cookies.set"sv,
    "cookies.remove"sv,
    "cookies.getAllCookieStores"sv,
    "cookies.onChanged"sv,
    "downloads.download"sv,
    "downloads.search"sv,
    "downloads.cancel"sv,
    "downloads.pause"sv,
    "downloads.resume"sv,
    "downloads.onCreated"sv,
    "downloads.onChanged"sv,
    "downloads.onErased"sv,
    "navigator.clipboard (clipboardWrite, clipboardRead)"sv,
};

// Each entry must have a verified package and a passing behavior test for the same bytes.
static constexpr Array s_tested_extension_versions {
    TestedExtensionVersion {
        .addon_id = "scrolltotop-lite@pratikabu"sv,
        .version = "1.0"sv,
        .sha256 = "3641b7d768842222e4b0c8dfc391504e53a5512a6b067406ba110bc3d7f9c97e"sv,
    },
    TestedExtensionVersion {
        .addon_id = "uBlock0@raymondhill.net"sv,
        .version = "1.77.2"sv,
        .sha256 = "c9186bcbb5ef542465187ce52aa7d76b5315e8a020c360e3e701f678ad47d776"sv,
    },
    TestedExtensionVersion {
        .addon_id = "uBOLiteRedux@raymondhill.net"sv,
        .version = "2026.1124.1811"sv,
        .sha256 = "9f714dd6480de7be085de094693478a9143cac1fa24af30b76c999fe9d031bac"sv,
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

bool SupportMatrix::is_permission_known_to_firefox(StringView permission)
{
    return s_firefox_permissions.span().contains_slow(permission);
}

bool SupportMatrix::is_manifest_key_known_unsupported(StringView key)
{
    return s_firefox_unsupported_manifest_keys.span().contains_slow(key);
}

bool SupportMatrix::is_permission_supported(StringView permission)
{
    if (MatchPattern::is_valid_host_permission_pattern(permission))
        return true;
    return s_supported_permissions.span().contains_slow(permission);
}

static void check_permission(CompatibilityEvaluation& evaluation, StringView permission)
{
    if (SupportMatrix::is_permission_supported(permission))
        return;
    if (SupportMatrix::is_permission_known_to_firefox(permission))
        evaluation.unsupported_reasons.append(MUST(String::formatted("Unsupported permission '{}'", permission)));
    else
        evaluation.warnings.append(MUST(String::formatted("Ignored permission '{}', which Firefox does not know", permission)));
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
        if (is_manifest_key_supported(key))
            continue;
        if (is_manifest_key_known_unsupported(key))
            evaluation.unsupported_reasons.append(MUST(String::formatted("Unsupported manifest key '{}'", key)));
        else
            evaluation.warnings.append(MUST(String::formatted("Ignored manifest key '{}', which Firefox does not read", key)));
    }

    auto const source = MUST(JsonValue::from_string(manifest.raw_json()));
    auto const& root = source.as_object();
    if (root.has("declarative_net_request"sv) && !uses_declarative_net_request(manifest))
        evaluation.unsupported_reasons.append("Declarative rules require declarativeNetRequest permission"_string);
    auto reject = [&](StringView field) {
        evaluation.unsupported_reasons.append(MUST(String::formatted("Unsupported manifest setting '{}'", field)));
    };

    if (auto const& bg = manifest.background(); bg.has_value()) {
        if (bg->service_worker.has_value() && bg->scripts.is_empty() && !bg->page.has_value())
            evaluation.unsupported_reasons.append("Background service_worker without scripts or page is not supported"_string);
    }
    if (auto background = root.get_object("background"sv); background.has_value()) {
        if (background->has("type"sv)) {
            auto type = background->get_string("type"sv);
            if (type != "classic"sv && (type != "module"sv || !manifest.background().has_value() || manifest.background()->scripts.is_empty() || manifest.background()->page.has_value()))
                reject("background.type"sv);
        }
    }
    if (root.has("commands"sv)) {
        if (auto commands = root.get_object("commands"sv); commands.has_value()) {
            commands->for_each_member([&](String const& name, JsonValue const& options) {
                if (name.is_empty() || !options.is_object()) {
                    reject("commands"sv);
                    return;
                }
            });
        } else {
            reject("commands"sv);
        }
    }
    if (auto scripts = root.get_array("content_scripts"sv); scripts.has_value()) {
        for (auto const& script : scripts->values()) {
            auto const& options = script.as_object();
            if (options.has("match_origin_as_fallback"sv) && options.get_bool("match_origin_as_fallback"sv) != false)
                reject("content_scripts.match_origin_as_fallback"sv);
            if (options.has("css_origin"sv)) {
                auto css_origin = options.get_string("css_origin"sv);
                if (!css_origin.has_value() || !css_origin->equals_ignoring_ascii_case("author"sv))
                    reject("content_scripts.css_origin"sv);
            }
        }
    }
    for (auto const& perm : manifest.permissions())
        check_permission(evaluation, perm);
    for (auto const& pattern : manifest.host_permissions()) {
        if (!pattern.is_web_host_permission())
            evaluation.unsupported_reasons.append(MUST(String::formatted("Unsupported host permission '{}'", pattern.raw())));
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

    for (auto const& perm : permissions)
        check_permission(evaluation, perm);

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
