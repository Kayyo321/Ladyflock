/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestControlClient.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/BrowsingContext.h>
#include <LibWebView/Application.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <LibWebView/Extensions/MozillaAddonTrust.h>
#include <LibWebView/Extensions/NativeMessaging.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/NotificationManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/Settings.h>
#include <LibWebView/SystemIdle.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <time.h>

namespace WebView::Extensions {

static HashTable<ViewImplementation const*>& background_views()
{
    static NeverDestroyed<HashTable<ViewImplementation const*>> s_views;
    return *s_views;
}

static String sha256_prefix_hex(ReadonlyBytes data)
{
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, data);
    StringBuilder builder;
    auto bytes = digest.bytes();
    for (size_t i = 0; i < min<size_t>(8, bytes.size()); ++i)
        builder.appendff("{:02x}", bytes[i]);
    return MUST(builder.to_string());
}

static String mime_type_for_path(StringView path)
{
    if (path.ends_with(".html"sv, CaseSensitivity::CaseInsensitive) || path.ends_with(".htm"sv, CaseSensitivity::CaseInsensitive))
        return "text/html; charset=utf-8"_string;
    if (path.ends_with(".js"sv, CaseSensitivity::CaseInsensitive) || path.ends_with(".mjs"sv, CaseSensitivity::CaseInsensitive))
        return "application/javascript; charset=utf-8"_string;
    if (path.ends_with(".css"sv, CaseSensitivity::CaseInsensitive))
        return "text/css; charset=utf-8"_string;
    if (path.ends_with(".json"sv, CaseSensitivity::CaseInsensitive))
        return "application/json; charset=utf-8"_string;
    if (path.ends_with(".svg"sv, CaseSensitivity::CaseInsensitive))
        return "image/svg+xml"_string;
    if (path.ends_with(".png"sv, CaseSensitivity::CaseInsensitive))
        return "image/png"_string;
    if (path.ends_with(".jpg"sv, CaseSensitivity::CaseInsensitive) || path.ends_with(".jpeg"sv, CaseSensitivity::CaseInsensitive))
        return "image/jpeg"_string;
    if (path.ends_with(".woff2"sv, CaseSensitivity::CaseInsensitive))
        return "font/woff2"_string;
    return "application/octet-stream"_string;
}

static ErrorOr<String> read_utf8_file_bounded(ByteString const& path)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    auto bytes = TRY(file->read_until_eof());
    return String::from_utf8(StringView { bytes.bytes() });
}

static ErrorOr<ByteBuffer> read_package_resource(ByteString const& directory, StringView relative_path, size_t limit)
{
    auto root = TRY(FileSystem::real_path(directory));
    auto path = LexicalPath::join(root, relative_path).string();
#if defined(AK_OS_WINDOWS)
    if (TRY(FileSystem::real_path(path)) != path)
        return Error::from_string_literal("Symlink in extension resource path");
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read | Core::File::OpenMode::NoFollow));
#else
    auto file = TRY(Core::File::open_regular_file_or_directory(path));
    if (!S_ISREG(TRY(file->stat()).st_mode))
        return Error::from_string_literal("Extension resource must be a regular file");
#endif
    ByteBuffer data;
    Array<u8, 8192> buffer;
    while (!file->is_eof()) {
        auto chunk = TRY(file->read_some(buffer.span()));
        if (chunk.size() > limit - data.size())
            return Error::from_string_literal("Extension resource exceeds its size limit");
        TRY(data.try_append(chunk));
    }
    return data;
}

static ErrorOr<String> read_package_script(ByteString const& directory, StringView relative_path)
{
    auto data = TRY(read_package_resource(directory, relative_path, 16 * MiB));
    return String::from_utf8(StringView { data.bytes() });
}

static ErrorOr<RegisteredExtensionContentScript> parse_registered_content_script(JsonValue const& value, ByteString const& package, Optional<RegisteredExtensionContentScript::Owner> owner = {})
{
    if (!value.is_object())
        return Error::from_string_literal("Expected a content script descriptor");
    auto const& descriptor = value.as_object();
    auto id = descriptor.get_string("id"sv);
    if (!id.has_value() || id->is_empty() || id->byte_count() > 256 || id->starts_with('_'))
        return Error::from_string_literal("Invalid content script identifier");

    auto matches = descriptor.get_array("matches"sv);
    if (!matches.has_value() || matches->is_empty())
        return Error::from_string_literal("Content scripts require at least one match pattern");

    RegisteredExtensionContentScript script { *id, {}, descriptor };
    auto parse_patterns = [&](StringView key, Vector<Web::Extensions::MatchPattern>& output) -> ErrorOr<void> {
        if (!descriptor.has(key))
            return {};
        auto patterns = descriptor.get_array(key);
        if (!patterns.has_value() || patterns->size() > 4096)
            return Error::from_string_literal("Invalid content script match patterns");
        for (auto const& pattern : patterns->values()) {
            if (!pattern.is_string())
                return Error::from_string_literal("Content script match patterns must be strings");
            output.append(TRY(Web::Extensions::MatchPattern::parse(pattern.as_string())));
        }
        return {};
    };
    TRY(parse_patterns("matches"sv, script.rule.matches));
    TRY(parse_patterns("excludeMatches"sv, script.rule.exclude_matches));

    size_t legacy_source_bytes = 0;
    auto parse_sources = [&](StringView key, Vector<String>& output, Vector<Web::Extensions::NamedSource>& legacy_sources) -> ErrorOr<void> {
        if (!descriptor.has(key))
            return {};
        auto files = descriptor.get_array(key);
        if (!files.has_value() || files->size() > 128)
            return Error::from_string_literal("Invalid content script file list");
        for (auto const& file : files->values()) {
            if (owner.has_value()) {
                if (!file.is_object())
                    return Error::from_string_literal("Expected an object containing a content script file or source");
                auto const& source = file.as_object();
                if (source.has("file"sv) == source.has("code"sv))
                    return Error::from_string_literal("Specify one file or one source string");
                if (auto filename = source.get_string("file"sv); filename.has_value()) {
                    auto path = TRY(Web::Extensions::ExtensionManifest::normalize_package_relative_path(*filename));
                    auto content = TRY(read_package_script(package, path));
                    legacy_sources.append({ move(path), move(content) });
                } else if (auto code = source.get_string("code"sv); code.has_value()) {
                    auto name = TRY(String::formatted("registered-content-script-{}/{}-{}", *id, key, legacy_sources.size()));
                    legacy_sources.append({ move(name), *code });
                } else {
                    return Error::from_string_literal("Expected a string for the content script file or source");
                }
                legacy_source_bytes += legacy_sources.last().content.byte_count();
                if (legacy_source_bytes > 2 * MiB)
                    return Error::from_string_literal("Content script sources exceed their size limit");
                continue;
            }
            if (!file.is_string())
                return Error::from_string_literal("Content script file names must be strings");
            auto path = TRY(Web::Extensions::ExtensionManifest::normalize_package_relative_path(file.as_string()));
            if (read_package_script(package, path).is_error())
                return Error::from_string_literal("Content script file is absent or unreadable in its extension package");
            output.append(move(path));
        }
        return {};
    };
    TRY(parse_sources("js"sv, script.rule.js, script.legacy_js));
    TRY(parse_sources("css"sv, script.rule.css, script.legacy_css));
    if (script.rule.js.is_empty() && script.rule.css.is_empty() && script.legacy_js.is_empty() && script.legacy_css.is_empty())
        return Error::from_string_literal("Content script requires JavaScript or CSS files");

    auto read_bool = [&](StringView key, bool fallback) -> ErrorOr<bool> {
        auto bool_value = descriptor.get_bool(key);
        if (descriptor.has(key) && !bool_value.has_value())
            return Error::from_string_literal("Expected a boolean content script option");
        return bool_value.value_or(fallback);
    };
    script.rule.all_frames = TRY(read_bool("allFrames"sv, false));
    auto match_origin_as_fallback = TRY(read_bool("matchOriginAsFallback"sv, false));
    script.rule.match_about_blank = match_origin_as_fallback || (owner.has_value() && TRY(read_bool("matchAboutBlank"sv, false)));
    if (match_origin_as_fallback && any_of(script.rule.matches, [](auto const& pattern) {
            auto raw = pattern.raw().bytes_as_string_view();
            if (raw == "<all_urls>"sv)
                return false;
            auto path_offset = raw.find('/', raw.find("://"sv).value() + 3);
            return !path_offset.has_value() || raw.substring_view(*path_offset) != "/*"sv;
        }))
        return Error::from_string_literal("Origin fallback requires wildcard paths");
    script.persist_across_sessions = !owner.has_value() && TRY(read_bool("persistAcrossSessions"sv, true));
    script.legacy_owner = move(owner);
    if (script.legacy_owner.has_value()) {
        auto parse_globs = [&](StringView key, Vector<String>& output) -> ErrorOr<void> {
            if (!descriptor.has(key))
                return {};
            auto globs = descriptor.get_array(key);
            if (!globs.has_value() || globs->size() > 4096)
                return Error::from_string_literal("Invalid content script glob list");
            for (auto const& glob : globs->values()) {
                if (!glob.is_string())
                    return Error::from_string_literal("Expected a content script glob string");
                output.append(glob.as_string());
            }
            return {};
        };
        TRY(parse_globs("includeGlobs"sv, script.rule.include_globs));
        TRY(parse_globs("excludeGlobs"sv, script.rule.exclude_globs));
        if (descriptor.has("cookieStoreId"sv))
            return Error::from_string_literal("Registration in a specific cookie store is unsupported");
    }

    if (descriptor.has("runAt"sv)) {
        auto run_at = Web::Extensions::run_at_from_string(descriptor.get_string("runAt"sv).value_or({}));
        if (!run_at.has_value())
            return Error::from_string_literal("Unknown content script runAt value");
        script.rule.run_at = *run_at;
    }
    auto world = descriptor.has("world"sv) ? descriptor.get_string("world"sv).value_or({}) : "ISOLATED"_string;
    if (world != "ISOLATED"sv && world != "MAIN"sv)
        return Error::from_string_literal("Unknown content script world");
    script.run_in_main_world = world == "MAIN"sv;
    if ((!script.rule.css.is_empty() || !script.legacy_css.is_empty()) && descriptor.get_string("cssOrigin"sv) != "user"sv)
        return Error::from_string_literal("Registered style sheets currently require cssOrigin 'user'");
    return script;
}

ErrorOr<JsonValue> ExtensionManager::handle_registered_content_scripts(InstalledExtension& extension, StringView method, JsonArray const& arguments)
{
    auto& scripts = extension.registered_content_scripts;
    if (method == "getRegisteredContentScripts"sv || method == "unregisterContentScripts"sv) {
        HashTable<String> ids;
        bool has_filter = false;
        if (!arguments.is_empty()) {
            if (arguments.size() != 1 || !arguments[0].is_object())
                return Error::from_string_literal("Expected a registered content script filter object");
            auto const& filter = arguments[0].as_object();
            has_filter = filter.has("ids"sv);
            if (has_filter) {
                auto id_list = filter.get_array("ids"sv);
                if (!id_list.has_value() || id_list->size() > 100)
                    return Error::from_string_literal("Expected an array of registered content script IDs");
                for (auto const& id : id_list->values()) {
                    if (!id.is_string())
                        return Error::from_string_literal("Registered content script IDs must be strings");
                    ids.set(id.as_string());
                }
            }
        }
        auto matches = [&](auto const& script) { return !script.legacy_owner.has_value() && (!has_filter || ids.contains(script.id)); };
        if (method == "getRegisteredContentScripts"sv) {
            JsonArray result;
            for (auto const& script : scripts) {
                if (matches(script))
                    result.must_append(script.descriptor);
            }
            return JsonValue(move(result));
        }
        for (auto const& id : ids) {
            if (!any_of(scripts, [&](auto const& script) { return !script.legacy_owner.has_value() && script.id == id; }))
                return Error::from_string_literal("No registered content script has the specified ID");
        }

        auto previous = scripts;
        scripts.remove_all_matching(matches);
        if (auto saved = save_registry(); saved.is_error()) {
            scripts = move(previous);
            return saved.release_error();
        }
        return JsonValue {};
    }

    if (arguments.size() != 1 || !arguments[0].is_array() || arguments[0].as_array().size() > 100)
        return Error::from_string_literal("Expected a list of registered content scripts");
    bool update = method == "updateContentScripts"sv;
    auto next = scripts;
    HashTable<String> changed_ids;
    for (auto const& entry : arguments[0].as_array().values()) {
        if (!entry.is_object())
            return Error::from_string_literal("Expected a registered content script descriptor");
        auto id = entry.as_object().get_string("id"sv);
        if (!id.has_value() || changed_ids.set(*id) == AK::HashSetResult::ReplacedExistingEntry)
            return Error::from_string_literal("Expected a unique registered content script ID");
        auto existing = next.find_if([&](auto const& script) { return script.id == *id; });
        if (existing != next.end() && existing->legacy_owner.has_value())
            return Error::from_string_literal("A registered content script already uses this ID");
        if (update && existing == next.end())
            return Error::from_string_literal("No registered content script has the specified ID");
        if (!update && existing != next.end())
            return Error::from_string_literal("A registered content script already uses this ID");
        if (update) {
            auto merged = existing->descriptor;
            entry.as_object().for_each_member([&](auto const& key, auto const& value) { merged.set(key, value); });
            *existing = TRY(parse_registered_content_script(merged, extension.install_directory));
        } else {
            if (next.size() >= 100)
                return Error::from_string_literal("Too many registered content scripts");
            next.append(TRY(parse_registered_content_script(entry, extension.install_directory)));
        }
    }

    size_t descriptor_bytes = 0;
    for (auto const& script : next)
        descriptor_bytes += script.descriptor.serialized().byte_count();
    if (descriptor_bytes > 512 * KiB)
        return Error::from_string_literal("Registered content scripts exceed their storage limit");

    auto previous = move(scripts);
    scripts = move(next);
    if (auto saved = save_registry(); saved.is_error()) {
        scripts = move(previous);
        return saved.release_error();
    }
    return JsonValue {};
}

static ErrorOr<Vector<DeclarativeRuleset>> read_declarative_rulesets(Web::Extensions::ExtensionManifest const& manifest, ByteString const& directory)
{
    return load_declarative_rulesets(manifest, [&](StringView path) { return read_package_script(directory, path); });
}

ErrorOr<ByteBuffer> ExtensionManager::read_package_icon(ByteString const& directory, StringView path)
{
    auto normalized = TRY(Web::Extensions::ExtensionManifest::normalize_package_relative_path(path));
    return read_package_resource(directory, normalized, 1 * MiB);
}

String ExtensionManager::locale_messages_for(InstalledExtension const& extension) const
{
    JsonObject messages;
    auto merge_locale = [&](StringView locale) {
        if (locale.is_empty() || locale.length() > 64)
            return;
        StringBuilder folder;
        for (auto byte : locale) {
            if (!is_ascii_alphanumeric(byte) && byte != '_' && byte != '-')
                return;
            folder.append(byte == '-' ? '_' : byte);
        }
        auto path = MUST(String::formatted("_locales/{}/messages.json", folder.string_view()));
        auto bytes = read_package_resource(extension.install_directory, path, 512 * KiB);
        if (bytes.is_error())
            return;
        auto json = JsonValue::from_string(StringView { bytes.value().bytes() });
        if (json.is_error() || !json.value().is_object())
            return;
        json.value().as_object().for_each_member([&](String const& key, JsonValue const& value) {
            if (value.is_object() && value.as_object().get_string("message"sv).has_value() && !key.starts_with_bytes("@@"sv))
                messages.set(key.to_ascii_lowercase(), value);
        });
    };

    auto const& language = Application::settings().languages().first();
    auto locale = language.bytes_as_string_view();
    auto primary = locale.find_first_split_view('-');
    auto manifest = MUST(JsonValue::from_string(extension.manifest.raw_json()));
    if (auto default_locale = manifest.as_object().get_string("default_locale"sv); default_locale.has_value()) {
        merge_locale(*default_locale);
        if (primary != *default_locale)
            merge_locale(primary);
        if (locale != primary && locale != *default_locale)
            merge_locale(locale);
    }

    auto add_builtin = [&](StringView key, StringView value) {
        JsonObject message;
        message.set("message"sv, value);
        messages.set(key, move(message));
    };
    auto ui_locale = MUST(language.replace("-"sv, "_"sv, ReplaceMode::All));
    bool rtl = primary.is_one_of("ar"sv, "ckb"sv, "fa"sv, "he"sv, "ps"sv, "ur"sv, "yi"sv);
    add_builtin("@@ui_locale"sv, ui_locale);
    add_builtin("@@bidi_dir"sv, rtl ? "rtl"sv : "ltr"sv);
    add_builtin("@@bidi_reversed_dir"sv, rtl ? "ltr"sv : "rtl"sv);
    add_builtin("@@bidi_start_edge"sv, rtl ? "right"sv : "left"sv);
    add_builtin("@@bidi_end_edge"sv, rtl ? "left"sv : "right"sv);
    add_builtin("@@extension_id"sv, extension.uuid);
    return messages.serialized();
}

// Tells the extensions with the downloads permission what became of the profile's downloads.
class ExtensionManager::DownloadEvents final : public FileDownloaderObserver {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit DownloadEvents(ExtensionManager& manager)
        : m_manager(manager)
    {
    }

private:
    virtual void download_added(FileDownloader::Download const& download) override
    {
        if (download.is_private != IsPrivate::No || !download.identity_id.is_empty())
            return;
        auto item = m_manager.serialize_extension_download(download);
        m_snapshots.set(download.id, item);
        JsonArray arguments;
        arguments.must_append(move(item));
        m_manager.dispatch_to_extensions_with_permission("downloads"sv, "downloads"sv, "onCreated"sv, arguments.serialized());
    }

    virtual void download_updated(FileDownloader::Download const& download) override
    {
        if (download.is_private != IsPrivate::No || !download.identity_id.is_empty())
            return;
        auto item = m_manager.serialize_extension_download(download);
        auto previous = m_snapshots.get(download.id).value_or({});
        // A delta names each changed field with its previous and current value, as in Firefox.
        JsonObject delta;
        delta.set("id"sv, download.id);
        for (auto key : { "state"sv, "paused"sv, "canResume"sv, "error"sv, "totalBytes"sv, "fileSize"sv, "exists"sv, "filename"sv, "url"sv }) {
            auto current = item.get(key);
            auto before = previous.get(key);
            if ((current.has_value() ? current->serialized() : String {}) == (before.has_value() ? before->serialized() : String {}))
                continue;
            JsonObject change;
            if (before.has_value())
                change.set("previous"sv, *before);
            if (current.has_value())
                change.set("current"sv, *current);
            delta.set(key, move(change));
        }
        m_snapshots.set(download.id, move(item));
        if (delta.size() == 1)
            return;
        JsonArray arguments;
        arguments.must_append(move(delta));
        m_manager.dispatch_to_extensions_with_permission("downloads"sv, "downloads"sv, "onChanged"sv, arguments.serialized());
    }

    virtual void download_removed(u64 id, IsPrivate is_private) override
    {
        if (is_private != IsPrivate::No || !m_snapshots.remove(id))
            return;
        m_manager.m_extension_downloads.remove(id);
        JsonArray arguments;
        arguments.must_append(id);
        m_manager.dispatch_to_extensions_with_permission("downloads"sv, "downloads"sv, "onErased"sv, arguments.serialized());
    }

    ExtensionManager& m_manager;
    HashMap<u64, JsonObject> m_snapshots;
};

ErrorOr<NonnullOwnPtr<ExtensionManager>> ExtensionManager::create(ByteString profile_data_root, Database::Database* shared_database)
{
    NonnullOwnPtr<ExtensionStorage> storage = shared_database
        ? TRY(ExtensionStorage::create(*shared_database))
        : ExtensionStorage::create();
    NonnullOwnPtr<ExtensionStorage> sync_storage = shared_database
        ? TRY(ExtensionStorage::create(*shared_database, ExtensionStorage::Area::Sync))
        : ExtensionStorage::create();
    return adopt_own(*new ExtensionManager(move(profile_data_root), move(storage), move(sync_storage)));
}

ExtensionManager::ExtensionManager(ByteString profile_data_root, NonnullOwnPtr<ExtensionStorage> storage, NonnullOwnPtr<ExtensionStorage> sync_storage)
    : m_profile_data_root(move(profile_data_root))
    , m_extensions_root(LexicalPath::join(m_profile_data_root, "extensions"sv).string())
    , m_storage(move(storage))
    , m_sync_storage(move(sync_storage))
    , m_alarms([this](String const& extension_id, JsonObject alarm) {
        JsonArray arguments;
        arguments.must_append(move(alarm));
        dispatch_event_to_extension(extension_id, "alarms"sv, "onAlarm"sv, arguments.serialized());
    })
    , m_installer(m_extensions_root, m_revocation_list)
{
    m_installer.add_trusted_root_pem(MUST(String::from_utf8(mozilla_addon_signing_root)));
    m_web_request_coordinator.set_on_snapshot_changed([this](Web::Extensions::WebRequestWatchSnapshot const& snapshot) {
        push_watch_snapshot_everywhere(snapshot);
    });
    m_web_request_coordinator.set_permission_check_callback([this](StringView ext_id, URL::URL const& url) {
        if (url.scheme() != "moz-extension"sv)
            return has_host_permission(ext_id, url);
        auto owner = extension_by_uuid(url.serialized_host());
        return m_extensions_enabled && owner.has_value() && owner->enabled && owner->extension_id == ext_id;
    });
    m_web_request_coordinator.set_redirect_check_callback([this](StringView extension_id, URL::URL const& url) {
        if (url.includes_credentials())
            return false;
        if (url.scheme().is_one_of("http"sv, "https"sv))
            return has_host_permission(extension_id, url);
        auto extension = extension_by_id(extension_id);
        if (url.scheme() != "moz-extension"sv || !extension.has_value() || !extension->enabled || extension->uuid != url.serialized_host())
            return false;
        auto normalized = Web::Extensions::ExtensionManifest::normalize_package_relative_path(url.serialize_path());
        return !normalized.is_error() && extension->manifest.is_web_accessible(normalized.value(), {}, {});
    });
    m_web_request_coordinator.set_declarative_decision_callback([this](auto const& request, auto const& initiator) -> Web::Extensions::WebRequestDecision {
        using Action = DeclarativeRequestRule::Action;
        using Event = Web::Extensions::WebRequestEvent;
        using ResourceType = Web::Extensions::WebRequestResourceType;
        auto host = request.url.serialized_host();
        auto restricted_host = [](StringView host) {
            if (host.ends_with('.'))
                host = host.substring_view(0, host.length() - 1);
            return host.is_one_of("addons.mozilla.org"sv, "addons.cdn.mozilla.net"sv);
        };
        if (!m_extensions_enabled || restricted_host(host) || (initiator.has_value() && restricted_host(initiator->serialized_host())))
            return {};

        Web::Extensions::WebRequestDecision decision;
        HashTable<String> modified_headers;
        auto is_navigation = request.type == ResourceType::MainFrame || request.type == ResourceType::SubFrame;
        for (auto const& [_, extension] : m_extensions_by_id) {
            if (!extension.enabled || (extension.declarative_rulesets.is_empty() && extension.dynamic_declarative_rules.rules.is_empty() && extension.session_declarative_rules.rules.is_empty()))
                continue;

            // Blocking rules can run without broad host grants. Explicit user
            // site restrictions apply to the destination and source in all cases.
            auto site_is_enabled = [&](StringView site) {
                return !extension.revoked_hosts.contains(site) && (extension.allow_all_sites || extension.granted_extra_hosts.contains(site));
            };
            if (!site_is_enabled(host) || (initiator.has_value() && !site_is_enabled(initiator->serialized_host())))
                continue;

            auto has_host_access = has_host_permission(extension.extension_id, request.url)
                && (is_navigation || (initiator.has_value() && has_host_permission(extension.extension_id, *initiator)));
            // With declarativeNetRequestWithHostAccess alone, every rule needs host access, as in Firefox.
            if (!has_host_access && !extension.manifest.has_permission("declarativeNetRequest"sv))
                continue;
            auto match = match_declarative_request(extension.declarative_rulesets, request, initiator, has_host_access, extension.dynamic_declarative_rules.rules, extension.session_declarative_rules.rules);
            if (request.event == Event::BeforeRequest && match.winner) {
                if (match.winner->action == Action::Block)
                    return { .cancel = true };
                if (match.winner->action != Action::Redirect || decision.redirect_url.has_value())
                    continue;
                if (match.winner->extension_path.has_value() && !extension.manifest.is_web_accessible(*match.winner->extension_path, initiator))
                    continue;
                decision.redirect_url = declarative_redirect(*match.winner, request.url, extension.uuid);
            } else if (request.event == Event::HeadersReceived) {
                for (auto const* rule : match.response_header_rules) {
                    for (auto const& change : rule->response_headers) {
                        if (!decision.response_headers.has_value())
                            decision.response_headers = request.headers;
                        auto& headers = *decision.response_headers;
                        if (change.append_value.has_value()) {
                            // Separate CSP fields are enforced independently, so an
                            // extension can only add policy, never weaken an existing one.
                            headers.append(HTTP::Header { change.name.to_byte_string(), change.append_value->to_byte_string() });
                        } else if (modified_headers.set(change.name) == AK::HashSetResult::InsertedNewEntry) {
                            headers.remove_all_matching([&](HTTP::Header const& header) {
                                return header.name.equals_ignoring_ascii_case(change.name.bytes_as_string_view());
                            });
                        }
                    }
                }
            }
        }
        return decision;
    });
    m_web_request_coordinator.set_dispatch_event_callback([this](
                                                              String const& ext_id,
                                                              StringView ns_name,
                                                              StringView ev_name,
                                                              String const& args_json,
                                                              Function<void(bool, String const&)> reply_cb) {
        dispatch_event_to_extension(ext_id, ns_name, ev_name, args_json, move(reply_cb));
    });
}

ExtensionManager::~ExtensionManager()
{
    shutdown();
}

void ExtensionManager::shutdown()
{
    m_alarms.clear();
    m_session_storage = ExtensionStorage::create();
    m_content_script_session_storage_access.clear();
    m_pending_event_replies.clear();
    m_pending_script_executions.clear();
    m_script_execution_groups.clear();

    Vector<String> keys;
    for (auto const& [ext_id, _] : m_background_hosts)
        keys.append(ext_id);
    for (auto const& ext_id : keys)
        stop_background_page(ext_id);
    m_runtime_ports.clear();
}

ErrorOr<void> ExtensionManager::initialize_persisted_extensions()
{
    if (auto revocations = m_revocation_list.load_bundled(s_ladybird_resource_root); revocations.is_error()) {
        m_catalog_client.set_enable_public_amo_catalog(false);
        m_installer.disable_verification();
        return revocations.release_error();
    }
    m_installer.cleanup_stale_staging();
    if (auto err = load_registry(); err.is_error())
        warnln("ExtensionManager: Failed loading extensions registry: {}", err.error());
    return {};
}

ErrorOr<void> ExtensionManager::load_registry()
{
    auto registry_path = LexicalPath::join(m_extensions_root, "registry.json"sv).string();
    if (!FileSystem::exists(registry_path))
        return {};

    auto text = TRY(read_utf8_file_bounded(registry_path));
    auto parsed = TRY(JsonValue::from_string(text));
    if (!parsed.is_object())
        return Error::from_string_literal("registry.json must be a JSON object");

    auto const& root = parsed.as_object();
    if (auto width = root.get_u32("sidebar_width_px"sv); width.has_value())
        m_sidebar_controller.set_persisted_default_width(*width);

    if (auto exts = root.get_array("extensions"sv); exts.has_value()) {
        for (auto const& item : exts->values()) {
            if (!item.is_object())
                continue;
            auto const& obj = item.as_object();
            auto id = obj.get_string("extension_id"sv);
            auto uuid = obj.get_string("uuid"sv);
            auto dir = obj.get_string("install_directory"sv);
            if (!id.has_value() || !uuid.has_value() || !dir.has_value())
                continue;
            m_persisted_uuid_by_id.set(*id, *uuid);

            bool is_signed_package = obj.get_string("source"sv).value_or({}) == "signed_xpi"sv;
            if (!m_extensions_enabled || !is_signed_package)
                continue;

            ByteString dir_path = dir->to_byte_string();
            auto sha256_hex = obj.get_string("sha256_hex"sv).value_or({});
            auto manifest = m_installer.validate_installed_package(dir_path, *id, sha256_hex);
            if (manifest.is_error()) {
                warnln("ExtensionManager: could not verify installed package {}: {}", *id, manifest.error());
                continue;
            }

            auto compat = Web::Extensions::SupportMatrix::evaluate_manifest(manifest.value(), sha256_hex.is_empty() ? Optional<StringView> {} : Optional<StringView> { sha256_hex });
            if (compat.status == Web::Extensions::CompatibilityStatus::Incompatible)
                continue;
            auto rulesets = read_declarative_rulesets(manifest.value(), dir_path);
            if (rulesets.is_error()) {
                warnln("ExtensionManager: could not load declarative rules for {}: {}", *id, rulesets.error());
                continue;
            }
            if (auto enabled_ids = obj.get_array("enabled_static_rulesets"sv); enabled_ids.has_value()) {
                Vector<bool> enabled;
                for (auto const& ruleset : rulesets.value())
                    enabled.append(any_of(enabled_ids->values(), [&](JsonValue const& value) { return value.is_string() && ruleset.id == value.as_string(); }));
                if (declarative_ruleset_limits_ok(rulesets.value(), enabled)) {
                    for (size_t i = 0; i < enabled.size(); ++i)
                        rulesets.value()[i].enabled = enabled[i];
                }
            }

            InstalledExtension entry {
                .extension_id = *id,
                .uuid = *uuid,
                .source = Web::Extensions::InstallSource::SignedXpi,
                .install_directory = dir_path,
                .manifest = manifest.release_value(),
                .compatibility_status = compat.status,
                .sha256_hex = move(sha256_hex),
                .verified_signature = true,
                .enabled = obj.get_bool("enabled"sv).value_or(true),
                .grant_generation = obj.get_u64("grant_generation"sv).value_or(1),
                .allow_all_sites = obj.get_bool("allow_all_sites"sv).value_or(true),
                .granted_extra_hosts = {},
                .revoked_hosts = {},
                .active_tab_ids = {},
                .from_catalog = obj.get_bool("from_catalog"sv).value_or(false),
                .network_prediction_setting = obj.get_bool("network_prediction_enabled"sv),
                .declarative_rulesets = rulesets.release_value(),
            };
            if (auto granted = obj.get_array("granted_extra_hosts"sv); granted.has_value()) {
                for (auto const& h : granted->values()) {
                    if (h.is_string())
                        entry.granted_extra_hosts.set(h.as_string());
                }
            }
            if (auto revoked = obj.get_array("revoked_hosts"sv); revoked.has_value()) {
                for (auto const& h : revoked->values()) {
                    if (h.is_string())
                        entry.revoked_hosts.set(h.as_string());
                }
            }
            if (auto scripts = obj.get_array("registered_content_scripts"sv); scripts.has_value()
                && entry.manifest.has_permission("scripting"sv) && scripts->size() <= 100) {
                for (auto const& value : scripts->values()) {
                    auto script = parse_registered_content_script(value, dir_path);
                    if (!script.is_error() && script.value().persist_across_sessions)
                        entry.registered_content_scripts.append(script.release_value());
                }
            }
            if (auto dynamic_rules = obj.get_array("dynamic_declarative_rules"sv); dynamic_rules.has_value() && Web::Extensions::uses_declarative_net_request(entry.manifest)) {
                auto loaded = parse_mutable_ruleset(*dynamic_rules);
                if (!loaded.is_error())
                    entry.dynamic_declarative_rules = loaded.release_value();
            }
            auto pinned = obj.get_bool("pinned"sv).value_or(false);
            register_extension_entry(move(entry));
            m_action_controller.set_pinned(*id, pinned);
        }
    }

    return {};
}

ErrorOr<void> ExtensionManager::save_registry(InstalledExtension const* replacement) const
{
    TRY(Core::Directory::create(m_extensions_root, Core::Directory::CreateDirectories::Yes));

    JsonObject root;
    root.set("version"sv, 1);
    root.set("sidebar_width_px"sv, static_cast<u64>(m_sidebar_controller.persisted_default_width()));

    JsonArray exts;
    auto append_extension = [&](InstalledExtension const& ext) {
        JsonObject obj;
        obj.set("extension_id"sv, ext.extension_id);
        obj.set("uuid"sv, ext.uuid);
        obj.set("source"sv, ext.source == Web::Extensions::InstallSource::SignedXpi ? "signed_xpi"_string : "unpacked"_string);
        obj.set("install_directory"sv, MUST(String::from_byte_string(ext.install_directory)));
        obj.set("version"sv, ext.manifest.version());
        obj.set("sha256_hex"sv, ext.sha256_hex);
        obj.set("verified_signature"sv, ext.verified_signature);
        obj.set("from_catalog"sv, ext.from_catalog);
        obj.set("pinned"sv, m_action_controller.resolved_state(ext.extension_id, {}, {}).pinned);
        obj.set("enabled"sv, ext.enabled);
        obj.set("grant_generation"sv, ext.grant_generation);
        obj.set("allow_all_sites"sv, ext.allow_all_sites);
        if (!ext.dynamic_declarative_rules.source.is_empty())
            obj.set("dynamic_declarative_rules"sv, ext.dynamic_declarative_rules.source);
        if (!ext.declarative_rulesets.is_empty()) {
            JsonArray enabled;
            for (auto const& ruleset : ext.declarative_rulesets) {
                if (ruleset.enabled)
                    enabled.must_append(ruleset.id);
            }
            obj.set("enabled_static_rulesets"sv, move(enabled));
        }
        if (ext.network_prediction_setting.has_value() && ext.enabled && ext.manifest.has_permission("privacy"sv))
            obj.set("network_prediction_enabled"sv, *ext.network_prediction_setting);
        JsonArray registered_content_scripts;
        for (auto const& script : ext.registered_content_scripts) {
            if (script.persist_across_sessions)
                registered_content_scripts.must_append(script.descriptor);
        }
        if (!registered_content_scripts.is_empty())
            obj.set("registered_content_scripts"sv, move(registered_content_scripts));

        JsonArray granted;
        for (auto const& h : ext.granted_extra_hosts)
            granted.must_append(h);
        obj.set("granted_extra_hosts"sv, move(granted));

        JsonArray revoked;
        for (auto const& h : ext.revoked_hosts)
            revoked.must_append(h);
        obj.set("revoked_hosts"sv, move(revoked));

        exts.must_append(move(obj));
    };
    for (auto const& [id, ext] : m_extensions_by_id) {
        if (!replacement || id != replacement->extension_id)
            append_extension(ext);
    }
    if (replacement)
        append_extension(*replacement);
    root.set("extensions"sv, move(exts));

    auto registry_path = LexicalPath::join(m_extensions_root, "registry.json"sv).string();
    auto tmp_path = ByteString::formatted("{}.tmp", registry_path);
    auto serialized = root.serialized();

    auto file = TRY(Core::File::open(tmp_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted(serialized.bytes()));
    file->close();
    TRY(Core::System::rename(tmp_path, registry_path));
    return {};
}

void ExtensionManager::register_extension_entry(InstalledExtension ext, bool first_install)
{
    bool prediction_was_enabled = network_prediction_enabled();
    if (!ext.enabled || !ext.manifest.has_permission("privacy"sv))
        ext.network_prediction_setting.clear();
    auto ext_id = ext.extension_id;
    auto uuid = ext.uuid;
    auto manifest = ext.manifest;
    bool enabled = ext.enabled;
    bool pinned = m_action_controller.resolved_state(ext_id, {}, {}).pinned;

    m_page_menus.remove(ext_id);
    m_id_by_uuid.set(uuid, ext_id);
    m_persisted_uuid_by_id.set(ext_id, uuid);
    m_extensions_by_id.set(ext_id, move(ext));
    refresh_declarative_watch_state();

    m_action_controller.configure_from_manifest(ext_id, manifest);
    m_action_controller.set_pinned(ext_id, pinned);
    if (enabled && manifest.has_permission("idle"sv))
        start_idle_polling();
    if (enabled && manifest.has_permission("downloads"sv))
        ensure_download_events();
    if (enabled) {
        m_sidebar_controller.configure_from_manifest(ext_id, manifest, first_install, current_window_id());
        if (manifest.background().has_value() && manifest.background()->has_background() && manifest.background()->persistent)
            ensure_background_started(ext_id, [](bool) { });
    }
    notify_network_prediction_changed(prediction_was_enabled);
    notify_chrome_changed();
    if (first_install) {
        if (auto installed = m_extensions_by_id.get(ext_id); installed.has_value())
            notify_management_event("onInstalled"sv, *installed);
    }
}

static String iso8601_utc(UnixDateTime time)
{
    time_t seconds = time.seconds_since_epoch();
    struct tm parts {};
    gmtime_r(&seconds, &parts);
    return MUST(String::formatted("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
        parts.tm_hour, parts.tm_min, parts.tm_sec, time.milliseconds_since_epoch() % 1000));
}

// Extensions see the downloads of the profile they run in, not those of private windows or other identities.
bool ExtensionManager::is_extension_visible_download(u64 id) const
{
    auto download = Application::the().file_downloader().download(id);
    return download.has_value() && download->is_private == IsPrivate::No && download->identity_id.is_empty();
}

// Where downloads.download saves, as in Firefox: a relative path within the downloads folder.
ErrorOr<LexicalPath> ExtensionManager::extension_download_destination(Optional<String> const& filename, URL::URL const& url, StringView conflict_action)
{
    if (!conflict_action.is_one_of("uniquify"sv, "overwrite"sv))
        return Error::from_string_literal("Unsupported conflictAction");
    ByteString relative;
    if (filename.has_value() && !filename->is_empty()) {
        auto name = filename->bytes_as_string_view();
        if (name.starts_with('/') || name.contains('\\') || name.contains(':'))
            return Error::from_string_literal("filename must not be an absolute path");
        for (auto part : name.split_view('/', SplitBehavior::KeepEmpty)) {
            if (part.is_empty() || part == "."sv || part == ".."sv || part.starts_with('.'))
                return Error::from_string_literal("filename must not contain back-references or hidden parts");
            if (any_of(part, [](char c) { return static_cast<u8>(c) < 0x20 || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|'; }))
                return Error::from_string_literal("filename must not contain illegal characters");
        }
        relative = name;
    } else {
        auto last = url.path_segment_count() > 0 ? URL::percent_decode(url.path_segment_at_index(url.path_segment_count() - 1)) : ByteString {};
        relative = last.is_empty() || last.contains('/') || last.starts_with('.') ? ByteString { "download"sv } : last;
    }

    auto directory = Core::StandardPaths::downloads_directory();
    auto destination = LexicalPath::join(directory, relative);
    TRY(Core::Directory::create(destination.dirname(), Core::Directory::CreateDirectories::Yes));
    if (conflict_action == "overwrite"sv || !FileSystem::exists(destination.string()))
        return destination;
    for (size_t n = 1; n < 10000; ++n) {
        auto candidate = LexicalPath::join(destination.dirname(), destination.extension().is_empty() ? ByteString::formatted("{}({})", destination.title(), n) : ByteString::formatted("{}({}).{}", destination.title(), n, destination.extension()));
        if (!FileSystem::exists(candidate.string()))
            return candidate;
    }
    return Error::from_string_literal("No free file name for the download");
}

JsonObject ExtensionManager::serialize_extension_download(FileDownloader::Download const& download) const
{
    using Status = FileDownloader::DownloadStatus;
    JsonObject item;
    item.set("id"sv, download.id);
    item.set("url"sv, download.url.serialize());
    item.set("finalUrl"sv, download.url.serialize());
    item.set("referrer"sv, ""sv);
    item.set("filename"sv, MUST(String::from_byte_string(download.destination.string())));
    item.set("incognito"sv, false);
    item.set("danger"sv, "safe"sv);
    item.set("mime"sv, ""sv);
    item.set("startTime"sv, iso8601_utc(download.created_time));
    auto state = download.status == Status::Completed ? "complete"sv : FileDownloader::status_is_active(download.status) ? "in_progress"sv
                                                                                                                         : "interrupted"sv;
    item.set("state"sv, state);
    item.set("paused"sv, download.status == Status::Paused);
    item.set("canResume"sv, download.status == Status::Paused && download.can_resume);
    if (download.status == Status::Canceled)
        item.set("error"sv, "USER_CANCELED"sv);
    else if (download.status == Status::Failed)
        item.set("error"sv, "NETWORK_FAILED"sv);
    item.set("bytesReceived"sv, download.downloaded_size);
    item.set("totalBytes"sv, download.total_size.has_value() ? static_cast<i64>(*download.total_size) : -1);
    item.set("fileSize"sv, download.status == Status::Completed ? static_cast<i64>(download.downloaded_size) : download.total_size.has_value() ? static_cast<i64>(*download.total_size)
                                                                                                                                               : -1);
    item.set("exists"sv, download.status == Status::Completed && FileSystem::exists(download.destination.string()));
    if (auto extension_id = m_extension_downloads.get(download.id); extension_id.has_value()) {
        item.set("byExtensionId"sv, *extension_id);
        if (auto extension = m_extensions_by_id.get(*extension_id); extension.has_value())
            item.set("byExtensionName"sv, extension->manifest.name());
    }
    return item;
}

void ExtensionManager::ensure_download_events()
{
    if (!m_download_events)
        m_download_events = make<DownloadEvents>(*this);
}

void ExtensionManager::dispatch_to_extensions_with_permission(StringView permission, StringView namespace_name, StringView event_name, String const& arguments)
{
    Vector<String> listeners;
    for (auto const& [id, extension] : m_extensions_by_id) {
        if (extension.enabled && (permission.is_empty() || extension.manifest.has_permission(permission)))
            listeners.append(id);
    }
    for (auto const& id : listeners)
        dispatch_event_to_extension(id, namespace_name, event_name, arguments);
}

// An extension uses a cookie when it has access to a site within the cookie's domain.
bool ExtensionManager::extension_may_use_cookie(InstalledExtension const& extension, HTTP::Cookie::Cookie const& cookie) const
{
    for (auto scheme : { "https"sv, "http"sv }) {
        if (cookie.secure && scheme == "http"sv)
            continue;
        auto url = URL::Parser::basic_parse(ByteString::formatted("{}://{}/", scheme, cookie.domain));
        if (url.has_value() && has_host_permission(extension.extension_id, *url))
            return true;
    }
    return false;
}

JsonObject ExtensionManager::serialize_extension_cookie(HTTP::Cookie::Cookie const& cookie)
{
    JsonObject result;
    result.set("name"sv, cookie.name);
    result.set("value"sv, cookie.value);
    result.set("domain"sv, cookie.host_only ? cookie.domain : MUST(String::formatted(".{}", cookie.domain)));
    result.set("hostOnly"sv, cookie.host_only);
    result.set("path"sv, cookie.path);
    result.set("secure"sv, cookie.secure);
    result.set("httpOnly"sv, cookie.http_only);
    auto same_site = cookie.same_site == HTTP::Cookie::SameSite::Strict ? "strict"sv
        : cookie.same_site == HTTP::Cookie::SameSite::Lax               ? "lax"sv
                                                                        : "no_restriction"sv;
    result.set("sameSite"sv, same_site);
    result.set("session"sv, !cookie.persistent);
    if (cookie.persistent)
        result.set("expirationDate"sv, static_cast<double>(cookie.expiry_time.milliseconds_since_epoch()) / 1000.0);
    result.set("storeId"sv, cookie_store_id);
    result.set("firstPartyDomain"sv, ""sv);
    if (cookie.partition_key.is_empty()) {
        result.set("partitionKey"sv, JsonValue {});
    } else {
        JsonObject partition;
        partition.set("topLevelSite"sv, cookie.partition_key.to_utf8());
        result.set("partitionKey"sv, move(partition));
    }
    return result;
}

void ExtensionManager::cookie_changed(HTTP::Cookie::Cookie const& cookie, bool removed)
{
    Vector<String> listeners;
    for (auto const& [id, extension] : m_extensions_by_id) {
        if (extension.enabled && extension.manifest.has_permission("cookies"sv) && extension_may_use_cookie(extension, cookie))
            listeners.append(id);
    }
    if (listeners.is_empty())
        return;
    JsonObject change;
    change.set("removed"sv, removed);
    change.set("cookie"sv, serialize_extension_cookie(cookie));
    // A cookie the browser deletes is given the earliest expiry; one that runs out keeps its own.
    change.set("cause"sv, !removed ? "explicit"sv : cookie.expiry_time == UnixDateTime::earliest() ? "explicit"sv
                                                                                                   : "expired"sv);
    JsonArray arguments;
    arguments.must_append(move(change));
    auto serialized = arguments.serialized();
    for (auto const& id : listeners)
        dispatch_event_to_extension(id, "cookies"sv, "onChanged"sv, serialized);
}

// What a native application's port or message ends with. Ladyflock finds hosts where Firefox does, with Firefox's
// errors, but does not yet start them.
String ExtensionManager::native_application_error(StringView name, StringView extension_id)
{
    auto host = NativeMessaging::find_host(name, extension_id);
    if (host.is_error())
        return MUST(String::formatted("Invalid native application {}: {}", name, host.error()));
    if (!host.value().has_value())
        return MUST(String::formatted("No such native application {}", name));
    return MUST(String::formatted("Ladyflock does not start native applications such as {}", name));
}

void ExtensionManager::close_extension_notifications(StringView extension_id)
{
    auto notifications = m_notifications.take(extension_id);
    if (!notifications.has_value())
        return;
    for (auto const& [_, notification] : *notifications)
        NotificationManager::the().close_for_extension(notification.platform_id);
}

String ExtensionManager::idle_state_for(u32 detection_interval_seconds)
{
    auto status = system_idle_status();
    if (status.locked)
        return "locked"_string;
    if (status.idle_time.has_value() && *status.idle_time >= AK::Duration::from_seconds(detection_interval_seconds))
        return "idle"_string;
    return "active"_string;
}

void ExtensionManager::start_idle_polling()
{
    if (m_idle_poll_timer)
        return;
    m_idle_poll_timer = Core::Timer::create_repeating(AK::Duration::from_seconds(5), [this] { poll_idle_state(); });
    m_idle_poll_timer->start();
}

// idle.onStateChanged fires when the state an extension's detection interval gives changes.
void ExtensionManager::poll_idle_state()
{
    Vector<String> listeners;
    for (auto const& [id, extension] : m_extensions_by_id) {
        if (extension.enabled && extension.manifest.has_permission("idle"sv))
            listeners.append(id);
    }
    for (auto const& id : listeners) {
        auto state = idle_state_for(m_idle_detection_intervals.get(id).value_or(60));
        auto previous = m_idle_reported_states.get(id).copy();
        m_idle_reported_states.set(id, state);
        if (!previous.has_value() || *previous == state)
            continue;
        JsonArray arguments;
        arguments.must_append(state);
        dispatch_event_to_extension(id, "idle"sv, "onStateChanged"sv, arguments.serialized());
    }
}

void ExtensionManager::notify_management_event(StringView event, InstalledExtension const& subject)
{
    auto info = management_info(subject);
    Vector<String> listeners;
    for (auto const& [id, extension] : m_extensions_by_id) {
        if (id != subject.extension_id && extension.enabled && extension.manifest.has_permission("management"sv))
            listeners.append(id);
    }
    for (auto const& id : listeners) {
        JsonArray arguments;
        arguments.must_append(info);
        dispatch_event_to_extension(id, "management"sv, event, arguments.serialized());
    }
}

void ExtensionManager::dispatch_installed_event(InstalledExtension const& extension, Optional<String> previous_version)
{
    if (!extension.enabled)
        return;
    JsonObject details;
    details.set("reason"sv, previous_version.has_value() ? "update"_string : "install"_string);
    if (previous_version.has_value())
        details.set("previousVersion"sv, *previous_version);
    JsonArray arguments;
    arguments.must_append(move(details));
    dispatch_event_to_extension(extension.extension_id, "runtime"sv, "onInstalled"sv, arguments.serialized());
}

ErrorOr<InstalledExtension> ExtensionManager::load_unpacked(ByteString const& directory, bool enabled)
{
    auto resolved_dir = TRY(FileSystem::real_path(directory));
    auto manifest_path = LexicalPath::join(resolved_dir, "manifest.json"sv).string();
    auto manifest_text = TRY(read_utf8_file_bounded(manifest_path));
    auto manifest = TRY(Web::Extensions::ExtensionManifest::parse(manifest_text));

    auto compatibility = Web::Extensions::SupportMatrix::evaluate_manifest(manifest);
    if (compatibility.status == Web::Extensions::CompatibilityStatus::Incompatible) {
        for (auto const& reason : compatibility.unsupported_reasons)
            warnln("Unpacked extension {}: {}", manifest.name(), reason);
        return Error::from_string_literal("Extension manifest uses unsupported features");
    }
    auto rulesets = TRY(read_declarative_rulesets(manifest, resolved_dir));

    String ext_id;
    if (manifest.gecko_id().has_value() && !manifest.gecko_id()->is_empty()) {
        ext_id = *manifest.gecko_id();
    } else {
        // Deterministic unpacked ID keyed on directory + profile so reloads keep their ID/UUID while distinct profiles isolate.
        ext_id = MUST(String::formatted("unpacked-{}@ladyflock.local", sha256_prefix_hex(resolved_dir.bytes())));
    }

    if (auto installed = m_extensions_by_id.get(ext_id); installed.has_value() && installed->source == Web::Extensions::InstallSource::SignedXpi)
        return Error::from_string_literal("Unpacked extension cannot replace an installed signed extension");

    auto uuid = m_persisted_uuid_by_id.get(ext_id).value_or(generate_random_uuid());
    u64 grant_generation = 1;
    if (auto existing = m_extensions_by_id.get(ext_id); existing.has_value())
        grant_generation = existing->grant_generation + 1;

    InstalledExtension entry {
        .extension_id = ext_id,
        .uuid = uuid,
        .source = Web::Extensions::InstallSource::UnpackedDev,
        .install_directory = resolved_dir,
        .manifest = move(manifest),
        .compatibility_status = compatibility.status,
        .sha256_hex = {},
        .verified_signature = false,
        .enabled = enabled,
        .grant_generation = grant_generation,
        .allow_all_sites = true,
        .granted_extra_hosts = {},
        .revoked_hosts = {},
        .active_tab_ids = {},
        .declarative_rulesets = move(rulesets),
    };

    m_extensions_enabled = true;
    bool first_install = !m_extensions_by_id.contains(ext_id);
    TRY(save_registry(&entry));
    register_extension_entry(move(entry), first_install);
    if (first_install)
        dispatch_installed_event(*m_extensions_by_id.get(ext_id));
    return *m_extensions_by_id.get(ext_id);
}

ErrorOr<StagedInstall> ExtensionManager::stage_xpi(ByteString const& xpi_path, Optional<String> const& expected_addon_id, InstallDiagnostic* diagnostic)
{
    auto staged = TRY(m_installer.stage_xpi(xpi_path, expected_addon_id, {}, {}, diagnostic));
    if (m_extensions_by_id.contains(staged.addon_id)) {
        TRY(m_installer.cancel_install(staged.token));
        return Error::from_string_literal("Extension is already installed. Use its update action.");
    }
    return staged;
}

ErrorOr<StagedInstall> ExtensionManager::stage_catalog_package(CatalogEntry const& listing, ByteBuffer bytes, bool is_update, InstallDiagnostic* diagnostic)
{
    if (!m_catalog_client.enable_public_amo_catalog() || !listing.sha256_hex.has_value())
        return Error::from_string_literal("Mozilla Add-ons installation is unavailable");
    auto installed = m_extensions_by_id.get(listing.addon_id);
    if (is_update) {
        if (!installed.has_value() || !installed->from_catalog)
            return Error::from_string_literal("Extension has no Mozilla Add-ons update source");
        auto staged = TRY(m_installer.stage_package(move(bytes), listing.addon_id, listing.version, listing.sha256_hex, installed->manifest, installed->sha256_hex, diagnostic));
        dispatch_update_available_event(staged);
        return staged;
    }
    if (installed.has_value())
        return Error::from_string_literal("Extension is already installed. Use its update action.");
    return m_installer.stage_package(move(bytes), listing.addon_id, listing.version, listing.sha256_hex, {}, {}, diagnostic);
}

ErrorOr<InstalledExtension> ExtensionManager::approve_install(StringView token)
{
    auto staged = m_installer.staged_install(token);
    if (!staged.has_value())
        return Error::from_string_literal("Staged installation token not found");
    if (staged->is_update)
        return Error::from_string_literal("Staged update must be approved through its update action");
    auto ext_id = staged->addon_id;
    auto from_catalog = staged->from_catalog;

    if (m_extensions_by_id.contains(ext_id))
        return Error::from_string_literal("Extension is already installed. Use its update action.");

    Optional<InstalledExtension> next;
    auto commit = TRY(m_installer.commit_staged(token, true, {}, [&](CommitResult const& committed) -> ErrorOr<void> {
        auto manifest = TRY(Web::Extensions::ExtensionManifest::parse(committed.manifest_json));
        auto rulesets = TRY(read_declarative_rulesets(manifest, committed.install_directory));
        auto uuid = m_persisted_uuid_by_id.get(committed.addon_id).value_or(generate_random_uuid());

        next = InstalledExtension {
            .extension_id = committed.addon_id,
            .uuid = uuid,
            .source = Web::Extensions::InstallSource::SignedXpi,
            .install_directory = committed.install_directory,
            .manifest = move(manifest),
            .compatibility_status = committed.compatibility_status,
            .sha256_hex = committed.sha256_hex,
            .verified_signature = true,
            .enabled = true,
            .grant_generation = 1,
            .allow_all_sites = true,
            .granted_extra_hosts = {},
            .revoked_hosts = {},
            .active_tab_ids = {},
            .from_catalog = from_catalog,
            .declarative_rulesets = move(rulesets),
        };
        return save_registry(&*next);
    }));

    register_extension_entry(next.release_value(), true);
    dispatch_installed_event(*m_extensions_by_id.get(commit.addon_id));
    return *m_extensions_by_id.get(commit.addon_id);
}

ErrorOr<void> ExtensionManager::cancel_install(StringView token)
{
    return m_installer.cancel_install(token);
}

ErrorOr<StagedInstall> ExtensionManager::stage_update(StringView addon_id, ByteString const& xpi_path, InstallDiagnostic* diagnostic)
{
    auto existing = m_extensions_by_id.get(addon_id);
    if (!existing.has_value() || existing->source != Web::Extensions::InstallSource::SignedXpi)
        return Error::from_string_literal("A signed version of the extension is not installed");
    auto staged = TRY(m_installer.stage_xpi(xpi_path, existing->extension_id, existing->manifest, existing->sha256_hex, diagnostic));
    dispatch_update_available_event(staged);
    return staged;
}

void ExtensionManager::dispatch_update_available_event(StagedInstall const& staged)
{
    JsonObject details;
    details.set("version"sv, staged.version);
    JsonArray arguments;
    arguments.must_append(move(details));
    dispatch_event_to_extension(staged.addon_id, "runtime"sv, "onUpdateAvailable"sv, arguments.serialized());
}

ErrorOr<InstalledExtension> ExtensionManager::approve_update(StringView token, bool user_consented)
{
    auto staged = m_installer.staged_install(token);
    if (!staged.has_value())
        return Error::from_string_literal("Staged update token not found");
    if (!staged->is_update)
        return Error::from_string_literal("Staged installation must be approved through its install action");

    auto ext_id = staged->addon_id;
    auto existing = m_extensions_by_id.get(ext_id);
    if (!existing.has_value())
        return Error::from_string_literal("Extension to update is no longer installed");
    if (staged->previous_sha256_hex != existing->sha256_hex)
        return Error::from_string_literal("Installed extension changed. Check for updates again.");

    auto old_entry = *existing;
    Optional<InstalledExtension> next;
    TRY(m_installer.commit_staged(token, user_consented, old_entry.install_directory, [&](CommitResult const& committed) -> ErrorOr<void> {
        auto manifest = TRY(Web::Extensions::ExtensionManifest::parse(committed.manifest_json));
        auto rulesets = TRY(read_declarative_rulesets(manifest, committed.install_directory));
        InstalledExtension updated = old_entry;
        updated.install_directory = committed.install_directory;
        updated.manifest = move(manifest);
        updated.declarative_rulesets = move(rulesets);
        updated.session_declarative_rules = {};
        if (!Web::Extensions::uses_declarative_net_request(updated.manifest))
            updated.dynamic_declarative_rules = {};
        updated.compatibility_status = committed.compatibility_status;
        updated.sha256_hex = committed.sha256_hex;
        updated.verified_signature = true;
        updated.grant_generation = old_entry.grant_generation + 1;
        updated.active_tab_ids.clear();
        updated.registered_content_scripts.clear();
        next = move(updated);
        return save_registry(&*next);
    }));

    m_alarms.clear_extension(ext_id);
    m_session_storage->delete_extension_data(ext_id);
    m_content_script_session_storage_access.remove(ext_id);
    stop_background_page(ext_id);
    broadcast_world_revocation(ext_id, old_entry.grant_generation + 1);
    m_web_request_coordinator.remove_all_for_extension(ext_id);
    register_extension_entry(next.release_value());
    if (auto available = m_available_updates.get(ext_id); available.has_value() && compare_versions(*available, m_extensions_by_id.get(ext_id)->manifest.version()) <= 0)
        m_available_updates.remove(ext_id);
    notify_chrome_changed();
    dispatch_installed_event(*m_extensions_by_id.get(ext_id), old_entry.manifest.version());
    return *m_extensions_by_id.get(ext_id);
}

int ExtensionManager::compare_versions(StringView left, StringView right)
{
    // The installer refuses an update that isn't newer by the same rule, so an offered update can always install.
    return compare_extension_versions(left, right);
}

bool ExtensionManager::can_check_for_update(InstalledExtension const& extension) const
{
    return extension.from_catalog && m_catalog_client.enable_public_amo_catalog();
}

Optional<String> ExtensionManager::available_update_version(StringView extension_id) const
{
    return m_available_updates.get(extension_id).copy();
}

bool ExtensionManager::record_listed_version(StringView extension_id, StringView listed_version)
{
    auto installed = m_extensions_by_id.get(extension_id);
    if (!installed.has_value())
        return false;
    bool is_newer = compare_versions(listed_version, installed->manifest.version()) > 0;
    auto previous = m_available_updates.get(extension_id).copy();
    if (is_newer)
        m_available_updates.set(installed->extension_id, MUST(String::from_utf8(listed_version)));
    else
        m_available_updates.remove(extension_id);
    if (previous != m_available_updates.get(extension_id).copy())
        notify_chrome_changed();
    return is_newer;
}

void ExtensionManager::check_for_updates(Function<void(UpdateCheckResult)> on_complete)
{
    Vector<String> ids;
    for (auto const& [id, extension] : m_extensions_by_id) {
        if (can_check_for_update(extension))
            ids.append(id);
    }
    struct PendingCheck : public RefCounted<PendingCheck> {
        size_t remaining { 0 };
        UpdateCheckResult result;
        Function<void(UpdateCheckResult)> on_complete;
    };
    auto pending = make_ref_counted<PendingCheck>();
    pending->remaining = ids.size();
    pending->on_complete = move(on_complete);
    if (ids.is_empty()) {
        pending->on_complete(move(pending->result));
        return;
    }
    for (auto const& id : ids) {
        m_catalog_client.lookup(id, [weak = make_weak_ptr<ExtensionManager>(), pending, id](ErrorOr<CatalogEntry> listing) {
            auto* self = weak.ptr();
            if (self && !listing.is_error() && listing.value().addon_id == id) {
                self->record_listed_version(id, listing.value().version);
                ++pending->result.checked;
            } else {
                pending->result.failed_extension_ids.append(id);
            }
            if (--pending->remaining == 0)
                pending->on_complete(move(pending->result));
        });
    }
}

ErrorOr<void> ExtensionManager::set_extension_enabled(StringView extension_id, bool enabled)
{
    auto it = m_extensions_by_id.find(extension_id);
    if (it == m_extensions_by_id.end())
        return Error::from_string_literal("Extension not found");

    if (it->value.enabled == enabled)
        return {};

    bool prediction_was_enabled = network_prediction_enabled();
    it->value.enabled = enabled;
    refresh_declarative_watch_state();
    ++it->value.grant_generation;
    auto gen = it->value.grant_generation;

    if (!enabled) {
        close_extension_notifications(extension_id);
        it->value.active_tab_ids.clear();
        it->value.network_prediction_setting.clear();
        m_page_menus.remove(extension_id);
        m_sidebar_controller.remove_extension(extension_id);
        m_alarms.clear_extension(extension_id);
        m_session_storage->delete_extension_data(extension_id);
        m_content_script_session_storage_access.remove(extension_id);
        stop_background_page(extension_id);
        m_web_request_coordinator.remove_all_for_extension(extension_id);
        broadcast_world_revocation(extension_id, gen);
    } else {
        auto pinned = m_action_controller.resolved_state(extension_id).pinned;
        m_action_controller.configure_from_manifest(it->value.extension_id, it->value.manifest);
        m_action_controller.set_pinned(it->value.extension_id, pinned);
        m_sidebar_controller.configure_from_manifest(it->value.extension_id, it->value.manifest, false);
        if (it->value.manifest.background().has_value() && it->value.manifest.background()->has_background() && it->value.manifest.background()->persistent)
            ensure_background_started(it->value.extension_id, [](bool) { });
    }

    notify_network_prediction_changed(prediction_was_enabled);
    notify_chrome_changed();
    notify_management_event(enabled ? "onEnabled"sv : "onDisabled"sv, it->value);
    return save_registry();
}

static HashTable<String> granted_origin_patterns(InstalledExtension const& extension)
{
    HashTable<String> origins;
    if (extension.allow_all_sites) {
        for (auto const& pattern : extension.manifest.host_permissions()) {
            if (!any_of(extension.revoked_hosts, [&](auto const& host) { return pattern.covers_host(host); }))
                origins.set(pattern.raw());
        }
    }
    for (auto const& host : extension.granted_extra_hosts) {
        if (extension.revoked_hosts.contains(host))
            continue;
        for (auto scheme : { "http"sv, "https"sv }) {
            auto origin = MUST(String::formatted("{}://{}/*", scheme, host));
            auto requested = Web::Extensions::MatchPattern::parse(origin);
            if (requested.is_error())
                continue;
            auto covers_origin = [&](auto const& pattern) { return pattern.covers_origin(requested.value()); };
            if (any_of(extension.manifest.host_permissions(), covers_origin) || any_of(extension.manifest.optional_host_permissions(), covers_origin))
                origins.set(move(origin));
        }
    }
    return origins;
}

ErrorOr<void> ExtensionManager::set_site_access(StringView extension_id, bool allow_all_sites, Optional<String> toggle_host, bool grant_host)
{
    auto it = m_extensions_by_id.find(extension_id);
    if (it == m_extensions_by_id.end())
        return Error::from_string_literal("Extension not found");

    if (toggle_host.has_value()) {
        auto url = URL::Parser::basic_parse(MUST(String::formatted("https://{}/", *toggle_host)));
        if (!url.has_value() || url->serialized_host().is_empty() || url->serialize_path() != "/"sv
            || url->includes_credentials() || url->port().has_value() || url->query().has_value() || url->fragment().has_value())
            return Error::from_string_literal("Enter a hostname without a protocol, port, or path");
        toggle_host = TRY(String::from_utf8(url->serialized_host()));
    }

    auto previous = it->value;
    auto previous_origins = granted_origin_patterns(previous);
    it->value.allow_all_sites = allow_all_sites;
    if (toggle_host.has_value()) {
        if (grant_host) {
            it->value.revoked_hosts.remove(*toggle_host);
            it->value.granted_extra_hosts.set(*toggle_host);
        } else {
            it->value.granted_extra_hosts.remove(*toggle_host);
            it->value.revoked_hosts.set(*toggle_host);
        }
    }

    it->value.active_tab_ids.clear();
    ++it->value.grant_generation;
    if (auto saved = save_registry(); saved.is_error()) {
        it->value = move(previous);
        return saved.release_error();
    }
    broadcast_world_revocation(extension_id, it->value.grant_generation, false);
    notify_chrome_changed();
    auto current_origins = granted_origin_patterns(it->value);
    auto notify = [&](StringView event, HashTable<String> const& first, HashTable<String> const& second) {
        JsonArray origins;
        for (auto const& origin : first) {
            if (!second.contains(origin))
                origins.must_append(origin);
        }
        if (origins.is_empty())
            return;
        JsonObject permissions;
        permissions.set("origins"sv, move(origins));
        JsonArray args;
        args.must_append(move(permissions));
        dispatch_event_to_extension(it->value.extension_id, "permissions"sv, event, args.serialized());
    };
    notify("onRemoved"sv, previous_origins, current_origins);
    notify("onAdded"sv, current_origins, previous_origins);
    return {};
}

ErrorOr<void> ExtensionManager::remove_extension(StringView extension_id)
{
    bool prediction_was_enabled = network_prediction_enabled();
    auto ext = m_extensions_by_id.take(extension_id);
    if (!ext.has_value())
        return Error::from_string_literal("Extension not found");
    refresh_declarative_watch_state();

    m_alarms.clear_extension(extension_id);
    close_extension_notifications(extension_id);
    stop_background_page(extension_id);
    m_web_request_coordinator.remove_all_for_extension(extension_id);
    broadcast_world_revocation(extension_id, ext->grant_generation + 1);
    m_page_menus.remove(extension_id);
    m_action_controller.remove_extension(extension_id);
    m_sidebar_controller.remove_extension(extension_id);
    m_storage->delete_extension_data(extension_id);
    m_sync_storage->delete_extension_data(extension_id);
    m_session_storage->delete_extension_data(extension_id);
    m_content_script_session_storage_access.remove(extension_id);
    m_id_by_uuid.remove(ext->uuid);
    m_persisted_uuid_by_id.remove(extension_id);
    m_available_updates.remove(extension_id);
    notify_management_event("onUninstalled"sv, *ext);
    // Firefox opens the extension's uninstall page, if it set one, once the extension is gone.
    if (auto uninstall_url = m_uninstall_urls.take(extension_id); uninstall_url.has_value()) {
        auto tabs = ordinary_tab_views();
        if (!tabs.is_empty())
            Application::the().open_url_in_new_tab(*uninstall_url, Web::HTML::ActivateTab::Yes, tabs.first());
    }

    if (ext->source == Web::Extensions::InstallSource::SignedXpi)
        m_installer.remove_installed_package(ext->install_directory);

    notify_network_prediction_changed(prediction_was_enabled);
    notify_chrome_changed();
    return save_registry();
}

Optional<InstalledExtension const&> ExtensionManager::extension_by_id(StringView extension_id) const
{
    return m_extensions_by_id.get(extension_id);
}

Optional<InstalledExtension const&> ExtensionManager::extension_by_uuid(StringView uuid) const
{
    auto id = m_id_by_uuid.get(uuid);
    if (!id.has_value())
        return {};
    return m_extensions_by_id.get(*id);
}

Vector<InstalledExtension const*> ExtensionManager::all_extensions() const
{
    Vector<InstalledExtension const*> list;
    for (auto const& [_, ext] : m_extensions_by_id)
        list.append(&ext);
    return list;
}

Web::Extensions::ExtensionResourceResponse ExtensionManager::serve_resource(URL::URL const& url, Optional<URL::Origin> const& initiator_origin, Optional<size_t> size_limit) const
{
    auto make_error = [](String message) -> Web::Extensions::ExtensionResourceResponse {
        return {
            .allowed = false,
            .data = {},
            .mime_type = {},
            .content_security_policy = {},
            .error = move(message),
        };
    };

    if (url.scheme() != "moz-extension"sv)
        return make_error("Invalid extension scheme"_string);

    auto host = url.serialized_host();
    auto ext = extension_by_uuid(host);
    if (!ext.has_value() || !ext->enabled)
        return make_error("Unknown or disabled extension"_string);

    auto raw_path = url.serialize_path();
    auto rel_or_error = Web::Extensions::ExtensionManifest::normalize_package_relative_path(raw_path);
    if (rel_or_error.is_error())
        return make_error("Invalid extension resource path"_string);
    auto rel_path = rel_or_error.release_value();

    // Enforce web_accessible_resources when requested from a non-extension origin or another extension.
    if (initiator_origin.has_value()) {
        bool same_extension = !initiator_origin->is_opaque()
            && initiator_origin->scheme() == "moz-extension"sv
            && initiator_origin->host().serialize() == ext->uuid;

        if (!same_extension) {
            Optional<URL::URL> initiator_url;
            Optional<StringView> initiator_ext_id;
            if (!initiator_origin->is_opaque()) {
                initiator_url = URL::Parser::basic_parse(initiator_origin->serialize());
                if (initiator_origin->scheme() == "moz-extension"sv) {
                    if (auto other = extension_by_uuid(initiator_origin->host().serialize()); other.has_value())
                        initiator_ext_id = other->extension_id;
                }
            }
            if (!ext->manifest.is_web_accessible(rel_path, initiator_url, initiator_ext_id))
                return make_error("Resource is not listed in web_accessible_resources"_string);
        }
    }

    // Synthesize generated background page HTML when using background.scripts.
    if (rel_path == "_generated_background_page.html"sv && ext->manifest.background().has_value() && !ext->manifest.background()->scripts.is_empty()) {
        StringBuilder html;
        html.append("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"sv);
        auto const& background = *ext->manifest.background();
        for (auto const& script : background.scripts) {
            html.append(background.is_module ? "<script type=\"module\" src=\"/"sv : "<script src=\"/"sv);
            html.append(escape_html_entities(script.bytes_as_string_view()));
            html.append("\"></script>"sv);
        }
        html.append("</head><body></body></html>"sv);
        auto buf = ByteBuffer::copy(html.string_view().bytes());
        if (buf.is_error())
            return make_error("Out of memory"_string);
        return {
            .allowed = true,
            .data = buf.release_value(),
            .mime_type = "text/html; charset=utf-8"_string,
            .content_security_policy = ext->manifest.content_security_policy(),
            .error = {},
        };
    }

    auto data = read_package_resource(ext->install_directory, rel_path, size_limit.value_or(64 * MiB));
    if (data.is_error())
        return make_error("Failed reading extension resource"_string);

    return {
        .allowed = true,
        .data = data.release_value(),
        .mime_type = mime_type_for_path(rel_path),
        .content_security_policy = ext->manifest.content_security_policy(),
        .error = {},
    };
}

bool ExtensionManager::is_extension_document_url(URL::URL const& url) const
{
    if (url.scheme() != "moz-extension"sv)
        return false;
    return extension_by_uuid(url.serialized_host()).has_value();
}

void ExtensionManager::grant_active_tab(StringView extension_id, u64 tab_id)
{
    if (auto it = m_extensions_by_id.find(extension_id); it != m_extensions_by_id.end())
        it->value.active_tab_ids.set(tab_id);
}

void ExtensionManager::clear_active_tab(u64 tab_id)
{
    for (auto& [_, ext] : m_extensions_by_id)
        ext.active_tab_ids.remove(tab_id);
}

void ExtensionManager::clear_tab_grants_on_navigation(u64 tab_id)
{
    clear_active_tab(tab_id);
    m_action_controller.clear_tab_state(tab_id);
    m_sidebar_controller.clear_tab_state(tab_id);
    notify_chrome_changed();
}

bool ExtensionManager::has_host_permission(StringView extension_id, URL::URL const& url, Optional<u64> tab_id) const
{
    auto ext = m_extensions_by_id.get(extension_id);
    if (!ext.has_value() || !ext->enabled)
        return false;

    if (!url.scheme().is_one_of("http"sv, "https"sv))
        return false;

    auto host = url.serialized_host();
    if (ext->revoked_hosts.contains(host))
        return false;
    if (tab_id.has_value() && ext->manifest.has_permission("activeTab"sv) && ext->active_tab_ids.contains(*tab_id)) {
        auto tab = ViewImplementation::find_view_by_id(*tab_id);
        if (tab.has_value() && tab->has_traversable() && tab->traversable().active_document().origin().is_same_origin(url.origin()))
            return true;
    }
    if (ext->granted_extra_hosts.contains(host)) {
        return ext->manifest.has_host_permission_for(url)
            || any_of(ext->manifest.optional_host_permissions(), [&](auto const& pattern) { return pattern.matches(url); });
    }
    if (!ext->allow_all_sites)
        return false;

    return ext->manifest.has_host_permission_for(url);
}

bool ExtensionManager::extension_page_may_read_cross_origin(WebContentClient& caller, Web::PageId page_id, Web::HTML::CrossProcessId document_id, URL::URL const& url) const
{
    // The answer is for the extension this renderer hosts, and only for one of that extension's documents. A content
    // script, which runs in the web page's renderer, never gets it.
    auto const& extension_id = caller.hosted_extension_id();
    if (!extension_id.has_value())
        return false;
    auto* page = caller.page(page_id);
    auto document = page && page->is_open() && page->has_traversable() ? page->hosted_document_with_id(document_id) : nullptr;
    if (!document || document->unload_counter() > 0 || document->extension_id() != *extension_id || document->origin().is_opaque())
        return false;
    return has_host_permission(*extension_id, url);
}

static bool is_legacy_registration_owner_active(RegisteredExtensionContentScript::Owner const& owner)
{
    auto renderer = owner.renderer.strong_ref();
    auto* page = renderer ? renderer->page(owner.page_id) : nullptr;
    if (!page || !page->is_open() || !page->has_traversable())
        return false;
    bool active = false;
    page->for_each_navigable_with_a_hosted_active_document([&](CanonicalNavigable const& frame) {
        if (frame.active_document().id() != owner.document_id || frame.active_document().unload_counter() > 0)
            return IterationDecision::Continue;
        active = true;
        return IterationDecision::Break;
    });
    return active;
}

void ExtensionManager::attach_content_scripts_for_document(WebContentPage& page, CanonicalNavigable const& navigable, CanonicalDocument& document, URL::URL const& document_url) const
{
    if (!m_extensions_enabled)
        return;

    auto const* match_url = &document_url;
    bool inherited_blank = Web::HTML::url_matches_about_blank(document_url) || Web::HTML::url_matches_about_srcdoc(document_url);
    if (inherited_blank) {
        if (document.origin().is_opaque())
            return;
        // Only canonical same-origin ancestors can supply an inherited URL. In particular, a renderer's
        // about base URL is affected by <base> and cannot establish access to another site's document.
        auto const* child = &navigable;
        for (;;) {
            auto const* parent_document = child->container_document();
            if (!parent_document || !parent_document->origin().is_same_origin(document.origin()))
                return;
            match_url = &parent_document->creation_url();
            if (!Web::HTML::url_matches_about_blank(*match_url) && !Web::HTML::url_matches_about_srcdoc(*match_url))
                break;
            child = child->parent();
            if (!child)
                return;
        }
    }
    if (!match_url->scheme().is_one_of("http"sv, "https"sv) || (inherited_blank && !match_url->origin().is_same_origin(document.origin())))
        return;

    Vector<Web::Extensions::AttachedContentScript> attached;
    auto doc_host = match_url->serialized_host();
    for (auto const& [_, ext] : m_extensions_by_id) {
        if (!ext.enabled || ext.revoked_hosts.contains(doc_host))
            continue;

        auto attach = [&](Web::Extensions::ContentScriptRule const& rule, RegisteredExtensionContentScript const* registration) {
            if (!navigable.is_top_level_traversable() && !rule.all_frames)
                return;
            if (inherited_blank && !rule.match_about_blank)
                return;
            if (!ext.allow_all_sites && !ext.granted_extra_hosts.contains(doc_host))
                return;

            if (!Web::Extensions::MatchPattern::matches_url_with_rules(*match_url, rule.matches, rule.exclude_matches, rule.include_globs, rule.exclude_globs))
                return;

            Web::Extensions::AttachedContentScript script;
            script.extension_id = ext.extension_id;
            script.extension_uuid = ext.uuid;
            script.grant_generation = ext.grant_generation;
            script.document_id = document.id();
            script.run_at = rule.run_at;
            script.all_frames = rule.all_frames;
            script.match_about_blank = rule.match_about_blank;
            script.run_in_main_world = rule.main_world || (registration && registration->run_in_main_world);

            if (registration && registration->legacy_owner.has_value()) {
                if (document.origin().is_opaque() || !is_legacy_registration_owner_active(*registration->legacy_owner))
                    return;
                script.js_files = registration->legacy_js;
                script.css_files = registration->legacy_css;
            }

            for (auto const& js_rel : rule.js) {
                if (auto content = read_package_script(ext.install_directory, js_rel); !content.is_error())
                    script.js_files.append({ .path = js_rel, .content = content.release_value() });
            }
            for (auto const& css_rel : rule.css) {
                if (auto content = read_package_script(ext.install_directory, css_rel); !content.is_error())
                    script.css_files.append({ .path = css_rel, .content = content.release_value() });
            }

            if (!script.js_files.is_empty() || !script.css_files.is_empty()) {
                if (!script.run_in_main_world)
                    document.authorize_content_script(script.extension_id, script.grant_generation);
                attached.append(move(script));
            }
        };
        for (auto const& rule : ext.manifest.content_scripts())
            attach(rule, nullptr);
        if (has_host_permission(ext.extension_id, *match_url)) {
            for (auto const& registered : ext.registered_content_scripts)
                attach(registered.rule, &registered);
        }
    }
    if (attached.is_empty())
        return;
    for (auto const& script : attached) {
        if (auto ext = m_extensions_by_id.get(script.extension_id); ext.has_value())
            send_content_script_metadata(page.client(), *ext);
    }
    page.client().async_attach_extension_content_scripts(page.id(), document.id(), move(attached));
}

void ExtensionManager::send_content_script_metadata(WebContentClient& client, InstalledExtension const& extension) const
{
    auto token = MUST(String::formatted("{}/{}/{}", extension.uuid, extension.manifest.version(), extension.sha256_hex));
    if (!client.needs_extension_metadata(extension.extension_id, token))
        return;
    client.async_set_extension_content_script_metadata(extension.extension_id, extension.manifest.raw_json(), locale_messages_for(extension));
}

void ExtensionManager::revoke_document_worlds(WebContentPage& page, Web::HTML::CrossProcessId document_id)
{
    Vector<String> ports;
    for (auto const& [id, port] : m_runtime_ports) {
        if (port.sender.document_id == document_id || (port.receiver.has_value() && port.receiver->document_id == document_id))
            ports.append(id);
    }
    for (auto const& id : ports)
        close_runtime_port(id, document_id);
    for (auto& [ext_id, ext] : m_extensions_by_id) {
        ext.registered_content_scripts.remove_all_matching([&](auto const& script) {
            return script.legacy_owner.has_value() && script.legacy_owner->document_id == document_id;
        });
        page.client().async_revoke_extension_world(page.id(), ext_id, document_id, ext.grant_generation);
    }
}

void ExtensionManager::broadcast_world_revocation(StringView extension_id, u64 new_grant_generation, bool include_extension_pages)
{
    auto ext_str = MUST(String::from_utf8(extension_id));
    if (include_extension_pages) {
        if (auto extension = m_extensions_by_id.get(extension_id); extension.has_value())
            extension->registered_content_scripts.remove_all_matching([](auto const& script) { return script.legacy_owner.has_value(); });
    }
    Vector<String> ports;
    for (auto const& [id, port] : m_runtime_ports) {
        if (port.extension_id == extension_id)
            ports.append(id);
    }
    for (auto const& id : ports)
        close_runtime_port(id);
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.session().extension_manager.ptr() != this || (!include_extension_pages && client.hosted_extension_id() == extension_id))
            return IterationDecision::Continue;
        client.for_each_page([&](WebContentPage& page) {
            page.client().async_revoke_extension_world(page.id(), ext_str, Optional<Web::HTML::CrossProcessId> {}, new_grant_generation);
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });
}

void ExtensionManager::push_watch_filter_to_request_server(Requests::RequestControlClient& control_client, Web::Extensions::WebRequestWatchSnapshot const& snapshot)
{
    bool enabled = snapshot.has_declarative_rules || !snapshot.entries.is_empty();
    bool match_all_urls = snapshot.has_declarative_rules;
    Vector<String> exact_hosts;
    Vector<String> suffix_hosts;
    HashTable<String> seen_exact;
    HashTable<String> seen_suffix;

    for (auto const& entry : snapshot.entries) {
        if (entry.url_patterns.is_empty()) {
            match_all_urls = true;
            break;
        }
        for (auto const& pattern : entry.url_patterns) {
            auto view = pattern.bytes_as_string_view();
            if (view == "<all_urls>"sv) {
                match_all_urls = true;
                break;
            }
            auto scheme_sep = view.find("://"sv);
            if (!scheme_sep.has_value()) {
                match_all_urls = true;
                break;
            }
            auto after_scheme = view.substring_view(*scheme_sep + 3);
            auto path_sep = after_scheme.find('/');
            auto host_port = path_sep.has_value() ? after_scheme.substring_view(0, *path_sep) : after_scheme;
            if (host_port == "*"sv || host_port.is_empty()) {
                match_all_urls = true;
                break;
            }
            StringView host_view = host_port;
            if (host_view.starts_with('[')) {
                if (auto close_bracket = host_view.find(']'); close_bracket.has_value())
                    host_view = host_view.substring_view(1, *close_bracket - 1);
            } else if (auto colon = host_view.find(':'); colon.has_value()) {
                host_view = host_view.substring_view(0, *colon);
            }
            if (host_view == "*"sv) {
                match_all_urls = true;
                break;
            }
            if (host_view.starts_with("*."sv)) {
                auto suffix = MUST(String::from_utf8(host_view.substring_view(2)));
                if (suffix.is_empty() || suffix.bytes_as_string_view().contains('*')) {
                    match_all_urls = true;
                    break;
                }
                if (seen_suffix.set(suffix) == AK::HashSetResult::InsertedNewEntry)
                    suffix_hosts.append(move(suffix));
            } else if (host_view.contains('*')) {
                match_all_urls = true;
                break;
            } else {
                auto exact = MUST(String::from_utf8(host_view));
                if (seen_exact.set(exact) == AK::HashSetResult::InsertedNewEntry)
                    exact_hosts.append(move(exact));
            }
        }
        if (match_all_urls)
            break;
    }

    if (match_all_urls) {
        exact_hosts.clear();
        suffix_hosts.clear();
    }

    control_client.async_set_web_request_watch_filter(enabled, match_all_urls, move(exact_hosts), move(suffix_hosts));
}

void ExtensionManager::push_watch_snapshot_everywhere(Web::Extensions::WebRequestWatchSnapshot const& snapshot)
{
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.session().extension_manager.ptr() == this)
            client.async_set_web_request_watch_snapshot(snapshot);
        return IterationDecision::Continue;
    });
    RequestServerManager::the().for_each_instance([&](RequestServerInstance& instance) {
        if (instance.session().extension_manager.ptr() == this)
            push_watch_filter_to_request_server(instance.control_client(), snapshot);
        return IterationDecision::Continue;
    });
}

void ExtensionManager::refresh_declarative_watch_state()
{
    m_web_request_coordinator.set_has_declarative_rules(any_of(m_extensions_by_id, [](auto const& entry) {
        auto const& extension = entry.value;
        return extension.enabled && (!extension.dynamic_declarative_rules.rules.is_empty() || !extension.session_declarative_rules.rules.is_empty() || any_of(extension.declarative_rulesets, [](auto const& ruleset) {
            return ruleset.enabled && !ruleset.rules.is_empty();
        }));
    }));
}

bool ExtensionManager::is_extension_background_view(ViewImplementation const& view)
{
    return background_views().contains(&view);
}

Optional<InstalledExtension const&> ExtensionManager::network_prediction_owner() const
{
    for (auto const& [_, extension] : m_extensions_by_id) {
        if (extension.enabled && extension.network_prediction_setting.has_value() && extension.manifest.has_permission("privacy"sv))
            return extension;
    }
    return {};
}

bool ExtensionManager::network_prediction_enabled() const
{
    auto owner = network_prediction_owner();
    return !owner.has_value() || *owner->network_prediction_setting;
}

JsonObject ExtensionManager::network_prediction_setting_for(StringView extension_id) const
{
    auto owner = network_prediction_owner();
    StringView level_of_control = "controllable_by_this_extension"sv;
    if (owner.has_value())
        level_of_control = owner->extension_id == extension_id ? "controlled_by_this_extension"sv : "controlled_by_other_extensions"sv;

    JsonObject setting;
    setting.set("value"sv, !owner.has_value() || *owner->network_prediction_setting);
    setting.set("levelOfControl"sv, MUST(String::from_utf8(level_of_control)));
    return setting;
}

static JsonObject read_only_privacy_setting(bool value)
{
    JsonObject setting;
    setting.set("value"sv, value);
    setting.set("levelOfControl"sv, "not_controllable"_string);
    return setting;
}

void ExtensionManager::global_privacy_control_changed()
{
    JsonArray arguments;
    arguments.must_append(read_only_privacy_setting(Application::settings().global_privacy_control() == GlobalPrivacyControl::Yes));
    auto serialized = arguments.serialized();
    for (auto const& [id, extension] : m_extensions_by_id) {
        if (extension.enabled && extension.manifest.has_permission("privacy"sv))
            dispatch_event_to_extension(id, "privacy.network.globalPrivacyControl"sv, "onChange"sv, serialized);
    }
}

void ExtensionManager::notify_network_prediction_changed(bool previously_enabled)
{
    auto enabled = network_prediction_enabled();
    if (enabled == previously_enabled)
        return;

    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.session().extension_manager.ptr() == this)
            client.async_set_network_prediction_enabled(enabled);
        return IterationDecision::Continue;
    });
    for (auto const& [id, extension] : m_extensions_by_id) {
        if (!extension.enabled || !extension.manifest.has_permission("privacy"sv))
            continue;
        JsonArray arguments;
        arguments.must_append(network_prediction_setting_for(id));
        dispatch_event_to_extension(id, "privacy.network.networkPredictionEnabled"sv, "onChange"sv, arguments.serialized());
    }
}

Vector<ViewImplementation*> ExtensionManager::ordinary_tab_views() const
{
    Vector<ViewImplementation*> views;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.is_private() == IsPrivate::No && !view.owner_view_id().has_value() && !is_extension_background_view(view))
            views.append(&view);
        return IterationDecision::Continue;
    });
    return views;
}

Optional<Web::Extensions::ExtensionTabInfo> ExtensionManager::tab_info_for_extension(StringView extension_id, ViewImplementation const& view) const
{
    if (view.is_private() == IsPrivate::Yes || view.owner_view_id().has_value() || is_extension_background_view(view))
        return {};
    auto ext = m_extensions_by_id.get(extension_id);
    if (!ext.has_value() || !ext->enabled)
        return {};

    u64 tab_id = view.view_id();
    auto url = view.url();
    bool expose_sensitive = ext->manifest.has_permission("tabs"sv)
        || (ext->manifest.has_permission("activeTab"sv) && ext->active_tab_ids.contains(tab_id))
        || has_host_permission(extension_id, url, tab_id);

    auto active_view = Application::the().active_web_view();
    bool is_active = active_view.has_value() && &*active_view == &view;

    Web::Extensions::ExtensionTabInfo info;
    info.tab_id = tab_id;
    info.window_id = view.extension_window_id();
    info.active = view.is_current_tab_of_window() || is_active;
    info.pinned = view.is_pinned();
    info.status = view.traversable().has_unactivated_load() ? "loading"_string : "complete"_string;
    if (expose_sensitive) {
        info.url = url.serialize();
        info.title = view.title().to_utf8();
    }
    return info;
}

Optional<u64> ExtensionManager::tab_id_for_page(WebContentClient const& client, Web::PageId page_id) const
{
    auto* page = client.page(page_id);
    if (!page || !page->has_traversable())
        return {};
    auto view = page->traversable().view();
    if (!view.has_value() || view->owner_view_id().has_value() || is_extension_background_view(*view))
        return {};
    return view->view_id();
}

Optional<u64> ExtensionManager::view_id_for_document(Web::HTML::CrossProcessId document_id) const
{
    for (auto* view : ordinary_tab_views()) {
        if (view->traversable().document_with_id(document_id))
            return view->view_id();
    }
    return {};
}

bool ExtensionManager::is_background_running(StringView extension_id) const
{
    auto it = m_background_hosts.find(extension_id);
    return it != m_background_hosts.end() && it->value->loaded;
}

ViewImplementation* ExtensionManager::background_view_for_testing(StringView extension_id)
{
    auto it = m_background_hosts.find(extension_id);
    return it == m_background_hosts.end() ? nullptr : it->value->view.ptr();
}

void ExtensionManager::stop_background_page(StringView extension_id)
{
    auto host = m_background_hosts.take(extension_id);
    if (!host.has_value())
        return;
    if (host.value()->startup_timer)
        host.value()->startup_timer->stop();
    if (host.value()->idle_timer)
        host.value()->idle_timer->stop();
    if (host.value()->view) {
        auto& view = *host.value()->view;
        if (view.has_traversable()) {
            auto& document = view.traversable().active_document();
            if (document.host())
                revoke_document_worlds(*document.host(), document.id());
        }
        background_views().remove(host.value()->view.ptr());
    }
    for (auto& cb : host.value()->pending_ready_callbacks)
        cb(false);
}

void ExtensionManager::schedule_event_page_idle_timer(StringView extension_id)
{
    auto ext = m_extensions_by_id.get(extension_id);
    if (!ext.has_value() || !ext->manifest.background().has_value() || ext->manifest.background()->persistent)
        return;
    auto it = m_background_hosts.find(extension_id);
    if (it == m_background_hosts.end())
        return;
    if (!it->value->idle_timer) {
        it->value->idle_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(m_event_page_idle_timeout_ms), [this, ext_id = ext->extension_id] {
            stop_background_page(ext_id);
        });
    }
    it->value->idle_timer->restart();
}

void ExtensionManager::ensure_background_started(StringView extension_id, Function<void(bool ready)> on_ready)
{
    auto ext = m_extensions_by_id.get(extension_id);
    if (!ext.has_value() || !ext->enabled || !ext->manifest.background().has_value() || !ext->manifest.background()->has_background()) {
        on_ready(false);
        return;
    }

    auto& host = m_background_hosts.ensure(ext->extension_id, [&] {
        return make<BackgroundHost>();
    });

    if (host->loaded) {
        schedule_event_page_idle_timer(extension_id);
        on_ready(true);
        return;
    }

    host->pending_ready_callbacks.append(move(on_ready));
    if (host->view)
        return;

    // Create isolated HeadlessWebView for the extension's background/event page.
    auto theme_path = LexicalPath::join(s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme_or_error = Gfx::load_system_theme(theme_path.string());
    if (theme_or_error.is_error()) {
        auto callbacks = move(host->pending_ready_callbacks);
        for (auto& cb : callbacks)
            cb(false);
        return;
    }

    host->view = HeadlessWebView::create(theme_or_error.release_value(), { 320, 240 }, IsPrivate::No);
    background_views().set(host->view.ptr());
    host->startup_timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [this, id = ext->extension_id] {
        stop_background_page(id);
    });
    host->startup_timer->start();

    auto target_rel = ext->manifest.background()->page.has_value()
        ? *ext->manifest.background()->page
        : "_generated_background_page.html"_string;
    auto bg_url = URL::Parser::basic_parse(ByteString::formatted("moz-extension://{}/{}", ext->uuid, target_rel));
    if (!bg_url.has_value()) {
        stop_background_page(extension_id);
        return;
    }

    host->view->on_load_finish = [this, ext_id = ext->extension_id](URL::URL const& url) {
        if (url.scheme() != "moz-extension"sv)
            return;
        auto it = m_background_hosts.find(ext_id);
        if (it == m_background_hosts.end())
            return;
        it->value->startup_timer->stop();
        it->value->loaded = true;
        schedule_event_page_idle_timer(ext_id);
        auto callbacks = move(it->value->pending_ready_callbacks);
        for (auto& cb : callbacks)
            cb(true);
    };

    host->view->load_from_browser_ui(*bg_url);
}

void ExtensionManager::notify_background_document_loaded(StringView extension_id, WebContentPage&)
{
    auto it = m_background_hosts.find(extension_id);
    if (it == m_background_hosts.end())
        return;
    if (it->value->startup_timer)
        it->value->startup_timer->stop();
    it->value->loaded = true;
    schedule_event_page_idle_timer(extension_id);
    auto callbacks = move(it->value->pending_ready_callbacks);
    for (auto& cb : callbacks)
        cb(true);
}

void ExtensionManager::dispatch_event_to_extension(
    String const& extension_id,
    StringView namespace_name,
    StringView event_name,
    String const& args_json,
    Function<void(bool has_value, String const& result_json)> reply_callback,
    Optional<Web::HTML::CrossProcessId> source_document_id)
{
    auto ext = m_extensions_by_id.get(extension_id);
    if (!ext.has_value() || !ext->enabled) {
        if (reply_callback)
            reply_callback(false, "null"_string);
        return;
    }

    auto deliver = [this, extension_id, ns = MUST(String::from_utf8(namespace_name)), event = MUST(String::from_utf8(event_name)),
                       args_json, callback = move(reply_callback), source_document_id](bool) mutable {
        auto extension = m_extensions_by_id.get(extension_id);
        struct Target {
            WebContentPage& page;
            Web::HTML::CrossProcessId document_id;
        };
        Vector<Target> targets;
        bool dispatch_storage_to_content = false;
        if (ns == "storage"sv) {
            auto arguments = JsonValue::from_string(args_json);
            if (!arguments.is_error() && arguments.value().is_array() && arguments.value().as_array().size() == 2) {
                auto const& area = arguments.value().as_array().at(1);
                dispatch_storage_to_content = area.is_string()
                    && (area.as_string().is_one_of("local"sv, "sync"sv) || (area.as_string() == "session"sv && m_content_script_session_storage_access.contains(extension_id)));
            }
        }
        if (extension.has_value() && extension->enabled) {
            WebContentClient::for_each_client([&](WebContentClient& client) {
                if (client.session().extension_manager.ptr() != this)
                    return IterationDecision::Continue;
                client.for_each_page([&](WebContentPage& page) {
                    if (!page.is_open() || !page.has_traversable())
                        return IterationDecision::Continue;
                    page.for_each_navigable_with_a_hosted_active_document([&](CanonicalNavigable const& navigable) {
                        auto const& document = navigable.active_document();
                        if (document.id() == source_document_id || document.unload_counter() > 0)
                            return IterationDecision::Continue;
                        bool extension_page = client.hosted_extension_id() == extension_id && document.extension_id() == extension_id;
                        bool storage_listener = dispatch_storage_to_content && document.has_content_script_grant(extension_id, extension->grant_generation);
                        if (extension_page || storage_listener)
                            targets.append({ page, document.id() });
                        return IterationDecision::Continue;
                    });
                    return IterationDecision::Continue;
                });
                return IterationDecision::Continue;
            });
        }
        if (targets.is_empty()) {
            if (callback)
                callback(false, "null"_string);
            return;
        }
        schedule_event_page_idle_timer(extension_id);

        struct Replies : public RefCounted<Replies> {
            Function<void(bool, String const&)> callback;
            size_t remaining { 0 };
            bool blocking_event { false };
            bool answered_blocking_event { false };
        };
        auto replies = make_ref_counted<Replies>();
        replies->callback = move(callback);
        replies->remaining = targets.size();
        replies->blocking_event = ns == "webRequest"sv;
        for (auto const& target : targets) {
            Optional<u64> reply_id;
            if (replies->callback) {
                reply_id = register_event_reply([replies](bool has_value, String const& result) {
                    if (!replies->callback)
                        return;
                    if (has_value && replies->blocking_event) {
                        auto response = JsonValue::from_string(result);
                        if (!response.is_error() && response.value().is_object()) {
                            replies->answered_blocking_event = true;
                            auto const& decision = response.value().as_object();
                            has_value = decision.get_bool("cancel"sv) == true || decision.has("redirectUrl"sv) || decision.has("responseHeaders"sv);
                        } else {
                            has_value = false;
                        }
                    }
                    if (has_value || --replies->remaining == 0) {
                        auto complete = move(replies->callback);
                        auto fallback = replies->answered_blocking_event ? "{}"_string : "null"_string;
                        complete(has_value || replies->answered_blocking_event, has_value ? result : fallback);
                    }
                });
                bind_event_reply(reply_id, target.page);
            }
            target.page.client().async_dispatch_extension_event(target.page.id(), extension_id, ns, event, args_json, reply_id, target.document_id);
        }
    };

    if (ext->manifest.background().has_value() && ext->manifest.background()->has_background() && !is_background_running(extension_id))
        ensure_background_started(extension_id, move(deliver));
    else
        deliver(true);
}

u64 ExtensionManager::register_event_reply(Function<void(bool, String const&)> callback)
{
    constexpr auto time_limit = AK::Duration::from_seconds(5);
    auto reply_id = m_next_reply_id++;
    auto timeout = Core::Timer::create_single_shot(time_limit, [this, reply_id] {
        if (auto pending = m_pending_event_replies.take(reply_id); pending.has_value())
            pending->callback(false, "null"_string);
    });
    m_pending_event_replies.set(reply_id, PendingEventReply { move(callback), {}, {}, timeout });
    timeout->start();
    return reply_id;
}

void ExtensionManager::bind_event_reply(Optional<u64> reply_id, WebContentPage& page)
{
    if (!reply_id.has_value())
        return;
    if (auto pending = m_pending_event_replies.get(*reply_id); pending.has_value()) {
        pending->renderer = page.client().make_weak_ptr<WebContentClient>();
        pending->page_id = page.id();
    }
}

void ExtensionManager::handle_event_reply(WebContentClient& renderer, Web::PageId page_id, u64 reply_id, bool has_value, String const& result_json)
{
    auto pending = m_pending_event_replies.get(reply_id);
    if (!pending.has_value() || pending->renderer.ptr() != &renderer || pending->page_id != page_id)
        return;
    auto reply = m_pending_event_replies.take(reply_id).release_value();
    reply.callback(has_value, result_json);
}

void ExtensionManager::handle_script_execution_finished(WebContentClient& renderer, Web::PageId page_id, u64 execution_id, bool success, String const& result_or_error_json)
{
    auto expected = m_pending_script_executions.get(execution_id);
    if (!expected.has_value() || expected->renderer.ptr() != &renderer || expected->target_page_id != page_id)
        return;
    auto pending = m_pending_script_executions.take(execution_id).release_value();
    auto group_entry = m_script_execution_groups.find(pending.group_id);
    if (group_entry == m_script_execution_groups.end())
        return;
    auto& group = group_entry->value;
    --group.remaining;
    auto caller = group.caller.strong_ref();

    // As in Firefox, the call fails with the first frame whose script fails, and otherwise has each frame's result.
    if (!success && !group.failed) {
        group.failed = true;
        if (caller)
            caller->async_extension_api_response(group.page_id, group.document_id, group.call_id, false, JsonValue(result_or_error_json).serialized());
    } else if (success && !group.failed) {
        auto parsed = JsonValue::from_string(result_or_error_json);
        auto result = parsed.is_error() ? JsonValue {} : parsed.release_value();
        if (group.expect_array_results) {
            JsonObject wrapped;
            wrapped.set("frameId"sv, pending.frame_id);
            wrapped.set("result"sv, move(result));
            group.results[pending.index] = move(wrapped);
        } else {
            group.results[pending.index] = move(result);
        }
    }
    if (group.remaining > 0)
        return;
    if (!group.failed && caller) {
        JsonArray results;
        for (auto& result : group.results)
            results.must_append(move(result));
        caller->async_extension_api_response(group.page_id, group.document_id, group.call_id, true, results.serialized());
    }
    m_script_execution_groups.remove(pending.group_id);
}

void ExtensionManager::handle_web_request_check(WebContentClient& caller, Web::PageId page_id, u64 check_id, Web::Extensions::WebRequestBeforeRequestDetails const& requested_details)
{
    auto* page = caller.page(page_id);
    if (!page || !page->is_open() || !page->has_traversable()) {
        caller.async_did_check_web_request_before_request(page_id, check_id, { .cancel = false });
        return;
    }
    auto details = requested_details;
    if (details.event == Web::Extensions::WebRequestEvent::BeforeRequest && details.url.scheme() == "moz-extension"sv) {
        auto owner = extension_by_uuid(details.url.serialized_host());
        if (!owner.has_value() || !owner->enabled) {
            caller.async_did_check_web_request_before_request(page_id, check_id, {});
            return;
        }
    }
    details.tab_id = tab_id_for_page(caller, page_id).value_or(0);
    details.initiator_origin.clear();
    details.document_url.clear();
    details.frame_id = 0;
    details.parent_frame_id = 0;
    auto document = details.source_document_id.has_value() ? page->hosted_document_with_id(*details.source_document_id) : nullptr;
    if (document && document->unload_counter() == 0) {
        details.initiator_origin = document->origin().serialize();
        details.document_url = document->creation_url().serialize();
        if (details.type != Web::Extensions::WebRequestResourceType::MainFrame) {
            page->for_each_navigable_with_a_hosted_active_document([&](CanonicalNavigable const& frame) {
                if (frame.active_document().id() != document->id())
                    return IterationDecision::Continue;
                details.frame_id = frame.extension_frame_id();
                if (auto parent = frame.parent())
                    details.parent_frame_id = parent->extension_frame_id();
                return IterationDecision::Break;
            });
        }
    } else if (details.type != Web::Extensions::WebRequestResourceType::MainFrame) {
        caller.async_did_check_web_request_before_request(page_id, check_id, {});
        return;
    }
    auto weak_caller = caller.make_weak_ptr<WebContentClient>();
    m_web_request_coordinator.evaluate_before_request(details, [weak_caller, page_id, check_id](Web::Extensions::WebRequestDecision decision) {
        if (auto client = weak_caller.strong_ref())
            client->async_did_check_web_request_before_request(page_id, check_id, decision);
    });
}

static Optional<ViewImplementation&> default_extension_target(ExtensionManager const& manager)
{
    if (auto active = Application::the().active_web_view(); active.has_value())
        return active;
    auto views = manager.ordinary_tab_views();
    if (views.is_empty())
        return {};
    return *views.first();
}

// The frame an injection names, or all_frames for each frame the extension may access.
static constexpr i64 all_frames = -1;

static ErrorOr<i64> requested_script_frame(JsonObject const& details)
{
    if (details.get_bool("allFrames"sv) == true)
        return details.has("frameId"sv) ? ErrorOr<i64> { Error::from_string_literal("Injection names both a frame and all frames") } : all_frames;
    if (auto target = details.get_object("target"sv); target.has_value()) {
        if (target->has("documentIds"sv))
            return Error::from_string_literal("Injection by document ID is unavailable");
        if (target->get_bool("allFrames"sv) == true)
            return target->has("frameIds"sv) ? ErrorOr<i64> { Error::from_string_literal("Injection names both frames and all frames") } : all_frames;
        if (target->has("frameIds"sv)) {
            auto ids = target->get_array("frameIds"sv);
            if (!ids.has_value() || ids->size() != 1)
                return Error::from_string_literal("Injection requires a single frame ID");
            auto id = ids->at(0).get_i64();
            if (!id.has_value() || *id < 0)
                return Error::from_string_literal("Invalid frame ID");
            return *id;
        }
    }
    auto frame_id = details.get_i64("frameId"sv);
    if (details.has("frameId"sv) && (!frame_id.has_value() || *frame_id < 0))
        return Error::from_string_literal("Invalid frame ID");
    return frame_id.value_or(0);
}

static CanonicalNavigable const* scripting_target_frame(ViewImplementation const& view, i64 frame_id, bool allow_pending_document = false)
{
    CanonicalNavigable const* result = nullptr;
    view.traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& frame) {
        if (frame.extension_frame_id() != frame_id)
            return IterationDecision::Continue;
        for (auto const* ancestor = &frame; ancestor; ancestor = ancestor->parent()) {
            if (ancestor->parent() && ancestor->container_document() != &ancestor->parent()->active_document())
                return IterationDecision::Break;
            if ((ancestor != &frame || !allow_pending_document)
                && (!ancestor->active_document().host() || ancestor->active_document().unload_counter()))
                return IterationDecision::Break;
        }
        result = &frame;
        return IterationDecision::Break;
    });
    return result;
}

// The frames an injection reaches: the one it names, or with all_frames, the top-level frame and each other frame the
// extension may access. The named or top-level frame must be one it may access.
static Vector<CanonicalNavigable const*> scripting_target_frames(ExtensionManager const& manager, StringView extension_id, ViewImplementation const& view, i64 frame_id)
{
    auto may_access = [&](CanonicalNavigable const* frame) {
        return frame && !frame->active_document().origin().is_opaque() && manager.has_host_permission(extension_id, frame->active_document_url(), view.view_id());
    };
    Vector<CanonicalNavigable const*> frames;
    auto const* first = scripting_target_frame(view, frame_id == all_frames ? 0 : frame_id);
    if (!may_access(first))
        return frames;
    frames.append(first);
    if (frame_id != all_frames)
        return frames;
    view.traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& frame) {
        if (frame.extension_frame_id() == 0)
            return IterationDecision::Continue;
        if (auto const* target = scripting_target_frame(view, frame.extension_frame_id()); may_access(target))
            frames.append(target);
        return IterationDecision::Continue;
    });
    return frames;
}

void ExtensionManager::handle_scripting_execute_script(
    WebContentClient& caller,
    Web::PageId page_id,
    u64 call_id,
    Web::HTML::CrossProcessId source_document_id,
    InstalledExtension const& ext,
    JsonObject const& details_obj,
    bool modern_api)
{
    auto send_err = [&](char const* msg) {
        caller.async_extension_api_response(page_id, source_document_id, call_id, false, JsonValue(MUST(String::from_utf8(StringView { msg, __builtin_strlen(msg) }))).serialized());
    };

    auto target_obj = details_obj.get_object("target"sv);
    auto target_tab_id = target_obj.has_value() ? target_obj->get_u64("tabId"sv) : Optional<u64> {};
    auto frame_id = requested_script_frame(details_obj);
    if (frame_id.is_error() || (details_obj.has("world"sv) && details_obj.get_string("world"sv) != "ISOLATED"sv)) {
        send_err("Unsupported script injection target or world");
        return;
    }

    auto target_view = target_tab_id.has_value() ? ViewImplementation::find_view_by_id(*target_tab_id) : default_extension_target(*this);
    if (!target_view.has_value() || !target_view->has_traversable() || !tab_info_for_extension(ext.extension_id, *target_view).has_value()) {
        send_err("Target tab not found");
        return;
    }

    auto frames = scripting_target_frames(*this, ext.extension_id, *target_view, frame_id.value());
    if (frames.is_empty()) {
        send_err("Extension lacks host/activeTab permission for target tab");
        return;
    }

    Vector<String> sources;
    if (auto files = details_obj.get_array("files"sv); files.has_value()) {
        for (auto const& f : files->values()) {
            if (!f.is_string())
                continue;
            auto rel = Web::Extensions::ExtensionManifest::normalize_package_relative_path(f.as_string());
            if (rel.is_error()) {
                send_err("Invalid script path");
                return;
            }
            auto content = read_package_script(ext.install_directory, rel.value());
            if (content.is_error()) {
                send_err("Referenced script file not found");
                return;
            }
            sources.append(content.release_value());
        }
    } else if (auto code = details_obj.get_string("code"sv); code.has_value()) {
        sources.append(*code);
    }

    Optional<String> func_source;
    Optional<String> func_args;
    if (auto func = details_obj.get_string("func_source"sv); func.has_value())
        func_source = *func;
    else if (auto func = details_obj.get_string("func"sv); func.has_value())
        func_source = *func;
    if (auto args = details_obj.get_array("args"sv); args.has_value())
        func_args = args->serialized();
    if (sources.is_empty() && !func_source.has_value()) {
        send_err("Script injection requires a function, code, or a package file");
        return;
    }

    auto group_id = m_next_execution_id++;
    m_script_execution_groups.set(group_id, ScriptExecutionGroup {
                                                .caller = caller.make_weak_ptr<WebContentClient>(),
                                                .page_id = page_id,
                                                .document_id = source_document_id,
                                                .call_id = call_id,
                                                .expect_array_results = modern_api,
                                                .remaining = frames.size(),
                                                .failed = false,
                                                .results = Vector<JsonValue> {},
                                            });
    m_script_execution_groups.get(group_id)->results.resize(frames.size());
    for (size_t index = 0; index < frames.size(); ++index) {
        auto& target_document = frames[index]->active_document();
        auto& target_page = *target_document.host();
        u64 exec_id = m_next_execution_id++;
        target_document.authorize_content_script(ext.extension_id, ext.grant_generation);
        m_pending_script_executions.set(exec_id, PendingScriptExecution {
                                                     .group_id = group_id,
                                                     .index = index,
                                                     .frame_id = frames[index]->extension_frame_id(),
                                                     .renderer = target_page.client().make_weak_ptr<WebContentClient>(),
                                                     .target_page_id = target_page.id(),
                                                 });
        send_content_script_metadata(target_page.client(), ext);
        target_page.client().async_execute_extension_script(
            target_page.id(),
            exec_id,
            ext.extension_id,
            ext.uuid,
            ext.grant_generation,
            sources,
            func_source,
            func_args,
            target_document.id());
    }
}

void ExtensionManager::handle_scripting_css(
    WebContentClient& caller,
    Web::PageId page_id,
    u64 call_id,
    Web::HTML::CrossProcessId source_document_id,
    InstalledExtension const& ext,
    JsonObject const& details_obj,
    bool remove)
{
    auto send_err = [&](char const* msg) {
        caller.async_extension_api_response(page_id, source_document_id, call_id, false, JsonValue(MUST(String::from_utf8(StringView { msg, __builtin_strlen(msg) }))).serialized());
    };

    auto target_obj = details_obj.get_object("target"sv);
    auto target_tab_id = target_obj.has_value() ? target_obj->get_u64("tabId"sv) : Optional<u64> {};
    auto frame_id = requested_script_frame(details_obj);
    if (frame_id.is_error()) {
        send_err("Unsupported CSS injection target or style origin");
        return;
    }
    auto target_view = target_tab_id.has_value() ? ViewImplementation::find_view_by_id(*target_tab_id) : default_extension_target(*this);
    if (!target_view.has_value() || !target_view->has_traversable() || !tab_info_for_extension(ext.extension_id, *target_view).has_value()) {
        send_err("Target tab not found");
        return;
    }

    Vector<CanonicalDocument*> target_documents;
    if (frame_id.value() == all_frames) {
        for (auto const* frame : scripting_target_frames(*this, ext.extension_id, *target_view, all_frames))
            target_documents.append(&frame->active_document());
        if (target_documents.is_empty()) {
            send_err("Extension lacks host/activeTab permission for target tab");
            return;
        }
    }

    bool allow_pending_document = !remove && ext.manifest.manifest_version() == Web::Extensions::ManifestVersion::V2
        && details_obj.get_string("runAt"sv) == "document_start"sv;
    auto const* frame = target_documents.is_empty() ? scripting_target_frame(*target_view, frame_id.value(), allow_pending_document) : nullptr;
    if (target_documents.is_empty() && !frame) {
        send_err("Extension lacks host/activeTab permission for target tab");
        return;
    }

    auto* target_document = frame ? &frame->active_document() : nullptr;
    auto const* target_url = frame ? &frame->active_document_url() : nullptr;
    if (frame && allow_pending_document) {
        // A document_start content script can ask the background page to style its document
        // while the frame still reports the document being replaced as active.
        if (auto navigation_id = frame->ongoing_navigation_id(); navigation_id.has_value()) {
            auto navigation = frame->navigation_with_id(*navigation_id);
            if (navigation && navigation->history_entry) {
                auto const& populated = navigation->history_entry->document_state->populated_document;
                if (populated.has_value() && populated->document->has_content_script_grant(ext.extension_id, ext.grant_generation)) {
                    target_document = populated->document.ptr();
                    target_url = &target_document->creation_url();
                }
            }
        }
    }
    if (frame) {
        auto const& target_page = target_document->host();
        if (!target_page || !target_page->is_open() || target_document->unload_counter() || target_document->origin().is_opaque()
            || !has_host_permission(ext.extension_id, *target_url, target_view->view_id())) {
            send_err("Extension lacks host/activeTab permission for target tab");
            return;
        }
        target_documents.append(target_document);
    }

    String css_text;
    String css_key = "inline"_string;
    if (auto css = details_obj.get_string("css"sv); css.has_value()) {
        css_text = *css;
        css_key = sha256_prefix_hex(css_text.bytes());
    } else if (auto code = details_obj.get_string("code"sv); code.has_value()) {
        css_text = *code;
        css_key = sha256_prefix_hex(css_text.bytes());
    } else if (auto files = details_obj.get_array("files"sv); files.has_value() && !files->is_empty()) {
        StringBuilder sb;
        for (auto const& f : files->values()) {
            if (!f.is_string())
                continue;
            auto rel = Web::Extensions::ExtensionManifest::normalize_package_relative_path(f.as_string());
            if (rel.is_error()) {
                send_err("Invalid CSS path");
                return;
            }
            if (css_key == "inline"sv)
                css_key = rel.value();
            if (!remove) {
                auto content = read_package_script(ext.install_directory, rel.value());
                if (content.is_error()) {
                    send_err("Referenced CSS file not found");
                    return;
                }
                sb.append(content.value());
                sb.append('\n');
            }
        }
        css_text = MUST(sb.to_string());
    }

    for (auto* document : target_documents) {
        if (auto const& target_page = document->host(); target_page && target_page->is_open())
            target_page->client().async_inject_extension_css(target_page->id(), ext.extension_id, css_key, css_text, remove, document->id());
    }
    caller.async_extension_api_response(page_id, source_document_id, call_id, true, "null"_string);
}

static JsonObject serialize_tab(Web::Extensions::ExtensionTabInfo const& t)
{
    JsonObject o;
    o.set("id"sv, static_cast<i64>(t.tab_id));
    o.set("windowId"sv, static_cast<i64>(t.window_id));
    o.set("active"sv, t.active);
    o.set("pinned"sv, t.pinned);
    o.set("status"sv, t.status);
    if (t.url.has_value())
        o.set("url"sv, *t.url);
    if (t.title.has_value())
        o.set("title"sv, *t.title);
    return o;
}

JsonObject ExtensionManager::extension_message_sender(WebContentClient& client, Web::PageId page_id, Web::HTML::CrossProcessId document_id, String const& extension_id) const
{
    JsonObject sender;
    sender.set("id"sv, extension_id);
    if (auto* page = client.page(page_id); page && page->has_traversable()) {
        // document_start scripts can connect before their document is activated in the frame.
        page->traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& frame) {
            auto document = frame.document_with_id(document_id);
            if (!document || document->host() != page || document->unload_counter() > 0)
                return IterationDecision::Continue;
            auto const& url = &frame.active_document() == document.ptr() ? frame.active_document_url() : document->creation_url();
            sender.set("url"sv, url.serialize());
            sender.set("origin"sv, document->origin().serialize());
            sender.set("documentId"sv, document->extension_document_id());
            sender.set("frameId"sv, frame.extension_frame_id());
            return IterationDecision::Break;
        });
    }
    if (auto tab_id = tab_id_for_page(client, page_id); tab_id.has_value()) {
        if (auto view = ViewImplementation::find_view_by_id(*tab_id); view.has_value()) {
            if (auto info = tab_info_for_extension(extension_id, *view); info.has_value())
                sender.set("tab"sv, serialize_tab(*info));
        }
    }
    return sender;
}

void ExtensionManager::send_runtime_port_event(RuntimePortEndpoint const& endpoint, String const& extension_id, StringView event, String const& port_id, Optional<String> const& payload)
{
    auto client = endpoint.renderer.strong_ref();
    auto* page = client ? client->page(endpoint.page_id) : nullptr;
    if (!page || !page->is_open() || !page->has_traversable())
        return;
    auto document = page->hosted_document_with_id(endpoint.document_id);
    if (!document || document->unload_counter() > 0)
        return;

    JsonArray arguments;
    arguments.must_append(port_id);
    if (payload.has_value())
        arguments.must_append(*payload);
    client->async_dispatch_extension_event(endpoint.page_id, extension_id, "runtime"_string, MUST(String::from_utf8(event)), arguments.serialized(), {}, endpoint.document_id);
}

void ExtensionManager::close_runtime_port(String const& id, Optional<Web::HTML::CrossProcessId> source_document, Optional<String> const& error)
{
    auto port = m_runtime_ports.take(id);
    if (!port.has_value())
        return;
    if (port->sender.document_id != source_document)
        send_runtime_port_event(port->sender, port->extension_id, "onPortDisconnect"sv, id, error);
    if (port->receiver.has_value() && port->receiver->document_id != source_document)
        send_runtime_port_event(*port->receiver, port->extension_id, "onPortDisconnect"sv, id, error);
}

bool ExtensionManager::handle_runtime_port_call(WebContentClient& caller, Web::PageId page_id, Web::HTML::CrossProcessId document_id, String const& extension_id, StringView method, JsonArray const& arguments)
{
    if (arguments.is_empty() || !arguments[0].is_string() || arguments[0].as_string().byte_count() != 36)
        return false;
    auto const& id = arguments[0].as_string();
    RuntimePortEndpoint source { caller.make_weak_ptr<WebContentClient>(), page_id, document_id };

    if (method == "connectNativePort"sv) {
        if (arguments.size() != 2 || !arguments[1].is_string() || m_runtime_ports.contains(id))
            return false;
        auto extension = extension_by_id(extension_id);
        auto error = extension.has_value() && extension->manifest.has_permission("nativeMessaging"sv)
            ? native_application_error(arguments[1].as_string(), extension_id)
            : "Extension lacks the nativeMessaging permission"_string;
        send_runtime_port_event(source, extension_id, "onPortDisconnect"sv, id, error);
        return true;
    }

    if (method == "connectPort"sv) {
        if (arguments.size() != 2 || !arguments[1].is_string() || arguments[1].as_string().byte_count() > 1024 || m_runtime_ports.contains(id))
            return false;
        if (m_runtime_ports.size() >= 1024 || !extension_by_id(extension_id).has_value()) {
            send_runtime_port_event(source, extension_id, "onPortDisconnect"sv, id, "No extension port is available"_string);
            return true;
        }
        m_runtime_ports.set(id, RuntimePort { extension_id, source, {}, {} });
        auto sender = extension_message_sender(caller, page_id, document_id, extension_id);
        auto deliver = [this, id, name = arguments[1].as_string(), sender = move(sender)](bool) mutable {
            auto port = m_runtime_ports.get(id);
            if (!port.has_value())
                return;

            WebContentPage* receiver_page = nullptr;
            Optional<Web::HTML::CrossProcessId> receiver_document;
            auto extension = m_extensions_by_id.get(port->extension_id);
            if (!extension.has_value() || !extension->enabled) {
                close_runtime_port(id, {}, "Extension is unavailable"_string);
                return;
            }
            WebContentClient::for_each_client([&](WebContentClient& client) {
                if (client.session().extension_manager.ptr() != this || client.hosted_extension_id() != extension->extension_id)
                    return IterationDecision::Continue;
                client.for_each_page([&](WebContentPage& page) {
                    if (!page.is_open() || !page.has_traversable())
                        return IterationDecision::Continue;
                    bool is_background = is_extension_background_view(page.view());
                    if (receiver_page && !is_background)
                        return IterationDecision::Continue;
                    page.for_each_navigable_with_a_hosted_active_document([&](CanonicalNavigable const& frame) {
                        auto const& document = frame.active_document();
                        if (document.id() == port->sender.document_id || document.extension_id() != extension->extension_id || document.unload_counter() > 0)
                            return IterationDecision::Continue;
                        receiver_page = &page;
                        receiver_document = document.id();
                        return IterationDecision::Break;
                    });
                    return receiver_page == &page && is_background ? IterationDecision::Break : IterationDecision::Continue;
                });
                return receiver_page && is_extension_background_view(receiver_page->view()) ? IterationDecision::Break : IterationDecision::Continue;
            });
            if (!receiver_page || !receiver_document.has_value()) {
                close_runtime_port(id, {}, "No receiving extension context"_string);
                return;
            }

            port->receiver = RuntimePortEndpoint { receiver_page->client().make_weak_ptr<WebContentClient>(), receiver_page->id(), *receiver_document };
            auto reply_id = register_event_reply([this, id](bool accepted, String const&) {
                auto port = m_runtime_ports.get(id);
                if (!port.has_value())
                    return;
                if (!accepted) {
                    close_runtime_port(id, {}, "No listener accepted the extension port"_string);
                    return;
                }
                port->connected = true;
                for (auto const& message : port->queued_messages)
                    send_runtime_port_event(*port->receiver, port->extension_id, "onPortMessage"sv, id, message);
                port->queued_messages.clear();
                port->queued_message_bytes = 0;
            });
            bind_event_reply(reply_id, *receiver_page);

            JsonArray event_arguments;
            event_arguments.must_append(id);
            event_arguments.must_append(name);
            event_arguments.must_append(move(sender));
            receiver_page->client().async_dispatch_extension_event(receiver_page->id(), extension->extension_id, "runtime"_string, "onConnect"_string, event_arguments.serialized(), reply_id, receiver_document);
        };
        auto ext = extension_by_id(extension_id);
        if (ext->manifest.background().has_value() && ext->manifest.background()->has_background())
            ensure_background_started(extension_id, move(deliver));
        else
            deliver(true);
        return true;
    }

    auto port = m_runtime_ports.get(id);
    if (!port.has_value() || port->extension_id != extension_id)
        return false;
    auto identifies_source = [&](RuntimePortEndpoint const& endpoint) {
        return endpoint.renderer.ptr() == &caller && endpoint.page_id == page_id && endpoint.document_id == document_id;
    };
    bool from_sender = identifies_source(port->sender);
    if (!from_sender && (!port->receiver.has_value() || !identifies_source(*port->receiver)))
        return false;
    if (method == "disconnectPort"sv && arguments.size() == 1) {
        close_runtime_port(id, document_id);
        return true;
    }
    if (method != "postPortMessage"sv || arguments.size() != 2 || !arguments[1].is_string() || arguments[1].as_string().byte_count() > 4 * MiB)
        return false;
    auto const& message = arguments[1].as_string();
    if (from_sender && !port->connected) {
        if (port->queued_messages.size() >= 128 || message.byte_count() > 4 * MiB - port->queued_message_bytes) {
            close_runtime_port(id, {}, "Extension port queue is full"_string);
            return true;
        }
        port->queued_messages.append(message);
        port->queued_message_bytes += message.byte_count();
        return true;
    }
    send_runtime_port_event(from_sender ? *port->receiver : port->sender, extension_id, "onPortMessage"sv, id, message);
    return true;
}

void ExtensionManager::handle_api_call(
    WebContentClient& caller,
    Web::PageId page_id,
    u64 call_id,
    String const& extension_id,
    Web::Extensions::ContextType context_type,
    Web::HTML::CrossProcessId document_id,
    u64 grant_generation,
    String const& namespace_name,
    String const& method_name,
    String const& args_json)
{
    auto reply_ok = [&](JsonValue const& result) {
        caller.async_extension_api_response(page_id, document_id, call_id, true, result.serialized());
    };
    auto reply_err = [&](char const* err) {
        caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::from_utf8(StringView { err, __builtin_strlen(err) }))).serialized());
    };

    if (caller.session().is_private() == IsPrivate::Yes || !m_extensions_enabled) {
        reply_err("Extensions are unavailable in this session");
        return;
    }

    if (context_type != Web::Extensions::ContextType::ContentScript && caller.hosted_extension_id() != extension_id) {
        reply_err("Renderer process is not authorized to speak for this extension");
        return;
    }

    auto ext = m_extensions_by_id.get(extension_id);
    if (!ext.has_value() || !ext->enabled) {
        reply_err("Extension is disabled or not installed");
        return;
    }

    auto* page = caller.page(page_id);
    auto document = page && page->is_open() && page->has_traversable() ? page->hosted_document_with_id(document_id) : nullptr;
    if (!document || document->unload_counter() > 0) {
        reply_err("Extension document is no longer available");
        return;
    }
    if (context_type == Web::Extensions::ContextType::ContentScript) {
        if (grant_generation != ext->grant_generation || !document->has_content_script_grant(extension_id, grant_generation)) {
            reply_err("Document has no current content script grant");
            return;
        }
        bool storage_access = namespace_name.is_one_of("storage.local"sv, "storage.sync"sv, "storage.managed"sv)
            || (namespace_name == "storage.session"sv && method_name != "setAccessLevel"sv && m_content_script_session_storage_access.contains(extension_id));
        bool runtime_access = namespace_name == "runtime"sv && method_name.bytes_as_string_view().is_one_of("sendMessage"sv, "connectPort"sv, "postPortMessage"sv, "disconnectPort"sv);
        if (!runtime_access && !storage_access) {
            reply_err("API is unavailable from content scripts");
            return;
        }
    } else if (document->extension_id() != extension_id || document->origin().is_opaque()) {
        reply_err("Document is not owned by this extension");
        return;
    }

    schedule_event_page_idle_timer(extension_id);

    if (args_json.bytes().size() > 8 * MiB) {
        reply_err("Extension API payload exceeds its size limit");
        return;
    }
    auto caller_tab = [&]() -> ViewImplementation* {
        auto& view = page->view();
        if (view.owner_view_id().has_value()) {
            for (auto* tab : ordinary_tab_views()) {
                if (tab->extension_window_id() == view.extension_window_id() && tab->is_current_tab_of_window())
                    return tab;
            }
            return nullptr;
        }
        if (!is_extension_background_view(view))
            return &view;
        auto active = Application::the().active_web_view();
        if (active.has_value() && active->is_private() == IsPrivate::No)
            return &*active;
        for (auto* tab : ordinary_tab_views()) {
            if (tab->is_current_tab_of_window())
                return tab;
        }
        return nullptr;
    };
    auto caller_window = [&] {
        auto* tab = caller_tab();
        return tab ? tab->extension_window_id() : current_window_id();
    };

    if (namespace_name == "declarativeNetRequest"sv && args_json.byte_count() > 4 * MiB) {
        reply_err("Declarative request arguments exceed their size limit");
        return;
    }
    auto parsed_args = JsonValue::from_string(args_json);
    JsonArray empty_arr;
    JsonObject empty_obj;

    if (namespace_name == "dns"sv) {
        if (method_name != "resolve"sv || !ext->manifest.has_permission("dns"sv)) {
            reply_err("Extension lacks the dns permission or called an unknown DNS method");
            return;
        }
        auto const& arguments = !parsed_args.is_error() && parsed_args.value().is_array() ? parsed_args.value().as_array() : empty_arr;
        if (arguments.is_empty() || arguments.size() > 2 || !arguments[0].is_string()) {
            reply_err("dns.resolve requires a hostname and optional flags");
            return;
        }
        auto hostname = arguments[0].as_string().bytes_as_string_view();
        auto host = URL::Parser::parse_host(hostname);
        if (!host.has_value()) {
            if (auto ipv6 = IPv6Address::from_string(hostname); ipv6.has_value())
                host = URL::Host { URL::Host::VariantType { *ipv6 } };
        }
        if (!host.has_value() || host->is_empty_host() || hostname.length() > 253
            || hostname.starts_with('.') || hostname.contains(".."sv) || hostname.contains('*') || hostname.contains('%')) {
            reply_err("Invalid DNS hostname");
            return;
        }

        using Requests::HostLookupFlag;
        auto flags = HostLookupFlag::IncludeLocalIPv6;
        if (arguments.size() == 2 && !arguments[1].is_null()) {
            if (!arguments[1].is_array() || arguments[1].as_array().size() > 10) {
                reply_err("Invalid DNS options");
                return;
            }
            for (auto const& option : arguments[1].as_array().values()) {
                auto name = option.is_string() ? option.as_string().bytes_as_string_view() : StringView {};
                if (name == "canonical_name"sv)
                    flags |= HostLookupFlag::CanonicalName;
                else if (name == "disable_ipv4"sv)
                    flags |= HostLookupFlag::DisableIPv4;
                else if (name == "disable_ipv6"sv)
                    flags |= HostLookupFlag::DisableIPv6;
                else if (name == "bypass_cache"sv)
                    flags |= HostLookupFlag::BypassCache;
                else if (name == "offline"sv)
                    flags |= HostLookupFlag::CacheOnly;
                else if (name == "allow_name_collisions"sv)
                    flags |= HostLookupFlag::AllowNameCollisions;
                else if (name != "disable_trr"sv) {
                    // Ladyflock has no TRR resolver. Its resolver does not expose speculative or priority requests.
                    reply_err("Unsupported DNS option");
                    return;
                }
            }
        }

        auto request_server = RequestServerManager::the().browser_instance(caller.session());
        if (request_server.is_error()) {
            reply_err("DNS service is unavailable");
            return;
        }
        request_server.value()->ui_client().resolve_host(host->serialize().to_byte_string(), flags,
            [weak = caller.make_weak_ptr<WebContentClient>(), page_id, document_id, call_id](Vector<ByteString> addresses, Optional<ByteString> canonical_name) {
                auto renderer = weak.strong_ref();
                if (!renderer)
                    return;
                if (addresses.is_empty()) {
                    renderer->async_extension_api_response(page_id, document_id, call_id, false, "\"DNS lookup failed\""_string);
                    return;
                }
                JsonArray address_list;
                for (auto const& address : addresses)
                    address_list.must_append(MUST(String::from_byte_string(address)));
                JsonObject record;
                record.set("addresses"sv, move(address_list));
                record.set("isTRR"sv, false);
                if (canonical_name.has_value()) {
                    if (auto name = String::from_byte_string(*canonical_name); !name.is_error())
                        record.set("canonicalName"sv, name.release_value());
                }
                renderer->async_extension_api_response(page_id, document_id, call_id, true, record.serialized());
            });
        return;
    }

    if (namespace_name == "runtime"sv) {
        if (method_name.bytes_as_string_view().is_one_of("connectPort"sv, "connectNativePort"sv, "postPortMessage"sv, "disconnectPort"sv)) {
            if (!parsed_args.is_error() && parsed_args.value().is_array())
                handle_runtime_port_call(caller, page_id, document_id, extension_id, method_name, parsed_args.value().as_array());
            return;
        }
        if (method_name == "sendMessage"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty()) {
                reply_err("runtime.sendMessage requires a message argument");
                return;
            }
            auto msg = parsed_args.value().as_array().at(0);
            JsonArray ev_args;
            ev_args.must_append(msg);
            ev_args.must_append(extension_message_sender(caller, page_id, document_id, extension_id));

            auto weak_caller = caller.make_weak_ptr<WebContentClient>();
            dispatch_event_to_extension(ext->extension_id, "runtime"sv, "onMessage"sv, ev_args.serialized(), [weak_caller, page_id, document_id, call_id](bool has_val, String const& res_json) {
                if (auto c = weak_caller.strong_ref())
                    c->async_extension_api_response(page_id, document_id, call_id, true, has_val ? res_json : "null"_string); }, document_id);
            return;
        }
        if (method_name == "openOptionsPage"sv) {
            if (!ext->manifest.options_ui().has_value()) {
                reply_err("Extension declares no options_ui page");
                return;
            }
            auto opt_url = package_page_url(*ext, ext->manifest.options_ui()->page);
            auto* source = caller_tab();
            if (!opt_url.has_value() || !source) {
                reply_err("No ordinary browser tab is available to open the options page");
                return;
            }
            Application::the().open_url_in_new_tab(*opt_url, Web::HTML::ActivateTab::Yes, source);
            reply_ok(JsonValue {});
            return;
        }
        if (method_name == "sendNativeMessage"sv) {
            auto const& arguments = !parsed_args.is_error() && parsed_args.value().is_array() ? parsed_args.value().as_array() : empty_arr;
            if (!ext->manifest.has_permission("nativeMessaging"sv)) {
                reply_err("Extension lacks the nativeMessaging permission");
                return;
            }
            if (arguments.size() != 2 || !arguments[0].is_string()) {
                reply_err("runtime.sendNativeMessage requires an application name and a message");
                return;
            }
            auto error = native_application_error(arguments[0].as_string(), extension_id);
            caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(error).serialized());
            return;
        }
        if (method_name == "setUninstallURL"sv) {
            // As in Firefox, an empty or missing URL clears it, and only an http or https URL may be opened.
            auto const& arguments = !parsed_args.is_error() && parsed_args.value().is_array() ? parsed_args.value().as_array() : empty_arr;
            if (arguments.is_empty() || arguments[0].is_null() || (arguments[0].is_string() && arguments[0].as_string().is_empty())) {
                m_uninstall_urls.remove(extension_id);
                reply_ok(JsonValue {});
                return;
            }
            auto url = arguments[0].is_string() && arguments[0].as_string().byte_count() <= 1023 ? URL::Parser::basic_parse(arguments[0].as_string()) : Optional<URL::URL> {};
            if (!url.has_value() || !url->scheme().is_one_of("http"sv, "https"sv)) {
                reply_err("Invalid URL scheme: only http and https URLs can be uninstall URLs");
                return;
            }
            m_uninstall_urls.set(extension_id, url.release_value());
            reply_ok(JsonValue {});
            return;
        }
        if (method_name == "reload"sv) {
            // The reply goes out before the extension's pages close, as they would be gone when it arrived.
            reply_ok(JsonValue {});
            Core::deferred_invoke([weak = make_weak_ptr<ExtensionManager>(), extension_id] {
                auto* self = weak.ptr();
                if (!self || !self->m_extensions_by_id.contains(extension_id))
                    return;
                if (self->set_extension_enabled(extension_id, false).is_error())
                    return;
                (void)self->set_extension_enabled(extension_id, true);
            });
            return;
        }
        reply_err("Unsupported runtime API method");
        return;
    }

    if (namespace_name.is_one_of("storage"sv, "storage.local"sv, "storage.session"sv, "storage.sync"sv, "storage.managed"sv)) {
        if (!ext->manifest.has_permission("storage"sv)) {
            reply_err("Extension lacks 'storage' permission");
            return;
        }
        // Firefox reads managed storage from an enterprise policy or a native manifest. Ladyflock has neither for
        // extensions, so it answers as Firefox does when no managed storage manifest is installed.
        if (namespace_name == "storage.managed"sv) {
            if (method_name.is_one_of("get"sv, "getKeys"sv, "getBytesInUse"sv))
                reply_err("Managed storage manifest not found");
            else
                reply_err("storage.managed is read-only");
            return;
        }
        bool is_session_storage = namespace_name == "storage.session"sv;
        bool is_sync_storage = namespace_name == "storage.sync"sv;
        auto& storage = is_session_storage ? *m_session_storage : is_sync_storage ? *m_sync_storage
                                                                                  : *m_storage;
        auto area_name = is_session_storage ? "session"_string : is_sync_storage ? "sync"_string
                                                                                 : "local"_string;
        auto normalized_method = method_name.bytes_as_string_view();
        if (normalized_method.starts_with("local."sv))
            normalized_method = normalized_method.substring_view(6);

        if (is_session_storage && normalized_method == "setAccessLevel"sv) {
            if (context_type == Web::Extensions::ContextType::ContentScript || parsed_args.is_error() || !parsed_args.value().is_array()
                || parsed_args.value().as_array().size() != 1 || !parsed_args.value().as_array().at(0).is_object()) {
                reply_err("Expected a storage session access level from an extension page");
                return;
            }
            auto level = parsed_args.value().as_array().at(0).as_object().get_string("accessLevel"sv);
            if (level == "TRUSTED_CONTEXTS"sv)
                m_content_script_session_storage_access.remove(extension_id);
            else if (level == "TRUSTED_AND_UNTRUSTED_CONTEXTS"sv)
                m_content_script_session_storage_access.set(extension_id);
            else {
                reply_err("Invalid storage session access level");
                return;
            }
            reply_ok(JsonValue {});
            return;
        }

        auto notify_storage_change = [&](JsonObject changes) {
            if (changes.is_empty())
                return;
            JsonArray ev_args;
            ev_args.must_append(move(changes));
            ev_args.must_append(area_name);
            dispatch_event_to_extension(ext->extension_id, "storage"sv, "onChanged"sv, ev_args.serialized());
        };

        if (normalized_method == "get"sv) {
            JsonValue keys_spec;
            if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty())
                keys_spec = parsed_args.value().as_array().at(0);
            reply_ok(JsonValue(storage.get(extension_id, keys_spec)));
            return;
        }
        if (normalized_method == "getKeys"sv) {
            JsonArray keys;
            storage.get(extension_id, {}).for_each_member([&](String const& key, auto const&) { keys.must_append(key); });
            reply_ok(move(keys));
            return;
        }
        if (normalized_method == "set"sv) {
            JsonObject items;
            if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty() && parsed_args.value().as_array().at(0).is_object())
                items = parsed_args.value().as_array().at(0).as_object();
            auto quota = is_sync_storage                                                    ? ExtensionStorage::Quota::Sync
                : !is_session_storage && ext->manifest.has_permission("unlimitedStorage"sv) ? ExtensionStorage::Quota::Unlimited
                                                                                            : ExtensionStorage::Quota::Limited;
            auto changes = storage.set(extension_id, items, quota);
            if (changes.is_error()) {
                caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::formatted("{}", changes.error()))).serialized());
                return;
            }
            notify_storage_change(changes.release_value());
            reply_ok(JsonValue {});
            return;
        }
        if (normalized_method == "remove"sv) {
            Vector<String> keys;
            if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty()) {
                auto arg = parsed_args.value().as_array().at(0);
                if (arg.is_string())
                    keys.append(arg.as_string());
                else if (arg.is_array()) {
                    for (auto const& k : arg.as_array().values()) {
                        if (k.is_string())
                            keys.append(k.as_string());
                    }
                }
            }
            notify_storage_change(storage.remove(extension_id, keys));
            reply_ok(JsonValue {});
            return;
        }
        if (normalized_method == "clear"sv) {
            notify_storage_change(storage.clear(extension_id));
            reply_ok(JsonValue {});
            return;
        }
        if (normalized_method == "getBytesInUse"sv) {
            JsonValue keys_spec;
            if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty())
                keys_spec = parsed_args.value().as_array().at(0);
            auto size = storage.get_bytes_in_use(extension_id, keys_spec);
            if (size.is_error()) {
                caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::formatted("{}", size.error()))).serialized());
                return;
            }
            reply_ok(JsonValue(size.release_value()));
            return;
        }
        reply_err("Unsupported storage method");
        return;
    }

    // Content scripts may only access runtime and storage!
    if (context_type == Web::Extensions::ContextType::ContentScript) {
        reply_err("API not available in content scripts");
        return;
    }

    if (namespace_name == "commands"sv && method_name == "getAll"sv) {
        auto manifest = MUST(JsonValue::from_string(ext->manifest.raw_json()));
        JsonArray result;
        if (auto commands = manifest.as_object().get_object("commands"sv); commands.has_value()) {
            commands->for_each_member([&](String const& name, JsonValue const& options) {
                JsonObject command;
                command.set("name"sv, name);
                command.set("description"sv, options.as_object().get_string("description"sv).value_or(String {}));
                command.set("shortcut"sv, ""sv);
                result.must_append(move(command));
            });
        }
        reply_ok(move(result));
        return;
    }

    if (namespace_name == "downloads"sv) {
        if (!ext->manifest.has_permission("downloads"sv)) {
            reply_err("Extension lacks the downloads permission");
            return;
        }
        ensure_download_events();
        auto const& arguments = !parsed_args.is_error() && parsed_args.value().is_array() ? parsed_args.value().as_array() : empty_arr;
        auto& downloader = Application::the().file_downloader();
        auto download_id_argument = [&]() -> Optional<u64> {
            if (arguments.is_empty())
                return {};
            auto id = arguments[0].get_u64();
            if (!id.has_value() || !is_extension_visible_download(*id))
                return {};
            return id;
        };

        if (method_name == "download"sv) {
            if (arguments.is_empty() || !arguments[0].is_object()) {
                reply_err("downloads.download requires options");
                return;
            }
            auto const& options = arguments[0].as_object();
            auto url = options.get_string("url"sv).map([](auto const& url) { return URL::Parser::basic_parse(url); }).value_or({});
            if (!url.has_value()) {
                reply_err("Invalid download URL");
                return;
            }
            // A download request goes out as the browser's own GET, as Ladyflock downloads do.
            if (auto method = options.get_string("method"sv); (method.has_value() && *method != "GET"sv) || options.has("body"sv) || options.has("headers"sv)) {
                reply_err("Only plain GET downloads are supported");
                return;
            }
            Optional<Web::FileAPI::SerializedBlobURLEntry::Blob> blob;
            if (url->scheme() == "blob"sv) {
                // Only a blob the extension itself made, as in Firefox.
                auto entry = caller.session().blob_url_store ? caller.session().blob_url_store->resolve(Utf16String::from_utf8(url->serialize()), {}) : Optional<Web::FileAPI::SerializedBlobURLEntry> {};
                auto extension_origin = URL::Parser::basic_parse(ByteString::formatted("moz-extension://{}/", ext->uuid)).map([](auto const& page) { return page.origin(); });
                if (!entry.has_value() || !extension_origin.has_value() || !entry->origin.is_same_origin(*extension_origin) || !entry->object.has<Web::FileAPI::SerializedBlobURLEntry::Blob>()) {
                    reply_err("The blob URL is not one this extension made");
                    return;
                }
                blob = entry->object.get<Web::FileAPI::SerializedBlobURLEntry::Blob>();
            } else if (!url->scheme().is_one_of("http"sv, "https"sv)) {
                reply_err("Downloads need an http, https or blob URL");
                return;
            }
            auto filename = options.get_string("filename"sv).map([](auto const& name) { return String { name }; });
            auto destination = extension_download_destination(filename, *url, options.get_string("conflictAction"sv).value_or("uniquify"_string));
            if (destination.is_error()) {
                caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::formatted("{}", destination.error()))).serialized());
                return;
            }
            auto replace = options.get_string("conflictAction"sv) == "overwrite"sv ? FileDownloader::MayReplaceDestination::Yes : FileDownloader::MayReplaceDestination::No;
            u64 id;
            if (blob.has_value()) {
                id = downloader.start_download(FileDownloader::Owner { IsPrivate::No }, *url, destination.release_value(), blob->data.size(), replace);
                m_extension_downloads.set(id, extension_id);
                downloader.append_download_data(id, blob->data.bytes());
                downloader.finish_download(id);
            } else {
                id = downloader.download_file(FileDownloader::Owner { IsPrivate::No }, *url, destination.release_value(), replace);
                m_extension_downloads.set(id, extension_id);
            }
            reply_ok(JsonValue(id));
            return;
        }
        if (method_name == "search"sv) {
            auto query = !arguments.is_empty() && arguments[0].is_object() ? arguments[0].as_object() : JsonObject {};
            JsonArray items;
            Vector<FileDownloader::Download const*> matches;
            for (auto const& download : downloader.downloads()) {
                if (!is_extension_visible_download(download.id))
                    continue;
                auto item = serialize_extension_download(download);
                bool matches_query = true;
                query.for_each_member([&](String const& key, JsonValue const& value) {
                    if (key.is_one_of("limit"sv, "orderBy"sv, "query"sv) || !matches_query)
                        return;
                    if (auto field = item.get(key); !field.has_value() || field->serialized() != value.serialized())
                        matches_query = false;
                });
                if (matches_query)
                    matches.append(&download);
            }
            // The newest first when asked, as most extensions do with orderBy: ["-startTime"].
            auto order = query.get_array("orderBy"sv);
            if (order.has_value() && !order->is_empty() && order->at(0).is_string() && order->at(0).as_string() == "-startTime"sv)
                matches.reverse();
            auto limit = query.get_u32("limit"sv).value_or(0);
            for (auto const* download : matches) {
                if (limit != 0 && items.size() >= limit)
                    break;
                items.must_append(serialize_extension_download(*download));
            }
            reply_ok(move(items));
            return;
        }
        if (method_name.is_one_of("cancel"sv, "pause"sv, "resume"sv)) {
            auto id = download_id_argument();
            auto download = id.has_value() ? downloader.download(*id) : Optional<FileDownloader::Download const&> {};
            if (!download.has_value()) {
                reply_err("Invalid download id");
                return;
            }
            if (method_name == "cancel"sv) {
                if (FileDownloader::status_is_active(download->status))
                    downloader.cancel_download(*id);
            } else if (method_name == "pause"sv) {
                if (download->status != FileDownloader::DownloadStatus::InProgress) {
                    reply_err("Download is not in progress");
                    return;
                }
                downloader.pause_download(*id);
            } else {
                if (download->status != FileDownloader::DownloadStatus::Paused) {
                    reply_err("Download is not paused");
                    return;
                }
                downloader.resume_download(*id);
            }
            reply_ok(JsonValue {});
            return;
        }
        reply_err("Unsupported downloads method");
        return;
    }

    if (namespace_name == "cookies"sv) {
        if (!ext->manifest.has_permission("cookies"sv)) {
            reply_err("Extension lacks the cookies permission");
            return;
        }
        auto const& arguments = !parsed_args.is_error() && parsed_args.value().is_array() ? parsed_args.value().as_array() : empty_arr;
        auto& jar = *caller.session().cookie_jar;
        if (method_name == "getAllCookieStores"sv) {
            JsonArray tab_ids;
            for (auto* view : ordinary_tab_views())
                tab_ids.must_append(view->view_id());
            JsonObject store;
            store.set("id"sv, cookie_store_id);
            store.set("incognito"sv, false);
            store.set("tabIds"sv, move(tab_ids));
            JsonArray stores;
            stores.must_append(move(store));
            reply_ok(move(stores));
            return;
        }
        if (arguments.is_empty() || !arguments[0].is_object()) {
            reply_err("The cookies API needs a details object");
            return;
        }
        auto const& details = arguments[0].as_object();
        if (auto store = details.get_string("storeId"sv); store.has_value() && *store != cookie_store_id) {
            reply_err("Invalid cookie store id");
            return;
        }
        auto details_url = details.get_string("url"sv).map([](auto const& url) { return URL::Parser::basic_parse(url); }).value_or({});
        auto may_use = [&](HTTP::Cookie::Cookie const& cookie) { return extension_may_use_cookie(*ext, cookie); };

        // The cookie with the longest path among those the URL would send under the name.
        auto best_match = [&](URL::URL const& url, StringView name) -> Optional<HTTP::Cookie::Cookie> {
            auto host = HTTP::Cookie::canonicalize_domain(url);
            if (!host.has_value())
                return {};
            Optional<HTTP::Cookie::Cookie> best;
            for (auto& cookie : jar.get_all_cookies()) {
                if (cookie.name != name || !cookie.partition_key.is_empty() || !may_use(cookie))
                    continue;
                if (!HTTP::Cookie::cookie_matches_url(cookie, url, *host, HTTP::Cookie::Source::Http, HTTP::Cookie::SameSiteContext::same_site()))
                    continue;
                if (!best.has_value() || cookie.path.byte_count() > best->path.byte_count())
                    best = move(cookie);
            }
            return best;
        };
        auto url_is_allowed = [&] {
            return details_url.has_value() && details_url->scheme().is_one_of("http"sv, "https"sv) && has_host_permission(extension_id, *details_url);
        };

        if (method_name == "get"sv || method_name == "remove"sv) {
            auto name = details.get_string("name"sv);
            if (!name.has_value() || !details_url.has_value()) {
                reply_err("The details need a url and a name");
                return;
            }
            if (!url_is_allowed()) {
                reply_err("Permission denied to access cookies for this URL");
                return;
            }
            auto cookie = best_match(*details_url, *name);
            if (method_name == "get"sv) {
                reply_ok(cookie.has_value() ? JsonValue(serialize_extension_cookie(*cookie)) : JsonValue {});
                return;
            }
            if (!cookie.has_value()) {
                reply_ok(JsonValue {});
                return;
            }
            jar.delete_cookie({ cookie->name, cookie->domain, cookie->host_only, cookie->path, cookie->partition_key });
            JsonObject removed;
            removed.set("url"sv, details_url->serialize());
            removed.set("name"sv, *name);
            removed.set("storeId"sv, cookie_store_id);
            removed.set("firstPartyDomain"sv, ""sv);
            reply_ok(move(removed));
            return;
        }
        if (method_name == "getAll"sv) {
            Optional<String> url_host;
            if (details_url.has_value())
                url_host = HTTP::Cookie::canonicalize_domain(*details_url);
            auto domain = details.get_string("domain"sv).map([](auto const& domain) { return MUST(domain.to_ascii_lowercase().trim("."sv, TrimMode::Left)); });
            JsonArray cookies;
            for (auto const& cookie : jar.get_all_cookies()) {
                // As in Firefox, getAll returns the cookies of partitioned storage only when asked for a partition.
                if (!cookie.partition_key.is_empty() || !may_use(cookie))
                    continue;
                if (details_url.has_value() && (!url_host.has_value() || !HTTP::Cookie::cookie_matches_url(cookie, *details_url, *url_host, HTTP::Cookie::Source::Http, HTTP::Cookie::SameSiteContext::same_site())))
                    continue;
                if (domain.has_value() && !HTTP::Cookie::domain_matches(cookie.domain, *domain))
                    continue;
                if (auto name = details.get_string("name"sv); name.has_value() && cookie.name != *name)
                    continue;
                if (auto path = details.get_string("path"sv); path.has_value() && cookie.path != *path)
                    continue;
                if (auto secure = details.get_bool("secure"sv); secure.has_value() && cookie.secure != *secure)
                    continue;
                if (auto session = details.get_bool("session"sv); session.has_value() && cookie.persistent == *session)
                    continue;
                cookies.must_append(serialize_extension_cookie(cookie));
            }
            reply_ok(move(cookies));
            return;
        }
        if (method_name == "set"sv) {
            if (!details_url.has_value() || !url_is_allowed()) {
                reply_err("Permission denied to set cookies for this URL");
                return;
            }
            auto host = HTTP::Cookie::canonicalize_domain(*details_url);
            if (!host.has_value()) {
                reply_err("Invalid cookie URL");
                return;
            }
            HTTP::Cookie::Cookie cookie;
            cookie.name = details.get_string("name"sv).value_or({});
            cookie.value = details.get_string("value"sv).value_or({});
            cookie.secure = details.get_bool("secure"sv).value_or(false);
            cookie.http_only = details.get_bool("httpOnly"sv).value_or(false);
            cookie.path = details.get_string("path"sv).value_or(HTTP::Cookie::default_path(*details_url));
            if (auto domain = details.get_string("domain"sv); domain.has_value() && !domain->is_empty()) {
                cookie.domain = MUST(domain->to_ascii_lowercase().trim("."sv, TrimMode::Left));
                cookie.host_only = false;
            } else {
                cookie.domain = *host;
                cookie.host_only = true;
            }
            // An unset sameSite leaves the browser's default. SameSite=None needs Secure, so on a cookie that isn't
            // secure no_restriction also leaves the default, which sends the cookie as Firefox would.
            auto same_site = details.get_string("sameSite"sv);
            if (same_site == "strict"sv)
                cookie.same_site = HTTP::Cookie::SameSite::Strict;
            else if (same_site == "lax"sv)
                cookie.same_site = HTTP::Cookie::SameSite::Lax;
            else if (same_site == "no_restriction"sv && cookie.secure)
                cookie.same_site = HTTP::Cookie::SameSite::None;
            else
                cookie.same_site = HTTP::Cookie::SameSite::Default;
            auto now = UnixDateTime::now();
            cookie.creation_time = now;
            cookie.last_access_time = now;
            if (auto expiration = details.get_double_with_precision_loss("expirationDate"sv); expiration.has_value()) {
                cookie.persistent = true;
                cookie.expiry_time = UnixDateTime::from_milliseconds_since_epoch(static_cast<i64>(*expiration * 1000));
            } else {
                cookie.expiry_time = UnixDateTime::from_seconds_since_epoch(NumericLimits<i32>::max());
            }
            if (!may_use(cookie) || !HTTP::Cookie::domain_matches(*host, cookie.domain)) {
                reply_err("Permission denied to set this cookie's domain");
                return;
            }
            if (auto result = jar.set_cookie_from_devtools(*details_url, {}, cookie); result.is_error()) {
                reply_err("The cookie could not be set");
                return;
            }
            auto stored = best_match(*details_url, cookie.name);
            reply_ok(stored.has_value() ? JsonValue(serialize_extension_cookie(*stored)) : JsonValue {});
            return;
        }
        reply_err("Unsupported cookies method");
        return;
    }

    if (namespace_name == "notifications"sv) {
        if (!ext->manifest.has_permission("notifications"sv)) {
            reply_err("Extension lacks the notifications permission");
            return;
        }
        auto const& arguments = !parsed_args.is_error() && parsed_args.value().is_array() ? parsed_args.value().as_array() : empty_arr;
        auto& notifications = m_notifications.ensure(extension_id);
        if (method_name == "getAll"sv) {
            JsonObject all;
            for (auto const& [id, notification] : notifications)
                all.set(id, notification.options);
            reply_ok(move(all));
            return;
        }
        if (method_name == "clear"sv) {
            auto id = arguments.is_empty() || !arguments[0].is_string() ? Optional<String> {} : Optional<String> { arguments[0].as_string() };
            auto notification = id.has_value() ? notifications.take(*id) : Optional<ExtensionNotification> {};
            if (notification.has_value())
                NotificationManager::the().close_for_extension(notification->platform_id);
            reply_ok(JsonValue(notification.has_value()));
            return;
        }
        if (method_name != "create"sv) {
            reply_err("Unsupported notifications method");
            return;
        }
        // create([id], options): Firefox shows every template as a basic notification.
        size_t options_index = !arguments.is_empty() && (arguments[0].is_string() || arguments[0].is_null()) ? 1 : 0;
        if (options_index >= arguments.size() || !arguments[options_index].is_object()) {
            reply_err("notifications.create requires notification options");
            return;
        }
        auto const& options = arguments[options_index].as_object();
        auto type = options.get_string("type"sv);
        auto title = options.get_string("title"sv);
        auto message = options.get_string("message"sv);
        if (!type.has_value() || !type->is_one_of("basic"sv, "image"sv, "list"sv, "progress"sv) || !title.has_value() || !message.has_value()) {
            reply_err("Notification options need a type, title and message");
            return;
        }
        String id;
        if (options_index == 1 && arguments[0].is_string() && !arguments[0].as_string().is_empty())
            id = arguments[0].as_string();
        else
            id = MUST(String::formatted("ladyflock-notification-{}", m_next_notification_number++));
        if (auto previous = notifications.take(id); previous.has_value())
            NotificationManager::the().close_for_extension(previous->platform_id);

        Web::NotificationsAPI::NotificationData data;
        data.title = Utf16String::from_utf8(*title);
        data.body = Utf16String::from_utf8(*message);
        data.require_interaction = options.get_bool("requireInteraction"sv).value_or(false);
        data.silent = options.get_bool("silent"sv);

        auto deliver = [weak = make_weak_ptr<ExtensionManager>(), extension_id, id](StringView event, Optional<bool> by_user = {}) {
            auto* self = weak.ptr();
            if (!self)
                return;
            JsonArray event_arguments;
            event_arguments.must_append(id);
            if (by_user.has_value()) {
                event_arguments.must_append(*by_user);
                if (auto live = self->m_notifications.get(extension_id); live.has_value())
                    live->remove(id);
            }
            self->dispatch_event_to_extension(extension_id, "notifications"sv, event, event_arguments.serialized());
        };
        // The notification is recorded first, since a platform that refuses it reports its close before show returns.
        notifications.set(id, ExtensionNotification { .platform_id = 0, .options = options });
        auto platform_id = NotificationManager::the().show_for_extension(ext->manifest.name(), move(data),
            {
                .on_shown = [deliver] { deliver("onShown"sv); },
                .on_clicked = [deliver] { deliver("onClicked"sv); },
                .on_closed = [deliver](bool by_user) { deliver("onClosed"sv, by_user); },
            });
        if (auto live = m_notifications.get(extension_id); live.has_value()) {
            if (auto notification = live->get(id); notification.has_value())
                notification->platform_id = platform_id;
        }
        reply_ok(JsonValue(id));
        return;
    }

    // Ladyflock does not apply extension themes: no theme is current, and one can't be set.
    if (namespace_name == "theme"sv) {
        if (!ext->manifest.has_permission("theme"sv)) {
            reply_err("Extension lacks the theme permission");
            return;
        }
        if (method_name == "getCurrent"sv) {
            JsonObject theme;
            theme.set("colors"sv, JsonValue {});
            theme.set("images"sv, JsonValue {});
            theme.set("properties"sv, JsonValue {});
            reply_ok(move(theme));
            return;
        }
        reply_err("Ladyflock does not apply extension themes");
        return;
    }

    // The omnibox keyword is not bound to the address bar, so there is no suggestion to show.
    if (namespace_name == "omnibox"sv && method_name == "setDefaultSuggestion"sv) {
        reply_ok(JsonValue {});
        return;
    }

    if (namespace_name == "idle"sv) {
        if (!ext->manifest.has_permission("idle"sv)) {
            reply_err("Extension lacks the idle permission");
            return;
        }
        auto const& arguments = !parsed_args.is_error() && parsed_args.value().is_array() ? parsed_args.value().as_array() : empty_arr;
        auto interval = arguments.is_empty() || !arguments[0].is_number() ? Optional<double> {} : arguments[0].get_double_with_precision_loss();
        if (!interval.has_value() || *interval < 0) {
            reply_err("Expected a detection interval in seconds");
            return;
        }
        // As in Firefox, no interval is shorter than 15 seconds.
        auto seconds = static_cast<u32>(clamp(*interval, 15.0, 4294967295.0));
        if (method_name == "queryState"sv) {
            reply_ok(JsonValue(idle_state_for(seconds)));
            return;
        }
        if (method_name == "setDetectionInterval"sv) {
            m_idle_detection_intervals.set(extension_id, seconds);
            reply_ok(JsonValue {});
            return;
        }
        reply_err("Unsupported idle method");
        return;
    }

    if (namespace_name == "management"sv) {
        auto const& arguments = !parsed_args.is_error() && parsed_args.value().is_array() ? parsed_args.value().as_array() : empty_arr;
        if (method_name == "getSelf"sv) {
            reply_ok(JsonValue(management_info(*ext)));
            return;
        }
        if (method_name == "uninstallSelf"sv) {
            // Firefox asks the user first when the extension wants a confirmation. Ladyflock has no such dialog, so it
            // declines, as Firefox does when the user cancels.
            if (!arguments.is_empty() && arguments[0].is_object() && arguments[0].as_object().get_bool("showConfirmDialog"sv) == true) {
                reply_err("User cancelled uninstall of extension");
                return;
            }
            reply_ok(JsonValue {});
            Core::deferred_invoke([weak = make_weak_ptr<ExtensionManager>(), extension_id] {
                if (auto* self = weak.ptr())
                    (void)self->remove_extension(extension_id);
            });
            return;
        }
        if (!ext->manifest.has_permission("management"sv)) {
            reply_err("This method requires the management permission");
            return;
        }
        if (method_name == "getAll"sv) {
            JsonArray all;
            for (auto const& [_, installed] : m_extensions_by_id)
                all.must_append(management_info(installed));
            reply_ok(move(all));
            return;
        }
        if (method_name == "get"sv) {
            auto target = !arguments.is_empty() && arguments[0].is_string() ? m_extensions_by_id.get(arguments[0].as_string()) : Optional<InstalledExtension&> {};
            if (!target.has_value()) {
                reply_err("No such addon");
                return;
            }
            reply_ok(JsonValue(management_info(*target)));
            return;
        }
        reply_err("Unsupported management method");
        return;
    }

    // Ladyflock saves no passwords and fills no forms itself, so these are off and no extension can turn them on. A
    // password manager turns them off at startup, which then changes nothing.
    if (namespace_name.bytes_as_string_view().starts_with("privacy.services."sv)) {
        if (!ext->manifest.has_permission("privacy"sv)) {
            reply_err("Extension lacks the privacy permission");
            return;
        }
        if (method_name == "get"sv) {
            JsonObject setting;
            setting.set("value"sv, false);
            setting.set("levelOfControl"sv, "not_controllable"sv);
            reply_ok(move(setting));
            return;
        }
        if (method_name.is_one_of("set"sv, "clear"sv)) {
            reply_ok(JsonValue(false));
            return;
        }
        reply_err("Unsupported privacy setting method");
        return;
    }

    if ((namespace_name == "action"sv || namespace_name == "browserAction"sv) && method_name == "getUserSettings"sv) {
        JsonObject settings;
        settings.set("isOnToolbar"sv, m_action_controller.resolved_state(extension_id).pinned);
        reply_ok(move(settings));
        return;
    }

    bool is_network_prediction = namespace_name == "privacy.network.networkPredictionEnabled"sv;
    bool is_gpc = namespace_name == "privacy.network.globalPrivacyControl"sv;
    bool is_hyperlink_auditing = namespace_name == "privacy.websites.hyperlinkAuditingEnabled"sv;
    if (is_network_prediction || is_gpc || is_hyperlink_auditing) {
        if (!ext->manifest.has_permission("privacy"sv)) {
            reply_err("Extension lacks 'privacy' permission");
            return;
        }
        if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().size() != 1 || !parsed_args.value().as_array().at(0).is_object()) {
            reply_err("Privacy settings require an options object");
            return;
        }
        auto const& options = parsed_args.value().as_array().at(0).as_object();
        if (options.has("scope"sv) && options.get_string("scope"sv) != "regular"sv) {
            reply_err("Only the regular profile scope is supported");
            return;
        }
        if (method_name == "get"sv) {
            if (is_gpc) {
                reply_ok(read_only_privacy_setting(Application::settings().global_privacy_control() == GlobalPrivacyControl::Yes));
            } else if (is_hyperlink_auditing) {
                // Hyperlink audit pings are not implemented by the browser.
                reply_ok(read_only_privacy_setting(false));
            } else if (options.get_bool("incognito"sv) == true) {
                reply_ok(read_only_privacy_setting(true));
            } else {
                reply_ok(network_prediction_setting_for(extension_id));
            }
            return;
        }

        bool setting_value = method_name == "set"sv;
        if (!setting_value && method_name != "clear"sv) {
            reply_err("Unsupported privacy setting method");
            return;
        }
        if (!is_network_prediction) {
            reply_ok(JsonValue(false));
            return;
        }
        auto owner = network_prediction_owner();
        if ((owner.has_value() && owner->extension_id != extension_id) || (!owner.has_value() && !setting_value)) {
            reply_ok(JsonValue(false));
            return;
        }
        auto value = options.get_bool("value"sv);
        if (setting_value && !value.has_value()) {
            reply_err("A boolean value is required");
            return;
        }
        auto previous_setting = ext->network_prediction_setting;
        bool prediction_was_enabled = network_prediction_enabled();
        ext->network_prediction_setting = setting_value ? value : Optional<bool> {};
        if (save_registry().is_error()) {
            ext->network_prediction_setting = previous_setting;
            reply_err("Could not save the network prediction setting");
            return;
        }
        notify_network_prediction_changed(prediction_was_enabled);
        reply_ok(JsonValue(true));
        return;
    }

    if (namespace_name == "alarms"sv) {
        if (!ext->manifest.has_permission("alarms"sv)) {
            reply_err("Extension lacks 'alarms' permission");
            return;
        }
        if (parsed_args.is_error() || !parsed_args.value().is_array()) {
            reply_err("Invalid alarm arguments");
            return;
        }
        auto result = m_alarms.invoke(extension_id, method_name, parsed_args.value().as_array());
        if (result.is_error()) {
            caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::formatted("{}", result.error()))).serialized());
        } else if (result.value().has_value()) {
            reply_ok(*result.value());
        } else {
            caller.async_extension_api_response(page_id, document_id, call_id, true, String {});
        }
        return;
    }

    if (namespace_name == "webNavigation"sv) {
        if (!ext->manifest.has_permission("webNavigation"sv)) {
            reply_err("Requires webNavigation permission");
            return;
        }
        if (parsed_args.is_error()) {
            reply_err("Invalid webNavigation arguments");
            return;
        }
        auto frames = navigation_frames(method_name, parsed_args.value());
        if (frames.is_error()) {
            caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::formatted("{}", frames.error()))).serialized());
            return;
        }
        reply_ok(frames.value());
        return;
    }

    if (namespace_name == "windows"sv) {
        auto const& arguments = !parsed_args.is_error() && parsed_args.value().is_array() ? parsed_args.value().as_array() : empty_arr;
        auto active = Application::the().active_web_view();
        auto focused_window = active.has_value() && active->is_private() == IsPrivate::No ? Optional<u64> { active->extension_window_id() } : Optional<u64> {};
        Vector<u64> window_ids;
        for (auto* view : ordinary_tab_views()) {
            if (!window_ids.contains_slow(view->extension_window_id()))
                window_ids.append(view->extension_window_id());
        }
        auto window_json = [&](u64 window_id, JsonValue const& options) {
            return window_info(extension_id, window_id, options.is_object() && options.as_object().get_bool("populate"sv) == true);
        };
        auto options_at = [&](size_t index) { return index < arguments.size() ? arguments[index] : JsonValue {}; };

        if (method_name == "getCurrent"sv) {
            reply_ok(JsonValue(window_json(caller_window(), options_at(0))));
            return;
        }
        if (method_name == "getLastFocused"sv) {
            auto window_id = focused_window.value_or(window_ids.is_empty() ? current_window_id() : window_ids.first());
            reply_ok(JsonValue(window_json(window_id, options_at(0))));
            return;
        }
        if (method_name == "get"sv) {
            // windows.WINDOW_ID_CURRENT is -2.
            auto window_id = arguments.is_empty() ? Optional<u64> {} : arguments[0].get_u64();
            if (!arguments.is_empty() && arguments[0].get_i64() == -2)
                window_id = caller_window();
            if (!window_id.has_value() || !window_ids.contains_slow(*window_id)) {
                reply_err("Invalid window ID");
                return;
            }
            reply_ok(JsonValue(window_json(*window_id, options_at(1))));
            return;
        }
        if (method_name == "getAll"sv) {
            auto options = options_at(0);
            if (options.is_object()) {
                auto types = options.as_object().get_array("windowTypes"sv);
                if (types.has_value() && !any_of(types->values(), [](auto const& type) { return type.is_string() && type.as_string() == "normal"sv; })) {
                    reply_ok(JsonArray {});
                    return;
                }
            }
            JsonArray windows;
            for (auto window_id : window_ids)
                windows.must_append(window_json(window_id, options));
            reply_ok(move(windows));
            return;
        }
        if (method_name == "create"sv) {
            auto options = options_at(0);
            auto const& details = options.is_object() ? options.as_object() : empty_obj;
            if (details.get_bool("incognito"sv) == true) {
                reply_err("Extension does not have permission for incognito mode");
                return;
            }
            // The first URL opens in the new window. Ladyflock opens every window as a normal one.
            Optional<String> requested;
            if (auto url = details.get_string("url"sv); url.has_value())
                requested = *url;
            else if (auto urls = details.get_array("url"sv); urls.has_value() && !urls->is_empty() && urls->at(0).is_string())
                requested = urls->at(0).as_string();
            auto base = URL::Parser::basic_parse(ByteString::formatted("moz-extension://{}/", ext->uuid));
            auto url = requested.has_value() ? URL::Parser::basic_parse(*requested, base) : URL::Parser::basic_parse("about:blank"sv);
            bool own_page = url.has_value() && url->scheme() == "moz-extension"sv && url->serialized_host() == ext->uuid;
            if (!url.has_value() || (!own_page && !url->scheme().is_one_of("http"sv, "https"sv) && url->serialize() != "about:blank"sv)) {
                reply_err("Illegal URL for a new window");
                return;
            }
            auto pending_id = m_next_window_creation_id++;
            m_pending_window_creations.append(PendingWindowCreation {
                .id = pending_id,
                .caller = caller.make_weak_ptr<WebContentClient>(),
                .page_id = page_id,
                .document_id = document_id,
                .call_id = call_id,
                .extension_id = extension_id,
                .timeout = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [this, pending_id] {
                    auto index = m_pending_window_creations.find_first_index_if([&](auto const& pending) { return pending.id == pending_id; });
                    if (!index.has_value())
                        return;
                    auto pending = m_pending_window_creations.take(*index);
                    if (auto renderer = pending.caller.strong_ref())
                        renderer->async_extension_api_response(pending.page_id, pending.document_id, pending.call_id, false, "\"The window did not open\""_string);
                }),
            });
            m_pending_window_creations.last().timeout->start();
            Application::the().open_url_in_new_window(*url, IsPrivate::No);
            return;
        }
        reply_err("Unsupported windows method");
        return;
    }

    if (namespace_name == "tabs"sv && method_name == "getCurrent"sv) {
        // Only a page shown in a browser tab has a tab; a background page, popup or sidebar does not.
        auto& view = page->view();
        if (view.owner_view_id().has_value() || is_extension_background_view(view)) {
            reply_ok(JsonValue {});
            return;
        }
        auto tab = tab_info_for_extension(extension_id, view);
        reply_ok(tab.has_value() ? JsonValue(serialize_tab(*tab)) : JsonValue {});
        return;
    }

    if (namespace_name == "tabs"sv) {
        if (method_name == "query"sv) {
            Optional<bool> want_active;
            Optional<bool> want_current_window;
            Optional<u64> window_id;
            if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty() && parsed_args.value().as_array().at(0).is_object()) {
                auto const& q = parsed_args.value().as_array().at(0).as_object();
                want_active = q.get_bool("active"sv);
                want_current_window = q.get_bool("currentWindow"sv);
                window_id = q.get_u64("windowId"sv);
            }
            auto current_window = caller_window();

            JsonArray arr;
            for (auto* view : ordinary_tab_views()) {
                auto info = tab_info_for_extension(extension_id, *view);
                if (!info.has_value())
                    continue;
                if (window_id.has_value() && info->window_id != *window_id)
                    continue;
                if (want_current_window.has_value() && (info->window_id == current_window) != *want_current_window)
                    continue;
                if (want_active.has_value() && info->active != *want_active)
                    continue;
                arr.must_append(serialize_tab(*info));
            }
            reply_ok(JsonValue(move(arr)));
            return;
        }
        if (method_name == "get"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty()) {
                reply_err("tabs.get requires tabId");
                return;
            }
            auto tab_id = parsed_args.value().as_array().at(0).get_u64();
            auto view = tab_id.has_value() ? ViewImplementation::find_view_by_id(*tab_id) : Optional<ViewImplementation&> {};
            if (!view.has_value()) {
                reply_err("Tab not found");
                return;
            }
            auto info = tab_info_for_extension(extension_id, *view);
            if (!info.has_value()) {
                reply_err("Tab unavailable");
                return;
            }
            reply_ok(JsonValue(serialize_tab(*info)));
            return;
        }
        if (method_name == "create"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty() || !parsed_args.value().as_array().at(0).is_object()) {
                reply_err("tabs.create requires createProperties");
                return;
            }
            auto const& props = parsed_args.value().as_array().at(0).as_object();
            auto raw_url = props.get_string("url"sv).value_or("about:blank"_string);
            auto url = URL::Parser::basic_parse(raw_url);
            if (!url.has_value())
                url = URL::Parser::basic_parse(ByteString::formatted("moz-extension://{}/{}", ext->uuid, raw_url));
            if (!url.has_value()
                || !(*url == URL::about_blank() || url->scheme().is_one_of("http"sv, "https"sv)
                    || (url->scheme() == "moz-extension"sv && url->serialized_host() == ext->uuid))) {
                reply_err("Tab target URL is unavailable to this extension");
                return;
            }
            bool active = props.get_bool("active"sv).value_or(true);
            auto* source = caller_tab();
            if (auto window = props.get_u64("windowId"sv); window.has_value()) {
                source = nullptr;
                for (auto* tab : ordinary_tab_views()) {
                    if (tab->extension_window_id() == *window && tab->is_current_tab_of_window()) {
                        source = tab;
                        break;
                    }
                }
            }
            if (!source) {
                reply_err("No ordinary browser window is available");
                return;
            }
            auto tab = Application::the().open_blank_new_tab(active ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No, source);
            if (!tab.has_value()) {
                reply_err("Could not open a new tab");
                return;
            }
            tab->set_extension_window_id(source->extension_window_id());
            tab->load_from_browser_ui(*url);
            auto info = tab_info_for_extension(extension_id, *tab);
            if (!info.has_value()) {
                reply_err("New tab is unavailable");
                return;
            }
            info->active = active;
            reply_ok(serialize_tab(*info));
            return;
        }
        if (method_name == "update"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty()) {
                reply_err("tabs.update requires tabId/properties");
                return;
            }
            auto const& arr = parsed_args.value().as_array();
            u64 tab_id = arr.at(0).get_u64().value_or(0);
            JsonObject props = arr.size() > 1 && arr.at(1).is_object() ? arr.at(1).as_object() : (arr.at(0).is_object() ? arr.at(0).as_object() : JsonObject {});
            auto view = ViewImplementation::find_view_by_id(tab_id);
            if (arr.at(0).is_object()) {
                auto* candidate = caller_tab();
                view = candidate ? Optional<ViewImplementation&> { *candidate } : OptionalNone {};
            }
            if (!view.has_value() || !tab_info_for_extension(extension_id, *view).has_value()) {
                reply_err("Tab not found");
                return;
            }
            if (auto raw_url = props.get_string("url"sv); raw_url.has_value()) {
                auto parsed = URL::Parser::basic_parse(*raw_url);
                if (!parsed.has_value() || !(parsed->scheme().is_one_of("http"sv, "https"sv) || (parsed->scheme() == "moz-extension"sv && parsed->serialized_host() == ext->uuid))) {
                    reply_err("Tab target URL is unavailable to this extension");
                    return;
                }
                view->load_from_browser_ui(*parsed);
            }
            if (props.get_bool("active"sv).value_or(false) && view->on_activate_tab)
                view->on_activate_tab();
            auto info = tab_info_for_extension(extension_id, *view);
            reply_ok(serialize_tab(*info));
            return;
        }
        if (method_name == "remove"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty()) {
                reply_err("tabs.remove requires tabIds");
                return;
            }
            auto close_id = [&](u64 tid) {
                if (auto view = ViewImplementation::find_view_by_id(tid); view.has_value() && tab_info_for_extension(extension_id, *view).has_value())
                    view->request_close();
            };
            auto arg = parsed_args.value().as_array().at(0);
            if (auto single = arg.get_u64(); single.has_value())
                close_id(*single);
            else if (arg.is_array()) {
                for (auto const& id_val : arg.as_array().values()) {
                    if (auto tid = id_val.get_u64(); tid.has_value())
                        close_id(*tid);
                }
            }
            reply_ok(JsonValue {});
            return;
        }
        if (method_name == "reload"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().size() > 2) {
                reply_err("Expected a tab ID and optional reload properties");
                return;
            }
            auto const& arguments = parsed_args.value().as_array();
            auto* view = caller_tab();
            size_t options_index = 0;
            if (!arguments.is_empty() && arguments[0].is_number()) {
                auto tab_id = arguments[0].get_u64();
                auto target = tab_id.has_value() ? ViewImplementation::find_view_by_id(*tab_id) : Optional<ViewImplementation&> {};
                view = target.has_value() ? &*target : nullptr;
                options_index = 1;
            } else if (!arguments.is_empty() && arguments[0].is_null() && arguments.size() == 2) {
                options_index = 1;
            }
            if (!view || !tab_info_for_extension(extension_id, *view).has_value()) {
                reply_err("Tab is unavailable");
                return;
            }
            bool bypass_cache = false;
            if (arguments.size() > options_index) {
                auto const& options = arguments[options_index];
                if (arguments.size() != options_index + 1 || (!options.is_null() && !options.is_object())) {
                    reply_err("Expected reload properties to be an object");
                    return;
                }
                if (options.is_object()) {
                    auto bypass_value = options.as_object().get_bool("bypassCache"sv);
                    if (options.as_object().has("bypassCache"sv) && !bypass_value.has_value()) {
                        reply_err("Expected bypassCache to be a boolean");
                        return;
                    }
                    bypass_cache = bypass_value.value_or(false);
                }
            }
            view->reload(bypass_cache ? BypassCache::Yes : BypassCache::No);
            reply_ok(JsonValue {});
            return;
        }
        if (method_name == "sendMessage"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().size() < 2) {
                reply_err("tabs.sendMessage requires tabId and message");
                return;
            }
            auto tab_id = parsed_args.value().as_array().at(0).get_u64().value_or(0);
            auto msg = parsed_args.value().as_array().at(1);
            auto view = ViewImplementation::find_view_by_id(tab_id);
            if (!view.has_value() || view->is_private() == IsPrivate::Yes) {
                reply_err("Target tab not found");
                return;
            }
            // The message goes to the frame the options name, or else to the tab's top-level document.
            i64 frame_id = 0;
            if (auto const& arguments = parsed_args.value().as_array(); arguments.size() > 2 && arguments.at(2).is_object()) {
                auto const& options = arguments.at(2).as_object();
                if (options.has("frameId"sv)) {
                    auto requested = options.get_i64("frameId"sv);
                    if (!requested.has_value() || *requested < 0) {
                        reply_err("Invalid frame ID");
                        return;
                    }
                    frame_id = *requested;
                }
            }
            auto const* frame = scripting_target_frame(*view, frame_id);
            if (!frame) {
                reply_err("Could not establish connection. Receiving end does not exist.");
                return;
            }
            auto weak_caller = caller.make_weak_ptr<WebContentClient>();
            auto rid = register_event_reply([weak_caller, page_id, document_id, call_id](bool has_val, String const& res_json) {
                if (auto c = weak_caller.strong_ref())
                    c->async_extension_api_response(page_id, document_id, call_id, true, has_val ? res_json : "null"_string);
            });

            JsonObject sender;
            sender.set("id"sv, ext->extension_id);
            JsonArray ev_args;
            ev_args.must_append(msg);
            ev_args.must_append(move(sender));

            auto& target_document = frame->active_document();
            auto& target_page = target_document.host() ? *target_document.host() : view->page();
            bind_event_reply(rid, target_page);
            target_page.client().async_dispatch_extension_event(
                target_page.id(),
                ext->extension_id,
                "runtime"_string,
                "onMessage"_string,
                ev_args.serialized(),
                rid,
                target_document.id());
            return;
        }
        if (method_name == "executeScript"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty()) {
                reply_err("tabs.executeScript requires details");
                return;
            }
            JsonObject details = parsed_args.value().as_array().size() > 1 && parsed_args.value().as_array().at(1).is_object()
                ? parsed_args.value().as_array().at(1).as_object()
                : (parsed_args.value().as_array().at(0).is_object() ? parsed_args.value().as_array().at(0).as_object() : JsonObject {});
            if (auto tid = parsed_args.value().as_array().at(0).get_u64(); tid.has_value()) {
                JsonObject t;
                t.set("tabId"sv, static_cast<i64>(*tid));
                details.set("target"sv, move(t));
            }
            if (auto file = details.get_string("file"sv); file.has_value()) {
                JsonArray files;
                files.must_append(*file);
                details.set("files"sv, move(files));
            }
            handle_scripting_execute_script(caller, page_id, call_id, document_id, *ext, details, false);
            return;
        }
        if (method_name == "insertCSS"sv || method_name == "removeCSS"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty()) {
                reply_err("tabs CSS API requires details");
                return;
            }
            JsonObject details = parsed_args.value().as_array().size() > 1 && parsed_args.value().as_array().at(1).is_object()
                ? parsed_args.value().as_array().at(1).as_object()
                : (parsed_args.value().as_array().at(0).is_object() ? parsed_args.value().as_array().at(0).as_object() : JsonObject {});
            if (auto tid = parsed_args.value().as_array().at(0).get_u64(); tid.has_value()) {
                JsonObject t;
                t.set("tabId"sv, static_cast<i64>(*tid));
                details.set("target"sv, move(t));
            }
            if (auto file = details.get_string("file"sv); file.has_value()) {
                JsonArray files;
                files.must_append(*file);
                details.set("files"sv, move(files));
            }
            handle_scripting_css(caller, page_id, call_id, document_id, *ext, details, method_name == "removeCSS"sv);
            return;
        }
        reply_err("Unsupported tabs API method");
        return;
    }

    if (namespace_name == "contentScripts"sv) {
        if (ext->manifest.manifest_version() != Web::Extensions::ManifestVersion::V2
            || parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().size() != 1) {
            reply_err("Expected a Firefox MV2 content script registration call");
            return;
        }
        auto const& argument = parsed_args.value().as_array().at(0);
        auto& scripts = m_extensions_by_id.get(extension_id)->registered_content_scripts;
        if (method_name == "unregister"sv && argument.is_string()) {
            auto removed = scripts.remove_all_matching([&](auto const& script) {
                return script.id == argument.as_string() && script.legacy_owner.has_value()
                    && script.legacy_owner->renderer.ptr() == &caller && script.legacy_owner->page_id == page_id
                    && script.legacy_owner->document_id == document_id;
            });
            if (!removed) {
                reply_err("This document does not own the content script registration");
                return;
            }
            reply_ok(JsonValue {});
            return;
        }
        if (method_name != "register"sv || !argument.is_object()) {
            reply_err("Expected Firefox MV2 content script options");
            return;
        }

        auto options = argument.as_object();
        auto id = MUST(String::formatted("legacy-{}", generate_random_uuid()));
        options.set("id"sv, id);
        auto registration = parse_registered_content_script(options, ext->install_directory, RegisteredExtensionContentScript::Owner { caller.make_weak_ptr<WebContentClient>(), page_id, document_id });
        if (registration.is_error()) {
            caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::formatted("{}", registration.error()))).serialized());
            return;
        }
        auto origins = granted_origin_patterns(*ext);
        for (auto const& pattern : registration.value().rule.matches) {
            if (!pattern.is_web_host_permission() || !any_of(origins, [&](String const& allowed) {
                    return MUST(Web::Extensions::MatchPattern::parse(allowed)).covers_origin(pattern);
                })) {
                reply_err("Extension lacks host access for the content script registration");
                return;
            }
        }

        scripts.remove_all_matching([](auto const& script) { return script.legacy_owner.has_value() && !is_legacy_registration_owner_active(*script.legacy_owner); });
        size_t source_bytes = 0;
        auto tally_sources = [&](auto const& script) {
            for (auto const& source : script.legacy_js)
                source_bytes += source.content.byte_count();
            for (auto const& source : script.legacy_css)
                source_bytes += source.content.byte_count();
        };
        for (auto const& script : scripts)
            tally_sources(script);
        tally_sources(registration.value());
        if (scripts.size() >= 1024 || source_bytes > 16 * MiB) {
            reply_err("Content script registrations exceed their storage limit");
            return;
        }
        scripts.append(registration.release_value());
        reply_ok(id);
        return;
    }

    if (namespace_name == "scripting"sv) {
        if (!ext->manifest.has_permission("scripting"sv)) {
            reply_err("Extension lacks 'scripting' permission");
            return;
        }
        if (parsed_args.is_error() || !parsed_args.value().is_array()) {
            reply_err("Expected scripting arguments");
            return;
        }
        auto const& arguments = parsed_args.value().as_array();
        if (method_name.bytes_as_string_view().is_one_of("registerContentScripts"sv, "getRegisteredContentScripts"sv, "updateContentScripts"sv, "unregisterContentScripts"sv)) {
            auto result = handle_registered_content_scripts(*m_extensions_by_id.get(extension_id), method_name, arguments);
            if (result.is_error())
                caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::formatted("{}", result.error()))).serialized());
            else
                reply_ok(result.release_value());
            return;
        }
        if (arguments.is_empty() || !arguments[0].is_object()) {
            reply_err("scripting method requires injection details object");
            return;
        }
        auto const& details = arguments[0].as_object();
        if (method_name == "executeScript"sv)
            handle_scripting_execute_script(caller, page_id, call_id, document_id, *ext, details);
        else if (method_name == "insertCSS"sv)
            handle_scripting_css(caller, page_id, call_id, document_id, *ext, details, false);
        else if (method_name == "removeCSS"sv)
            handle_scripting_css(caller, page_id, call_id, document_id, *ext, details, true);
        else
            reply_err("Unsupported scripting method");
        return;
    }

    auto copy_opt_string = [](Optional<String const&> const& value) -> Optional<String> {
        if (!value.has_value())
            return {};
        return *value;
    };

    auto valid_chrome_target = [&](Optional<u64> tab_id, Optional<u64> window_id) {
        if (tab_id.has_value() && window_id.has_value())
            return false;
        if (tab_id.has_value()) {
            auto tab = ViewImplementation::find_view_by_id(*tab_id);
            return tab.has_value() && tab->is_private() == IsPrivate::No && !tab->owner_view_id().has_value() && !is_extension_background_view(*tab);
        }
        if (window_id.has_value()) {
            for (auto* tab : ordinary_tab_views()) {
                if (tab->extension_window_id() == *window_id)
                    return true;
            }
            return false;
        }
        return true;
    };

    struct IconUpdate {
        bool keep_current { false };
        Optional<String> path;
    };
    auto icon_from_details = [&](JsonObject const& details) -> ErrorOr<IconUpdate> {
        auto path = details.get("path"sv);
        if (!path.has_value() || path->is_null()) {
            // The toolbar draws bitmaps from the package only, so an imageData icon leaves the current icon in place.
            if (auto image_data = details.get("imageData"sv); image_data.has_value() && !image_data->is_null())
                return IconUpdate { .keep_current = true, .path = {} };
            if (!path.has_value())
                return Error::from_string_literal("setIcon requires a package path or size-to-path object");
            return IconUpdate {};
        }
        // As in Firefox, a relative path is relative to the page that calls, so "../icons/a.png" from
        // "background/index.html" names "icons/a.png".
        auto resolve = [&](JsonValue const& value) -> JsonValue {
            if (!value.is_string() || value.as_string().starts_with('/'))
                return value;
            auto resolved = URL::Parser::basic_parse(value.as_string(), document->creation_url());
            if (!resolved.has_value() || resolved->scheme() != "moz-extension"sv || resolved->serialized_host() != ext->uuid)
                return value;
            return JsonValue { MUST(String::from_byte_string(URL::percent_decode(resolved->serialize_path()))) };
        };
        JsonValue resolved_path = resolve(*path);
        if (path->is_object()) {
            JsonObject sizes;
            path->as_object().for_each_member([&](auto const& size, auto const& value) { sizes.set(size, resolve(value)); });
            resolved_path = move(sizes);
        }
        auto icons = TRY(Web::Extensions::ExtensionManifest::parse_icon_map(resolved_path));
        auto selected = Web::Extensions::ExtensionManifest::preferred_icon(icons);
        if (!selected.has_value()) {
            // The isolated image decoder can't draw SVG. Firefox accepts SVG icons, so keep the current icon
            // instead of refusing the call.
            if (!icons.is_empty())
                return IconUpdate { .keep_current = true, .path = {} };
            return Error::from_string_literal("No valid icon size was supplied");
        }
        return IconUpdate { .path = move(selected) };
    };

    if (namespace_name == "action"sv || namespace_name == "browserAction"sv) {
        JsonObject arg_obj;
        if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty() && parsed_args.value().as_array().at(0).is_object())
            arg_obj = parsed_args.value().as_array().at(0).as_object();
        auto tab_id = arg_obj.get_u64("tabId"sv);
        auto window_id = arg_obj.get_u64("windowId"sv);
        if (!ext->manifest.action().has_value() || !valid_chrome_target(tab_id, window_id)) {
            reply_err("No matching extension action or browser target");
            return;
        }
        if (tab_id.has_value())
            window_id = ViewImplementation::find_view_by_id(*tab_id)->extension_window_id();

        if (method_name == "setTitle"sv) {
            m_action_controller.set_title(extension_id, copy_opt_string(arg_obj.get_string("title"sv)), tab_id, window_id);
            reply_ok(JsonValue {});
        } else if (method_name == "getTitle"sv) {
            reply_ok(JsonValue(m_action_controller.get_title(extension_id, tab_id, window_id)));
        } else if (method_name == "setBadgeText"sv) {
            m_action_controller.set_badge_text(extension_id, copy_opt_string(arg_obj.get_string("text"sv)), tab_id, window_id);
            reply_ok(JsonValue {});
        } else if (method_name == "getBadgeText"sv) {
            reply_ok(JsonValue(m_action_controller.get_badge_text(extension_id, tab_id, window_id)));
        } else if (method_name == "setBadgeBackgroundColor"sv) {
            auto value = arg_obj.get("color"sv);
            Optional<Gfx::Color> color;
            if (value.has_value() && value->is_string())
                color = Gfx::Color::from_string(value->as_string());
            if (value.has_value() && value->is_array() && value->as_array().size() == 4) {
                auto const& rgba = value->as_array();
                auto component = [&](size_t i) { return rgba.at(i).get_u32().value_or(256); };
                if (component(0) <= 255 && component(1) <= 255 && component(2) <= 255 && component(3) <= 255)
                    color = Gfx::Color(component(0), component(1), component(2), component(3));
            }
            if (!color.has_value() && (!value.has_value() || !value->is_null())) {
                reply_err("Badge color must be a CSS color or four RGBA bytes");
                return;
            }
            auto serialized = color.map([](auto const& c) { return MUST(String::formatted("#{:02x}{:02x}{:02x}{:02x}", c.red(), c.green(), c.blue(), c.alpha())); });
            m_action_controller.set_badge_background_color(extension_id, serialized, tab_id, window_id);
            reply_ok(JsonValue {});
        } else if (method_name == "getBadgeBackgroundColor"sv) {
            auto color = Gfx::Color::from_string(m_action_controller.get_badge_background_color(extension_id, tab_id, window_id)).value_or(Gfx::Color::Transparent);
            JsonArray rgba;
            for (auto component : { color.red(), color.green(), color.blue(), color.alpha() })
                rgba.must_append(component);
            reply_ok(move(rgba));
        } else if (method_name == "setPopup"sv) {
            m_action_controller.set_popup(extension_id, copy_opt_string(arg_obj.get_string("popup"sv)), tab_id, window_id);
            reply_ok(JsonValue {});
        } else if (method_name == "getPopup"sv) {
            reply_ok(JsonValue(m_action_controller.get_popup(extension_id, tab_id, window_id).value_or({})));
        } else if (method_name == "setIcon"sv) {
            auto update = icon_from_details(arg_obj);
            if (update.is_error()) {
                reply_err("setIcon requires a package path or size-to-path object");
                return;
            }
            if (!update.value().keep_current)
                m_action_controller.set_icon(extension_id, update.value().path, tab_id, window_id);
            reply_ok(JsonValue {});
        } else if (method_name == "enable"sv) {
            Optional<u64> tid;
            if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty())
                tid = parsed_args.value().as_array().at(0).get_u64();
            if (!valid_chrome_target(tid, {})) {
                reply_err("Unknown tab");
                return;
            }
            m_action_controller.set_enabled(extension_id, true, tid);
            reply_ok(JsonValue {});
        } else if (method_name == "disable"sv) {
            Optional<u64> tid;
            if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty())
                tid = parsed_args.value().as_array().at(0).get_u64();
            if (!valid_chrome_target(tid, {})) {
                reply_err("Unknown tab");
                return;
            }
            m_action_controller.set_enabled(extension_id, false, tid);
            reply_ok(JsonValue {});
        } else {
            reply_err("Unsupported action method");
        }
        if (!method_name.bytes_as_string_view().starts_with("get"sv) && method_name != "isOpen"sv)
            notify_chrome_changed();
        return;
    }

    if (namespace_name == "menus"sv) {
        if (!ext->manifest.has_permission("menus"sv) && !ext->manifest.has_permission("contextMenus"sv)) {
            reply_err("Extension has no menus permission");
            return;
        }
        if (parsed_args.is_error() || !parsed_args.value().is_array()) {
            reply_err("Expected menu arguments");
            return;
        }
        // Menus are drawn when they open, so a menu that is already showing keeps the items it opened with.
        if (method_name == "refresh"sv) {
            reply_ok(JsonValue {});
            return;
        }
        auto result = change_page_menu(extension_id, method_name, parsed_args.value().as_array());
        if (result.is_error()) {
            caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::from_utf8(result.error().string_literal()))).serialized());
            return;
        }
        reply_ok(JsonValue {});
        return;
    }

    if (namespace_name == "sidebarAction"sv) {
        JsonObject arg_obj;
        if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty() && parsed_args.value().as_array().at(0).is_object())
            arg_obj = parsed_args.value().as_array().at(0).as_object();
        auto tab_id = arg_obj.get_u64("tabId"sv);
        auto scope = arg_obj.get_u64("windowId"sv);
        if (!ext->manifest.sidebar_action().has_value() || !valid_chrome_target(tab_id, scope)) {
            reply_err("No matching extension sidebar or browser target");
            return;
        }
        if (tab_id.has_value())
            scope = ViewImplementation::find_view_by_id(*tab_id)->extension_window_id();
        auto window_id = scope.value_or_lazy_evaluated(caller_window);

        if (method_name == "setIcon"sv) {
            auto update = icon_from_details(arg_obj);
            if (update.is_error()) {
                reply_err("setIcon requires a package path or size-to-path object");
                return;
            }
            if (!update.value().keep_current)
                m_sidebar_controller.set_icon(extension_id, update.value().path, tab_id, scope);
            reply_ok(JsonValue {});
        } else if (method_name == "setPanel"sv) {
            m_sidebar_controller.set_panel(extension_id, copy_opt_string(arg_obj.get_string("panel"sv)), tab_id, scope);
            reply_ok(JsonValue {});
        } else if (method_name == "getPanel"sv) {
            reply_ok(JsonValue(m_sidebar_controller.get_panel(extension_id, tab_id, scope).value_or({})));
        } else if (method_name == "setTitle"sv) {
            m_sidebar_controller.set_title(extension_id, copy_opt_string(arg_obj.get_string("title"sv)), tab_id, scope);
            reply_ok(JsonValue {});
        } else if (method_name == "getTitle"sv) {
            reply_ok(JsonValue(m_sidebar_controller.get_title(extension_id, tab_id, scope)));
        } else if (method_name == "open"sv) {
            bool has_gesture = caller.page(page_id) ? caller.page(page_id)->has_transient_user_activation() : false;
            if (auto err = m_sidebar_controller.open(extension_id, window_id, has_gesture); err.is_error())
                caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::formatted("{}", err.error()))).serialized());
            else
                reply_ok(JsonValue {});
        } else if (method_name == "close"sv) {
            m_sidebar_controller.close(extension_id, window_id);
            reply_ok(JsonValue {});
        } else if (method_name == "toggle"sv) {
            bool has_gesture = caller.page(page_id) ? caller.page(page_id)->has_transient_user_activation() : false;
            auto res = m_sidebar_controller.toggle(extension_id, window_id, has_gesture);
            if (res.is_error())
                caller.async_extension_api_response(page_id, document_id, call_id, false, JsonValue(MUST(String::formatted("{}", res.error()))).serialized());
            else
                reply_ok(JsonValue(res.value()));
        } else if (method_name == "isOpen"sv) {
            reply_ok(JsonValue(m_sidebar_controller.is_open(extension_id, window_id)));
        } else {
            reply_err("Unsupported sidebarAction method");
        }
        if (!method_name.bytes_as_string_view().starts_with("get"sv) && method_name != "isOpen"sv)
            notify_chrome_changed();
        return;
    }

    if (namespace_name == "permissions"sv) {
        if (method_name == "getAll"sv) {
            JsonArray permissions;
            for (auto const& permission : ext->manifest.permissions())
                permissions.must_append(permission);
            JsonArray origins;
            for (auto const& origin : granted_origin_patterns(*ext))
                origins.must_append(origin);
            JsonObject result;
            result.set("permissions"sv, move(permissions));
            result.set("origins"sv, move(origins));
            reply_ok(JsonValue(move(result)));
            return;
        }
        if (!method_name.is_one_of("contains"sv, "remove"sv, "request"sv)) {
            reply_err("Unsupported permissions method");
            return;
        }
        if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty() || !parsed_args.value().as_array().at(0).is_object()) {
            reply_err("Permissions API requires a permissions object");
            return;
        }
        auto const& request = parsed_args.value().as_array().at(0).as_object();
        if ((request.has("permissions"sv) && !request.get_array("permissions"sv).has_value())
            || (request.has("origins"sv) && !request.get_array("origins"sv).has_value())) {
            reply_err("Permissions and origins must be arrays");
            return;
        }
        // A request can confirm existing grants. New grants still require browser-owned site access controls.
        auto removing = method_name == "remove"sv;
        bool ok = true;
        if (auto permissions = request.get_array("permissions"sv); permissions.has_value()) {
            for (auto const& item : permissions->values()) {
                if (!item.is_string()) {
                    reply_err("Permission name must be a string");
                    return;
                }
                if (ext->manifest.has_permission(item.as_string()) == removing)
                    ok = false;
            }
        }
        auto requested_origins = request.get_array("origins"sv);
        auto effective_origins = granted_origin_patterns(*ext);
        Vector<Web::Extensions::MatchPattern> patterns;
        if (requested_origins.has_value()) {
            for (auto const& item : requested_origins->values()) {
                if (!item.is_string()) {
                    reply_err("Origin match pattern must be a string");
                    return;
                }
                auto pattern = Web::Extensions::MatchPattern::parse(item.as_string());
                if (pattern.is_error()) {
                    reply_err("Invalid origin match pattern");
                    return;
                }
                patterns.append(pattern.release_value());
            }
        }
        auto includes_origin = [&](auto const& pattern) {
            return any_of(effective_origins, [&](String const& allowed) {
                return MUST(Web::Extensions::MatchPattern::parse(allowed)).covers_origin(pattern);
            });
        };
        if (removing) {
            auto current_hosts = ext->granted_extra_hosts;
            for (auto const& host : current_hosts) {
                bool remove_grant = any_of(patterns, [&](auto const& pattern) {
                    return pattern.covers_host(host)
                        && any_of(ext->manifest.optional_host_permissions(), [&](auto const& optional) { return optional.covers_origin(pattern); });
                });
                if (remove_grant) {
                    if (auto result = set_site_access(extension_id, ext->allow_all_sites, host, false); result.is_error()) {
                        reply_err("Unable to persist revoked origin");
                        return;
                    }
                }
            }
            effective_origins = granted_origin_patterns(*ext);
        }
        if (any_of(patterns, [&](auto const& pattern) { return includes_origin(pattern) == removing; }))
            ok = false;
        if (request.has("data_collection"sv))
            ok = false;
        reply_ok(JsonValue(ok));
        return;
    }

    if (namespace_name == "declarativeNetRequest"sv) {
        if (context_type == Web::Extensions::ContextType::ContentScript || !Web::Extensions::uses_declarative_net_request(ext->manifest)) {
            reply_err("Declarative network requests require an extension page with the declarativeNetRequest permission");
            return;
        }
        if (method_name == "getEnabledRulesets"sv) {
            JsonArray ids;
            for (auto const& ruleset : ext->declarative_rulesets) {
                if (ruleset.enabled)
                    ids.must_append(ruleset.id);
            }
            reply_ok(JsonValue(move(ids)));
            return;
        }
        if (method_name == "getAvailableStaticRuleCount"sv) {
            reply_ok(JsonValue(static_cast<u64>(available_static_declarative_rules(ext->declarative_rulesets))));
            return;
        }
        if (method_name == "isRegexSupported"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().size() != 1
                || !parsed_args.value().as_array().at(0).is_object() || !parsed_args.value().as_array().at(0).as_object().get_string("regex"sv).has_value()) {
                reply_err("Regex support query requires a regex string");
                return;
            }
            JsonObject result;
            result.set("isSupported"sv, false);
            result.set("reason"sv, "memoryLimitExceeded"_string);
            reply_ok(JsonValue(move(result)));
            return;
        }
        if (method_name.is_one_of("getDynamicRules"sv, "getSessionRules"sv, "updateDynamicRules"sv, "updateSessionRules"sv)) {
            if (parsed_args.is_error() || !parsed_args.value().is_array()) {
                reply_err("Invalid declarative rules call");
                return;
            }
            auto const& arguments = parsed_args.value().as_array();
            if (arguments.size() > 1 || (!arguments.is_empty() && !arguments.at(0).is_object())) {
                reply_err("Declarative rules call expects an options object");
                return;
            }
            auto is_dynamic = method_name.is_one_of("getDynamicRules"sv, "updateDynamicRules"sv);
            auto& mutable_extension = *m_extensions_by_id.get(extension_id);
            auto& rules = is_dynamic ? mutable_extension.dynamic_declarative_rules : mutable_extension.session_declarative_rules;
            JsonObject const empty_options;
            auto const& options = arguments.is_empty() ? empty_options : arguments.at(0).as_object();
            bool is_getter = method_name.bytes_as_string_view().starts_with("get"sv);
            auto id_key = is_getter ? "ruleIds"sv : "removeRuleIds"sv;
            HashTable<u32> ids;
            if (options.has(id_key)) {
                auto array = options.get_array(id_key);
                if (!array.has_value() || array->size() > 5000) {
                    reply_err("Invalid declarative rule ID list");
                    return;
                }
                for (auto const& value : array->values()) {
                    auto id = value.get_u32();
                    if (!id.has_value() || !*id) {
                        reply_err("Invalid declarative rule ID");
                        return;
                    }
                    ids.set(*id);
                }
            }

            JsonArray output;
            for (auto const& item : rules.source.values()) {
                bool listed = ids.contains(*item.as_object().get_u32("id"sv));
                if (is_getter ? !options.has(id_key) || listed : !listed)
                    output.must_append(item);
            }
            if (is_getter) {
                reply_ok(JsonValue(move(output)));
                return;
            }
            if (options.has("addRules"sv)) {
                auto additional = options.get_array("addRules"sv);
                if (!additional.has_value() || additional->size() > 5000 - output.size()) {
                    reply_err("Invalid or excessive declarative rules");
                    return;
                }
                for (auto const& item : additional->values())
                    output.must_append(item);
            }
            auto replacement = parse_mutable_ruleset(move(output));
            if (replacement.is_error()) {
                auto message = MUST(String::formatted("{}", replacement.error()));
                reply_err(message.to_byte_string().characters());
                return;
            }
            auto previous = move(rules);
            rules = replacement.release_value();
            if (is_dynamic && save_registry().is_error()) {
                rules = move(previous);
                reply_err("Could not save dynamic declarative rules");
                return;
            }
            refresh_declarative_watch_state();
            reply_ok(JsonValue {});
            return;
        }
        if (method_name != "updateEnabledRulesets"sv || parsed_args.is_error() || !parsed_args.value().is_array()) {
            reply_err("Unsupported declarativeNetRequest call");
            return;
        }
        auto const& arguments = parsed_args.value().as_array();
        if (arguments.size() != 1 || !arguments.at(0).is_object()) {
            reply_err("Ruleset update requires an options object");
            return;
        }
        auto const& options = arguments.at(0).as_object();
        auto& mutable_extension = *m_extensions_by_id.get(extension_id);
        Vector<bool> enabled;
        for (auto const& ruleset : mutable_extension.declarative_rulesets)
            enabled.append(ruleset.enabled);
        auto apply_ids = [&](StringView key, bool value) {
            if (!options.has(key))
                return true;
            auto ids = options.get_array(key);
            if (!ids.has_value() || ids->size() > 100)
                return false;
            for (auto const& id : ids->values()) {
                auto index = id.is_string() ? mutable_extension.declarative_rulesets.find_first_index_if([&](auto const& ruleset) { return ruleset.id == id.as_string(); }) : Optional<size_t> {};
                if (!index.has_value())
                    return false;
                enabled[*index] = value;
            }
            return true;
        };
        if (!apply_ids("disableRulesetIds"sv, false) || !apply_ids("enableRulesetIds"sv, true)) {
            reply_err("Ruleset update contains an invalid or unknown ruleset ID");
            return;
        }
        if (!declarative_ruleset_limits_ok(mutable_extension.declarative_rulesets, enabled)) {
            reply_err("Enabled declarative rulesets exceed the static rule limits");
            return;
        }
        for (size_t i = 0; i < enabled.size(); ++i)
            swap(enabled[i], mutable_extension.declarative_rulesets[i].enabled);
        if (auto saved = save_registry(); saved.is_error()) {
            for (size_t i = 0; i < enabled.size(); ++i)
                mutable_extension.declarative_rulesets[i].enabled = enabled[i];
            reply_err("Could not save enabled rulesets");
            return;
        }
        refresh_declarative_watch_state();
        reply_ok(JsonValue {});
        return;
    }

    if (namespace_name == "webRequest"sv) {
        if (!ext->manifest.has_permission("webRequest"sv)) {
            reply_err("Extension lacks 'webRequest' permission");
            return;
        }
        if (method_name == "addListener"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_object()) {
                reply_err("Invalid webRequest.addListener payload");
                return;
            }
            auto const& obj = parsed_args.value().as_object();
            u64 listener_id = obj.get_u64("listener_id"sv).value_or(0);
            bool blocking = obj.get_bool("blocking"sv).value_or(false);
            bool include_headers = obj.get_bool("includeHeaders"sv).value_or(false);
            auto event = Web::Extensions::web_request_event_from_string(obj.get_string("event"sv).value_or({}));
            if (!event.has_value() || !listener_id || context_type == Web::Extensions::ContextType::ContentScript
                || (blocking && *event != Web::Extensions::WebRequestEvent::BeforeRequest && *event != Web::Extensions::WebRequestEvent::HeadersReceived)) {
                reply_err("Invalid webRequest listener event or context");
                return;
            }
            if (blocking && !ext->manifest.has_permission("webRequestBlocking"sv)) {
                reply_err("Blocking webRequest listener requires 'webRequestBlocking' permission");
                return;
            }
            Vector<String> urls;
            if (auto arr = obj.get_array("urls"sv); arr.has_value()) {
                for (auto const& item : arr->values()) {
                    if (!item.is_string() || Web::Extensions::MatchPattern::parse(item.as_string()).is_error()) {
                        reply_err("Invalid webRequest URL pattern");
                        return;
                    }
                    if (item.as_string().starts_with_bytes("moz-extension://"sv)) {
                        auto own_scope = MUST(String::formatted("moz-extension://{}/", ext->uuid));
                        if (*event != Web::Extensions::WebRequestEvent::BeforeRequest || !item.as_string().starts_with_bytes(own_scope)) {
                            reply_err("webRequest cannot observe another extension's resources");
                            return;
                        }
                    }
                    urls.append(item.as_string());
                }
            }
            Vector<Web::Extensions::WebRequestResourceType> types;
            if (auto arr = obj.get_array("types"sv); arr.has_value()) {
                for (auto const& item : arr->values()) {
                    if (item.is_string()) {
                        if (auto rt = Web::Extensions::web_request_resource_type_from_string(item.as_string()); rt.has_value())
                            types.append(*rt);
                    }
                }
            }
            if (urls.is_empty()) {
                reply_err("webRequest requires at least one URL pattern");
                return;
            }
            m_web_request_coordinator.add_listener(ext->extension_id, listener_id, ext->grant_generation, move(urls), move(types), blocking, *event, include_headers);
            reply_ok(JsonValue {});
            return;
        }
        if (method_name == "removeListener"sv) {
            if (!parsed_args.is_error() && parsed_args.value().is_object()) {
                u64 listener_id = parsed_args.value().as_object().get_u64("listener_id"sv).value_or(0);
                m_web_request_coordinator.remove_listener(ext->extension_id, listener_id);
            }
            reply_ok(JsonValue {});
            return;
        }
        if (method_name == "handlerBehaviorChanged"sv) {
            // Both cached and uncached requests consult the currently registered listeners.
            reply_ok(JsonValue {});
            return;
        }
        reply_err("Unsupported webRequest method");
        return;
    }

    reply_err("Unsupported extension API namespace");
}

JsonObject ExtensionManager::window_info(StringView extension_id, u64 window_id, bool populate) const
{
    auto active = Application::the().active_web_view();
    JsonObject window;
    window.set("id"sv, window_id);
    window.set("focused"sv, active.has_value() && active->is_private() == IsPrivate::No && active->extension_window_id() == window_id);
    window.set("incognito"sv, false);
    window.set("type"sv, "normal"sv);
    window.set("state"sv, "normal"sv);
    window.set("alwaysOnTop"sv, false);
    if (populate) {
        JsonArray tabs;
        for (auto* view : ordinary_tab_views()) {
            if (view->extension_window_id() != window_id)
                continue;
            if (auto tab = tab_info_for_extension(extension_id, *view); tab.has_value())
                tabs.must_append(serialize_tab(*tab));
        }
        window.set("tabs"sv, move(tabs));
    }
    return window;
}

void ExtensionManager::notify_window_removed(u64 window_id)
{
    if (!m_known_window_ids.remove(window_id))
        return;
    JsonArray arguments;
    arguments.must_append(window_id);
    dispatch_to_extensions_with_permission({}, "windows"sv, "onRemoved"sv, arguments.serialized());
}

void ExtensionManager::notify_tab_created(ViewImplementation const& view)
{
    // A window's first tab is the window's creation, and the answer to the oldest windows.create still waiting.
    if (auto window_id = view.extension_window_id(); window_id != 0 && m_known_window_ids.set(window_id) == AK::HashSetResult::InsertedNewEntry) {
        if (!m_pending_window_creations.is_empty()) {
            auto pending = m_pending_window_creations.take_first();
            pending.timeout->stop();
            if (auto renderer = pending.caller.strong_ref())
                renderer->async_extension_api_response(pending.page_id, pending.document_id, pending.call_id, true, window_info(pending.extension_id, window_id, true).serialized());
        }
        for (auto const& [id, extension] : m_extensions_by_id) {
            if (!extension.enabled)
                continue;
            JsonArray arguments;
            arguments.must_append(window_info(id, window_id, false));
            dispatch_event_to_extension(extension.extension_id, "windows"sv, "onCreated"sv, arguments.serialized());
        }
    }

    for (auto const& [id, extension] : m_extensions_by_id) {
        if (auto tab = tab_info_for_extension(id, view); tab.has_value()) {
            JsonArray args;
            args.must_append(serialize_tab(*tab));
            dispatch_event_to_extension(extension.extension_id, "tabs"sv, "onCreated"sv, args.serialized());
        }
    }
}

void ExtensionManager::notify_tab_updated(u64 tab_id, TabChange change)
{
    auto view = ViewImplementation::find_view_by_id(tab_id);
    if (!view.has_value())
        return;
    for (auto const& [id, extension] : m_extensions_by_id) {
        auto tab = tab_info_for_extension(id, *view);
        if (!tab.has_value())
            continue;
        JsonObject info;
        switch (change) {
        case TabChange::Loading:
        case TabChange::Complete:
            tab->status = change == TabChange::Loading ? "loading"_string : "complete"_string;
            info.set("status"sv, tab->status);
            break;
        case TabChange::Title:
            if (!tab->title.has_value())
                continue;
            info.set("title"sv, *tab->title);
            break;
        case TabChange::URL:
            if (!tab->url.has_value())
                continue;
            info.set("url"sv, *tab->url);
            break;
        case TabChange::Pinned:
            info.set("pinned"sv, tab->pinned);
            break;
        }
        JsonArray args;
        args.must_append(tab_id);
        args.must_append(move(info));
        args.must_append(serialize_tab(*tab));
        dispatch_event_to_extension(extension.extension_id, "tabs"sv, "onUpdated"sv, args.serialized());
    }
}

void ExtensionManager::notify_tab_activated(u64 tab_id, u64 window_id)
{
    auto view = ViewImplementation::find_view_by_id(tab_id);
    if (!view.has_value() || view->is_private() == IsPrivate::Yes || is_extension_background_view(*view))
        return;

    JsonObject active_info;
    active_info.set("tabId"sv, static_cast<i64>(tab_id));
    active_info.set("windowId"sv, static_cast<i64>(window_id));
    JsonArray args;
    args.must_append(move(active_info));
    auto serialized = args.serialized();

    // Ladyflock highlights only the active tab, so the highlighted tabs change exactly when it does.
    JsonObject highlight_info;
    highlight_info.set("windowId"sv, static_cast<i64>(window_id));
    JsonArray tab_ids;
    tab_ids.must_append(static_cast<i64>(tab_id));
    highlight_info.set("tabIds"sv, move(tab_ids));
    JsonArray highlight_args;
    highlight_args.must_append(move(highlight_info));
    auto serialized_highlight = highlight_args.serialized();

    for (auto const& [ext_id, ext] : m_extensions_by_id) {
        if (!ext.enabled)
            continue;
        dispatch_event_to_extension(ext_id, "tabs"sv, "onActivated"sv, serialized);
        dispatch_event_to_extension(ext_id, "tabs"sv, "onHighlighted"sv, serialized_highlight);
    }
}

void ExtensionManager::notify_window_focus_changed(Optional<u64> window_id)
{
    if (!m_extensions_enabled || m_focused_window_id == window_id)
        return;
    m_focused_window_id = window_id;
    JsonArray arguments;
    arguments.must_append(window_id.has_value() ? static_cast<i64>(*window_id) : -1);
    auto serialized = arguments.serialized();
    for (auto const& [extension_id, extension] : m_extensions_by_id) {
        if (extension.enabled)
            dispatch_event_to_extension(extension_id, "windows"sv, "onFocusChanged"sv, serialized);
    }
}

void ExtensionManager::notify_tab_removed(u64 tab_id, bool window_closing)
{
    clear_tab_grants_on_navigation(tab_id);
    JsonObject remove_info;
    auto view = ViewImplementation::find_view_by_id(tab_id);
    remove_info.set("windowId"sv, view.has_value() ? view->extension_window_id() : current_window_id());
    remove_info.set("isWindowClosing"sv, window_closing);
    JsonArray args;
    args.must_append(static_cast<i64>(tab_id));
    args.must_append(move(remove_info));
    auto serialized = args.serialized();

    for (auto const& [ext_id, ext] : m_extensions_by_id) {
        if (ext.enabled)
            dispatch_event_to_extension(ext_id, "tabs"sv, "onRemoved"sv, serialized);
    }
}

ErrorOr<void> ExtensionManager::set_action_pinned(String const& id, bool pinned)
{
    m_action_controller.set_pinned(id, pinned);
    notify_chrome_changed();
    return save_registry();
}

ErrorOr<void> ExtensionManager::set_sidebar_width(u64 window_id, u32 width)
{
    m_sidebar_controller.set_width(window_id, width);
    notify_chrome_changed();
    return save_registry();
}

Optional<URL::URL> ExtensionManager::package_page_url(InstalledExtension const& extension, StringView path)
{
    auto relative = Web::Extensions::ExtensionManifest::normalize_package_relative_path(path);
    if (relative.is_error() || relative.value().is_empty())
        return {};
    return URL::Parser::basic_parse(ByteString::formatted("moz-extension://{}/{}", extension.uuid, relative.value()));
}

u64 ExtensionManager::current_window_id() const
{
    if (!Application::is_initialized())
        return 1;
    auto active = Application::the().active_web_view();
    if (active.has_value() && active->is_private() == IsPrivate::No)
        return active->extension_window_id();
    for (auto* view : ordinary_tab_views()) {
        if (view->is_current_tab_of_window())
            return view->extension_window_id();
    }
    return 1;
}

ExtensionActionController::InvokeOutcome ExtensionManager::invoke_action_for_tab(StringView extension_id, u64 tab_id, u64 window_id, Function<void(URL::URL const&)> open_popup)
{
    auto ext = m_extensions_by_id.get(extension_id);
    if (!ext.has_value() || !ext->enabled)
        return ExtensionActionController::InvokeOutcome::MissingExtension;

    auto view = ViewImplementation::find_view_by_id(tab_id);
    if (!view.has_value() || view->is_private() == IsPrivate::Yes || view->owner_view_id().has_value() || is_extension_background_view(*view) || !ext->manifest.action().has_value())
        return ExtensionActionController::InvokeOutcome::MissingExtension;
    if (!m_action_controller.is_enabled(extension_id, tab_id))
        return ExtensionActionController::InvokeOutcome::Disabled;
    if (ext->manifest.has_permission("activeTab"sv))
        grant_active_tab(extension_id, tab_id);

    return m_action_controller.invoke_action(
        extension_id,
        tab_id,
        window_id,
        [&](Optional<String> const& popup_path) {
            if (open_popup && popup_path.has_value()) {
                if (auto url = package_page_url(*ext, *popup_path); url.has_value())
                    open_popup(*url);
            }
        },
        [&]() {
            if (!view.has_value())
                return;
            auto info = tab_info_for_extension(extension_id, *view);
            if (!info.has_value())
                return;
            JsonObject tab_obj;
            tab_obj.set("id"sv, static_cast<i64>(info->tab_id));
            tab_obj.set("windowId"sv, static_cast<i64>(info->window_id));
            tab_obj.set("active"sv, info->active);
            tab_obj.set("status"sv, info->status);
            if (info->url.has_value())
                tab_obj.set("url"sv, *info->url);
            if (info->title.has_value())
                tab_obj.set("title"sv, *info->title);
            JsonArray args;
            args.must_append(move(tab_obj));
            auto ns = ext->manifest.manifest_version() == Web::Extensions::ManifestVersion::V3 ? "action"sv : "browserAction"sv;
            dispatch_event_to_extension(ext->extension_id, ns, "onClicked"sv, args.serialized());
        });
}

static CanonicalNavigable const* extension_menu_frame(ViewImplementation const& view, Web::HTML::CrossProcessId document)
{
    if (view.is_private() == IsPrivate::Yes || view.owner_view_id().has_value() || ExtensionManager::is_extension_background_view(view)
        || !view.has_traversable() || !view.url().scheme().is_one_of("http"sv, "https"sv))
        return nullptr;
    CanonicalNavigable const* result = nullptr;
    view.traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& frame) {
        if (frame.active_document().id() != document || !frame.active_document_url().scheme().is_one_of("http"sv, "https"sv))
            return IterationDecision::Continue;
        for (auto const* ancestor = &frame; ancestor; ancestor = ancestor->parent()) {
            if (!ancestor->active_document().host() || ancestor->active_document().unload_counter()
                || (ancestor->parent() && ancestor->container_document() != &ancestor->parent()->active_document()))
                return IterationDecision::Break;
        }
        result = &frame;
        return IterationDecision::Break;
    });
    return result;
}

Vector<ExtensionMenuItem> ExtensionManager::page_menu_items(ViewImplementation const& view, Web::HTML::CrossProcessId document, ExtensionMenuTarget const& target) const
{
    Vector<ExtensionMenuItem> result;
    auto* frame = extension_menu_frame(view, document);
    if (!extensions_enabled() || !frame)
        return result;
    for (auto const& [id, items] : m_page_menus) {
        auto ext = extension_by_id(id);
        if (!ext.has_value() || !ext->enabled)
            continue;
        for (auto const& item : items) {
            if (item.matches(*frame, target))
                result.append({ id, MUST(String::formatted("{} — {}", ext->manifest.name(), item.title)), item.token });
        }
    }
    return result;
}

void ExtensionManager::activate_page_menu_item(ViewImplementation const& view, Web::HTML::CrossProcessId document, u64 token, ExtensionMenuTarget const& target)
{
    auto* frame = extension_menu_frame(view, document);
    if (!extensions_enabled() || !frame)
        return;
    for (auto const& [id, items] : m_page_menus) {
        auto ext = extension_by_id(id);
        if (!ext.has_value() || !ext->enabled || (!ext->manifest.has_permission("menus"sv) && !ext->manifest.has_permission("contextMenus"sv)))
            continue;
        for (auto const& item : items) {
            if (item.token != token || !item.matches(*frame, target))
                continue;
            if (ext->manifest.has_permission("activeTab"sv))
                grant_active_tab(id, view.view_id());
            auto tab = tab_info_for_extension(id, view);
            if (!tab.has_value())
                return;
            JsonObject info;
            info.set("menuItemId"sv, item.id);
            info.set("pageUrl"sv, view.url().serialize());
            info.set("editable"sv, target.editable);
            info.set("frameId"sv, frame->extension_frame_id());
            if (frame->parent())
                info.set("frameUrl"sv, frame->active_document_url().serialize());
            if (target.link_url.has_value())
                info.set("linkUrl"sv, target.link_url->serialize());
            if (target.source_url.has_value())
                info.set("srcUrl"sv, target.source_url->serialize());
            if (target.media_type.has_value())
                info.set("mediaType"sv, *target.media_type);
            if (target.selection_text.has_value())
                info.set("selectionText"sv, *target.selection_text);
            JsonArray args;
            args.must_append(move(info));
            args.must_append(serialize_tab(*tab));
            dispatch_event_to_extension(id, "menus"sv, "onClicked"sv, args.serialized());
            notify_page_menu_hidden();
            return;
        }
    }
}

void ExtensionManager::notify_page_menu_shown(ViewImplementation const& view, Web::HTML::CrossProcessId document, ExtensionMenuTarget const& target)
{
    notify_page_menu_hidden();
    auto* frame = extension_menu_frame(view, document);
    if (!extensions_enabled() || !frame)
        return;

    JsonArray contexts;
    if (target.link_url.has_value())
        contexts.must_append("link"_string);
    if (target.media_type.has_value())
        contexts.must_append(*target.media_type);
    if (target.has_selection)
        contexts.must_append("selection"_string);
    if (target.editable)
        contexts.must_append("editable"_string);
    if (frame->parent())
        contexts.must_append("frame"_string);
    if (!target.link_url.has_value() && !target.media_type.has_value() && !target.has_selection && !target.editable)
        contexts.must_append("page"_string);

    for (auto const& [id, extension] : m_extensions_by_id) {
        if (!extension.enabled || (!extension.manifest.has_permission("menus"sv) && !extension.manifest.has_permission("contextMenus"sv)))
            continue;
        auto tab = tab_info_for_extension(id, view);
        if (!tab.has_value())
            continue;
        JsonArray menu_ids;
        if (auto items = m_page_menus.get(id); items.has_value()) {
            for (auto const& item : *items) {
                if (item.matches(*frame, target))
                    menu_ids.must_append(item.id);
            }
        }
        JsonObject info;
        info.set("menuIds"sv, move(menu_ids));
        info.set("contexts"sv, contexts);
        info.set("editable"sv, target.editable);
        info.set("frameId"sv, frame->extension_frame_id());
        // As in Firefox, what the menu was opened on is included only for pages the extension may access.
        if (has_host_permission(id, frame->active_document_url(), view.view_id())) {
            info.set("pageUrl"sv, view.url().serialize());
            if (frame->parent())
                info.set("frameUrl"sv, frame->active_document_url().serialize());
            if (target.link_url.has_value())
                info.set("linkUrl"sv, target.link_url->serialize());
            if (target.source_url.has_value())
                info.set("srcUrl"sv, target.source_url->serialize());
            if (target.selection_text.has_value())
                info.set("selectionText"sv, *target.selection_text);
        }
        if (target.media_type.has_value())
            info.set("mediaType"sv, *target.media_type);
        JsonArray args;
        args.must_append(move(info));
        args.must_append(serialize_tab(*tab));
        m_extensions_shown_page_menu.set(id);
        dispatch_event_to_extension(id, "menus"sv, "onShown"sv, args.serialized());
    }
}

void ExtensionManager::notify_page_menu_hidden()
{
    for (auto const& id : m_extensions_shown_page_menu) {
        if (auto extension = m_extensions_by_id.get(id); extension.has_value() && extension->enabled)
            dispatch_event_to_extension(id, "menus"sv, "onHidden"sv, "[]"_string);
    }
    m_extensions_shown_page_menu.clear();
}

JsonObject ExtensionManager::management_info(InstalledExtension const& extension) const
{
    auto const& manifest = extension.manifest;
    JsonObject info;
    info.set("id"sv, extension.extension_id);
    info.set("name"sv, manifest.name());
    info.set("shortName"sv, manifest.name());
    info.set("description"sv, manifest.description().value_or({}));
    info.set("version"sv, manifest.version());
    info.set("mayDisable"sv, true);
    info.set("enabled"sv, extension.enabled);
    info.set("type"sv, "extension"sv);
    info.set("installType"sv, extension.source == Web::Extensions::InstallSource::SignedXpi ? "normal"sv : "development"sv);
    auto raw_manifest = MUST(JsonValue::from_string(manifest.raw_json()));
    if (auto homepage = raw_manifest.as_object().get_string("homepage_url"sv); homepage.has_value())
        info.set("homepageUrl"sv, *homepage);
    if (manifest.options_ui().has_value()) {
        if (auto url = package_page_url(extension, manifest.options_ui()->page); url.has_value())
            info.set("optionsUrl"sv, url->serialize());
    } else {
        info.set("optionsUrl"sv, ""sv);
    }
    JsonArray permissions;
    for (auto const& permission : manifest.permissions()) {
        if (!Web::Extensions::MatchPattern::is_valid_host_permission_pattern(permission))
            permissions.must_append(permission);
    }
    info.set("permissions"sv, move(permissions));
    JsonArray hosts;
    for (auto const& origin : granted_origin_patterns(extension))
        hosts.must_append(origin);
    info.set("hostPermissions"sv, move(hosts));
    JsonArray icons;
    for (auto const& [size, path] : manifest.icons()) {
        auto size_number = size.to_number<u32>();
        auto url = package_page_url(extension, path);
        if (!size_number.has_value() || !url.has_value())
            continue;
        JsonObject icon;
        icon.set("size"sv, *size_number);
        icon.set("url"sv, url->serialize());
        icons.must_append(move(icon));
    }
    if (!icons.is_empty())
        info.set("icons"sv, move(icons));
    return info;
}

JsonObject ExtensionManager::summary_json() const
{
    JsonObject root;
    root.set("extensions_enabled"sv, m_extensions_enabled);
    root.set("enable_public_amo_catalog"sv, m_catalog_client.enable_public_amo_catalog());

    JsonArray list;
    for (auto const& [id, ext] : m_extensions_by_id) {
        JsonObject item;
        item.set("extension_id"sv, ext.extension_id);
        item.set("uuid"sv, ext.uuid);
        item.set("name"sv, ext.manifest.name());
        item.set("version"sv, ext.manifest.version());
        if (ext.manifest.package_icon_path().has_value())
            item.set("icon_key"sv, MUST(String::formatted("installed:{}:{}", ext.uuid, ext.manifest.version())));
        item.set("enabled"sv, ext.enabled);
        item.set("allow_all_sites"sv, ext.allow_all_sites);
        item.set("verified_signature"sv, ext.verified_signature);
        item.set("has_options"sv, ext.enabled && ext.manifest.options_ui().has_value());
        item.set("can_check_update"sv, can_check_for_update(ext));
        if (auto version = m_available_updates.get(id); version.has_value())
            item.set("update_version"sv, *version);
        HashTable<String> requested_hosts;
        for (auto const& pattern : ext.manifest.host_permissions())
            requested_hosts.set(pattern.raw());
        for (auto const& rule : ext.manifest.content_scripts()) {
            for (auto const& pattern : rule.matches)
                requested_hosts.set(pattern.raw());
        }
        auto serialize_hosts = [](HashTable<String> const& hosts) {
            JsonArray result;
            for (auto const& host : hosts)
                result.must_append(host);
            return result;
        };
        item.set("host_permissions"sv, serialize_hosts(requested_hosts));
        item.set("granted_hosts"sv, serialize_hosts(ext.granted_extra_hosts));
        item.set("revoked_hosts"sv, serialize_hosts(ext.revoked_hosts));
        item.set("compatibility_status"sv, MUST(String::from_utf8(Web::Extensions::to_string(ext.compatibility_status))));
        item.set("source"sv, ext.source == Web::Extensions::InstallSource::SignedXpi ? "signed_xpi"_string : "unpacked"_string);
        list.must_append(move(item));
    }
    root.set("extensions"sv, move(list));
    root.set("support_matrix"sv, Web::Extensions::SupportMatrix::to_json());
    return root;
}

static JsonObject navigation_frame_details(CanonicalNavigable const& frame)
{
    JsonObject result;
    result.set("url"sv, frame.active_document_url().serialize());
    result.set("frameId"sv, frame.extension_frame_id());
    result.set("parentFrameId"sv, frame.parent() ? frame.parent()->extension_frame_id() : -1);
    result.set("frameType"sv, frame.parent() ? "sub_frame"sv : "outermost_frame"sv);
    result.set("documentId"sv, frame.active_document().extension_document_id());
    result.set("documentLifecycle"sv, "active"sv);
    if (frame.parent())
        result.set("parentDocumentId"sv, frame.parent()->active_document().extension_document_id());
    return result;
}

ErrorOr<JsonValue> ExtensionManager::navigation_frames(StringView method, JsonValue const& arguments) const
{
    bool single = method == "getFrame"sv;
    if ((!single && method != "getAllFrames"sv) || !arguments.is_array() || arguments.as_array().size() != 1 || !arguments.as_array().at(0).is_object())
        return Error::from_string_literal("Invalid webNavigation method or arguments");
    auto const& details = arguments.as_array().at(0).as_object();
    auto tab_id = details.get_u64("tabId"sv);
    auto frame_id = details.get_i64("frameId"sv);
    auto document_id = details.get_string("documentId"sv);
    if ((single ? !document_id.has_value() && (!frame_id.has_value() || !tab_id.has_value()) : !tab_id.has_value())
        || (details.has("tabId"sv) && !tab_id.has_value())
        || (details.has("frameId"sv) && (!frame_id.has_value() || *frame_id < 0))
        || (details.has("documentId"sv) && !document_id.has_value()))
        return Error::from_string_literal("Invalid tab, frame or document identifier");
    for (auto* tab : ordinary_tab_views()) {
        if ((tab_id.has_value() && tab->view_id() != *tab_id) || tab->session().extension_manager.ptr() != this)
            continue;
        if (!tab->has_traversable()) {
            if (tab_id.has_value())
                return JsonValue {};
            continue;
        }
        JsonArray frames;
        JsonValue match;
        tab->traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& frame) {
            auto const& doc = frame.active_document();
            if (!doc.host() || doc.unload_counter() || (frame.parent() && frame.container_document() != &frame.parent()->active_document()))
                return IterationDecision::Continue;
            if (single && ((frame_id.has_value() && frame.extension_frame_id() != *frame_id) || (document_id.has_value() && doc.extension_document_id() != *document_id)))
                return IterationDecision::Continue;
            auto result = navigation_frame_details(frame);
            result.set("errorOccurred"sv, doc.creation_url() == URL::about_error());
            if (single) {
                match = move(result);
                return IterationDecision::Break;
            }
            frames.must_append(move(result));
            return IterationDecision::Continue;
        });
        if (!single)
            return JsonValue(move(frames));
        if (!match.is_null())
            return match;
    }
    return Error::from_string_literal("No matching tab or frame");
}

void ExtensionManager::dispatch_navigation_event(CanonicalNavigable const& frame, StringView event, JsonObject extra)
{
    auto tab = frame.top_level_traversable().view();
    if (!m_extensions_enabled || !tab.has_value() || tab->is_private() == IsPrivate::Yes || tab->owner_view_id().has_value()
        || is_extension_background_view(*tab) || tab->session().extension_manager.ptr() != this)
        return;
    Optional<String> arguments;
    for (auto const& [id, ext] : m_extensions_by_id) {
        if (!ext.enabled || !ext.manifest.has_permission("webNavigation"sv))
            continue;
        if (!arguments.has_value()) {
            JsonObject details;
            if (event != "onCreatedNavigationTarget"sv) {
                details = navigation_frame_details(frame);
                if (event == "onBeforeNavigate"sv) {
                    details.remove("documentId"sv);
                    details.remove("documentLifecycle"sv);
                }
            }
            extra.for_each_member([&](String const& key, JsonValue const& value) { details.set(key, value); });
            details.set("timeStamp"sv, UnixDateTime::now().milliseconds_since_epoch());
            details.set("tabId"sv, tab->view_id());
            JsonArray args;
            args.must_append(move(details));
            arguments = args.serialized();
        }
        dispatch_event_to_extension(id, "webNavigation"sv, event, *arguments);
    }
}

}
