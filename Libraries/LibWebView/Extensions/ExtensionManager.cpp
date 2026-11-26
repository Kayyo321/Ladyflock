/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibRequests/RequestControlClient.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <LibWebView/Extensions/MozillaAddonTrust.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

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

ErrorOr<NonnullOwnPtr<ExtensionManager>> ExtensionManager::create(ByteString profile_data_root, Database::Database* shared_database)
{
    NonnullOwnPtr<ExtensionStorage> storage = shared_database
        ? TRY(ExtensionStorage::create(*shared_database))
        : ExtensionStorage::create();
    return adopt_own(*new ExtensionManager(move(profile_data_root), move(storage)));
}

ExtensionManager::ExtensionManager(ByteString profile_data_root, NonnullOwnPtr<ExtensionStorage> storage)
    : m_profile_data_root(move(profile_data_root))
    , m_extensions_root(LexicalPath::join(m_profile_data_root, "extensions"sv).string())
    , m_storage(move(storage))
    , m_installer(m_extensions_root, m_revocation_list)
{
    m_installer.add_trusted_root_pem(MUST(String::from_utf8(mozilla_addon_signing_root)));
    m_web_request_coordinator.set_on_snapshot_changed([this](Web::Extensions::WebRequestWatchSnapshot const& snapshot) {
        push_watch_snapshot_everywhere(snapshot);
    });
    m_web_request_coordinator.set_permission_check_callback([this](StringView ext_id, URL::URL const& url) {
        return has_host_permission(ext_id, url);
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
    m_pending_event_replies.clear();
    m_pending_script_executions.clear();

    Vector<String> keys;
    for (auto const& [ext_id, _] : m_background_hosts)
        keys.append(ext_id);
    for (auto const& ext_id : keys)
        stop_background_page(ext_id);
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
    auto ext_id = ext.extension_id;
    auto uuid = ext.uuid;
    auto manifest = ext.manifest;
    bool enabled = ext.enabled;
    bool pinned = m_action_controller.resolved_state(ext_id, {}, {}).pinned;

    m_id_by_uuid.set(uuid, ext_id);
    m_persisted_uuid_by_id.set(ext_id, uuid);
    m_extensions_by_id.set(ext_id, move(ext));

    m_action_controller.configure_from_manifest(ext_id, manifest);
    m_action_controller.set_pinned(ext_id, pinned);
    if (enabled) {
        m_sidebar_controller.configure_from_manifest(ext_id, manifest, first_install, current_window_id());
        if (manifest.background().has_value() && manifest.background()->has_background() && manifest.background()->persistent)
            ensure_background_started(ext_id, [](bool) { });
    }
    notify_chrome_changed();
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
        return m_installer.stage_package(move(bytes), listing.addon_id, listing.version, listing.sha256_hex, installed->manifest, installed->sha256_hex, diagnostic);
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
    return m_installer.stage_xpi(xpi_path, existing->extension_id, existing->manifest, existing->sha256_hex, diagnostic);
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
        InstalledExtension updated = old_entry;
        updated.install_directory = committed.install_directory;
        updated.manifest = move(manifest);
        updated.compatibility_status = committed.compatibility_status;
        updated.sha256_hex = committed.sha256_hex;
        updated.verified_signature = true;
        updated.grant_generation = old_entry.grant_generation + 1;
        updated.active_tab_ids.clear();
        next = move(updated);
        return save_registry(&*next);
    }));

    stop_background_page(ext_id);
    broadcast_world_revocation(ext_id, old_entry.grant_generation + 1);
    m_web_request_coordinator.remove_all_for_extension(ext_id);
    register_extension_entry(next.release_value());
    dispatch_installed_event(*m_extensions_by_id.get(ext_id), old_entry.manifest.version());
    return *m_extensions_by_id.get(ext_id);
}

ErrorOr<void> ExtensionManager::set_extension_enabled(StringView extension_id, bool enabled)
{
    auto it = m_extensions_by_id.find(extension_id);
    if (it == m_extensions_by_id.end())
        return Error::from_string_literal("Extension not found");

    if (it->value.enabled == enabled)
        return {};

    it->value.enabled = enabled;
    ++it->value.grant_generation;
    auto gen = it->value.grant_generation;

    if (!enabled) {
        it->value.active_tab_ids.clear();
        m_sidebar_controller.remove_extension(extension_id);
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

    notify_chrome_changed();
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
    auto ext = m_extensions_by_id.take(extension_id);
    if (!ext.has_value())
        return Error::from_string_literal("Extension not found");

    stop_background_page(extension_id);
    m_web_request_coordinator.remove_all_for_extension(extension_id);
    broadcast_world_revocation(extension_id, ext->grant_generation + 1);
    m_action_controller.remove_extension(extension_id);
    m_sidebar_controller.remove_extension(extension_id);
    m_storage->delete_extension_data(extension_id);
    m_id_by_uuid.remove(ext->uuid);
    m_persisted_uuid_by_id.remove(extension_id);

    if (ext->source == Web::Extensions::InstallSource::SignedXpi)
        m_installer.remove_installed_package(ext->install_directory);

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
        for (auto const& script : ext->manifest.background()->scripts)
            html.appendff("<script src=\"/{}\"></script>", escape_html_entities(script.bytes_as_string_view()));
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

    if (url.scheme().is_one_of("about"sv, "resource"sv, "moz-extension"sv))
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

Vector<Web::Extensions::AttachedContentScript> ExtensionManager::content_scripts_for_document(URL::URL const& document_url, bool is_top_level, Web::HTML::CrossProcessId document_id) const
{
    Vector<Web::Extensions::AttachedContentScript> attached;
    if (!m_extensions_enabled)
        return attached;

    if (document_url.scheme() == "moz-extension"sv || document_url.scheme() == "about"sv)
        return attached;

    auto doc_host = document_url.serialized_host();
    for (auto const& [ext_id, ext] : m_extensions_by_id) {
        if (!ext.enabled || ext.revoked_hosts.contains(doc_host))
            continue;

        for (auto const& rule : ext.manifest.content_scripts()) {
            if (!is_top_level && !rule.all_frames)
                continue;
            if (!ext.allow_all_sites && !ext.granted_extra_hosts.contains(doc_host))
                continue;

            bool matched = false;
            for (auto const& pat : rule.matches) {
                if (pat.matches(document_url)) {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                continue;

            for (auto const& ex : rule.exclude_matches) {
                if (ex.matches(document_url)) {
                    matched = false;
                    break;
                }
            }
            if (!matched)
                continue;

            if (!rule.include_globs.is_empty()) {
                bool glob_ok = false;
                for (auto const& glob : rule.include_globs) {
                    if (Web::Extensions::MatchPattern::matches_glob(document_url.serialize(), glob)) {
                        glob_ok = true;
                        break;
                    }
                }
                if (!glob_ok)
                    continue;
            }
            for (auto const& glob : rule.exclude_globs) {
                if (Web::Extensions::MatchPattern::matches_glob(document_url.serialize(), glob)) {
                    matched = false;
                    break;
                }
            }
            if (!matched)
                continue;

            Web::Extensions::AttachedContentScript script;
            script.extension_id = ext.extension_id;
            script.extension_uuid = ext.uuid;
            script.grant_generation = ext.grant_generation;
            script.document_id = document_id;
            script.run_at = rule.run_at;
            script.all_frames = rule.all_frames;
            script.match_about_blank = rule.match_about_blank;

            for (auto const& js_rel : rule.js) {
                if (auto content = read_package_script(ext.install_directory, js_rel); !content.is_error())
                    script.js_files.append({ .path = js_rel, .content = content.release_value() });
            }
            for (auto const& css_rel : rule.css) {
                if (auto content = read_package_script(ext.install_directory, css_rel); !content.is_error())
                    script.css_files.append({ .path = css_rel, .content = content.release_value() });
            }

            if (!script.js_files.is_empty() || !script.css_files.is_empty())
                attached.append(move(script));
        }
    }
    return attached;
}

void ExtensionManager::attach_content_scripts_for_document(WebContentPage& page, URL::URL const& document_url, bool is_top_level, Web::HTML::CrossProcessId document_id) const
{
    auto document = page.hosted_document_with_id(document_id);
    if (!document)
        return;
    auto scripts = content_scripts_for_document(document_url, is_top_level, document_id);
    for (auto const& script : scripts)
        document->authorize_content_script(script.extension_id, script.grant_generation);
    if (!scripts.is_empty())
        page.client().async_attach_extension_content_scripts(page.id(), document_id, move(scripts));
}

void ExtensionManager::revoke_document_worlds(WebContentPage& page, Web::HTML::CrossProcessId document_id)
{
    for (auto const& [ext_id, ext] : m_extensions_by_id)
        page.client().async_revoke_extension_world(page.id(), ext_id, document_id, ext.grant_generation);
}

void ExtensionManager::broadcast_world_revocation(StringView extension_id, u64 new_grant_generation, bool include_extension_pages)
{
    auto ext_str = MUST(String::from_utf8(extension_id));
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
    bool enabled = !snapshot.entries.is_empty();
    bool match_all_urls = false;
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
        if (client.is_private() == IsPrivate::No)
            client.async_set_web_request_watch_snapshot(snapshot);
        return IterationDecision::Continue;
    });
    RequestServerManager::the().for_each_instance([&](RequestServerInstance& instance) {
        if (instance.session().is_private() == IsPrivate::No)
            push_watch_filter_to_request_server(instance.control_client(), snapshot);
        return IterationDecision::Continue;
    });
}

bool ExtensionManager::is_extension_background_view(ViewImplementation const& view)
{
    return background_views().contains(&view);
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

void ExtensionManager::stop_background_page(StringView extension_id)
{
    auto host = m_background_hosts.take(extension_id);
    if (!host.has_value())
        return;
    if (host.value()->startup_timer)
        host.value()->startup_timer->stop();
    if (host.value()->idle_timer)
        host.value()->idle_timer->stop();
    if (host.value()->view)
        background_views().remove(host.value()->view.ptr());
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
                        bool storage_listener = ns == "storage"sv && document.has_content_script_grant(extension_id, extension->grant_generation);
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
                        has_value = !response.is_error() && response.value().is_object() && response.value().as_object().get_bool("cancel"sv) == true;
                    }
                    if (has_value || --replies->remaining == 0) {
                        auto complete = move(replies->callback);
                        complete(has_value, has_value ? result : "null"_string);
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
    auto pending = m_pending_script_executions.take(execution_id);
    auto caller = pending->caller.strong_ref();
    if (!caller)
        return;

    if (!success) {
        caller->async_extension_api_response(pending->page_id, pending->document_id, pending->call_id, false, JsonValue(result_or_error_json).serialized());
        return;
    }

    if (pending->expect_array_results) {
        auto parsed = JsonValue::from_string(result_or_error_json);
        JsonObject wrapped;
        wrapped.set("frameId"sv, 0);
        wrapped.set("result"sv, parsed.is_error() ? JsonValue {} : parsed.release_value());
        JsonArray arr;
        arr.must_append(move(wrapped));
        caller->async_extension_api_response(pending->page_id, pending->document_id, pending->call_id, true, arr.serialized());
    } else {
        auto parsed = JsonValue::from_string(result_or_error_json);
        JsonArray arr;
        arr.must_append(parsed.is_error() ? JsonValue {} : parsed.release_value());
        caller->async_extension_api_response(pending->page_id, pending->document_id, pending->call_id, true, arr.serialized());
    }
}

void ExtensionManager::handle_web_request_check(WebContentClient& caller, Web::PageId page_id, u64 check_id, Web::Extensions::WebRequestBeforeRequestDetails const& requested_details)
{
    auto* page = caller.page(page_id);
    if (!page || !page->is_open() || !page->has_traversable()) {
        caller.async_did_check_web_request_before_request(page_id, check_id, { .cancel = false });
        return;
    }
    auto details = requested_details;
    details.tab_id = tab_id_for_page(caller, page_id).value_or(0);
    details.initiator_origin.clear();
    details.document_url.clear();
    auto document = details.source_document_id.has_value() ? page->hosted_document_with_id(*details.source_document_id) : nullptr;
    if (document) {
        details.initiator_origin = document->origin().serialize();
        details.document_url = page->traversable().active_document().creation_url().serialize();
    } else if (details.type != Web::Extensions::WebRequestResourceType::MainFrame) {
        caller.async_did_check_web_request_before_request(page_id, check_id, { .cancel = false });
        return;
    }
    details.frame_id = 0;
    details.parent_frame_id = 0;
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

    Optional<u64> target_tab_id;
    if (auto target_obj = details_obj.get_object("target"sv); target_obj.has_value()) {
        if (target_obj->get_bool("allFrames"sv) == true || target_obj->has("frameIds"sv) || target_obj->has("documentIds"sv)) {
            send_err("Only the main frame is supported for script injection");
            return;
        }
        target_tab_id = target_obj->get_u64("tabId"sv);
    }
    if (details_obj.get_bool("allFrames"sv) == true || details_obj.get_u64("frameId"sv).value_or(0) != 0
        || (details_obj.has("world"sv) && details_obj.get_string("world"sv) != "ISOLATED"sv)) {
        send_err("Unsupported script injection target or world");
        return;
    }

    auto target_view = target_tab_id.has_value() ? ViewImplementation::find_view_by_id(*target_tab_id) : default_extension_target(*this);
    if (!target_view.has_value() || !target_view->has_traversable() || !tab_info_for_extension(ext.extension_id, *target_view).has_value()) {
        send_err("Target tab not found");
        return;
    }

    if (!has_host_permission(ext.extension_id, target_view->url(), target_view->view_id())) {
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

    auto& target_page = target_view->traversable().active_document().host()
        ? *target_view->traversable().active_document().host()
        : target_view->page();
    auto target_doc_id = target_view->traversable().active_document().id();

    u64 exec_id = m_next_execution_id++;
    target_view->traversable().active_document().authorize_content_script(ext.extension_id, ext.grant_generation);
    m_pending_script_executions.set(exec_id, PendingScriptExecution {
                                                 .caller = caller.make_weak_ptr<WebContentClient>(),
                                                 .page_id = page_id,
                                                 .document_id = source_document_id,
                                                 .call_id = call_id,
                                                 .expect_array_results = modern_api,
                                                 .renderer = target_page.client().make_weak_ptr<WebContentClient>(),
                                                 .target_page_id = target_page.id(),
                                             });

    target_page.client().async_execute_extension_script(
        target_page.id(),
        exec_id,
        ext.extension_id,
        ext.uuid,
        ext.grant_generation,
        sources,
        func_source,
        func_args,
        target_doc_id);
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

    Optional<u64> target_tab_id;
    if (auto target_obj = details_obj.get_object("target"sv); target_obj.has_value()) {
        if (target_obj->get_bool("allFrames"sv) == true || target_obj->has("frameIds"sv) || target_obj->has("documentIds"sv)) {
            send_err("Only the main frame is supported for CSS injection");
            return;
        }
        target_tab_id = target_obj->get_u64("tabId"sv);
    }
    if (details_obj.get_bool("allFrames"sv) == true || details_obj.get_u64("frameId"sv).value_or(0) != 0
        || details_obj.get_string("origin"sv) == "USER"sv || details_obj.get_string("cssOrigin"sv) == "user"sv) {
        send_err("Unsupported CSS injection target or style origin");
        return;
    }
    auto target_view = target_tab_id.has_value() ? ViewImplementation::find_view_by_id(*target_tab_id) : default_extension_target(*this);
    if (!target_view.has_value() || !target_view->has_traversable() || !tab_info_for_extension(ext.extension_id, *target_view).has_value()) {
        send_err("Target tab not found");
        return;
    }

    if (!has_host_permission(ext.extension_id, target_view->url(), target_view->view_id())) {
        send_err("Extension lacks host/activeTab permission for target tab");
        return;
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

    auto& target_page = target_view->traversable().active_document().host()
        ? *target_view->traversable().active_document().host()
        : target_view->page();
    auto target_doc_id = target_view->traversable().active_document().id();
    target_page.client().async_inject_extension_css(target_page.id(), ext.extension_id, css_key, css_text, remove, target_doc_id);
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
        if (!(namespace_name == "runtime"sv && method_name == "sendMessage"sv) && namespace_name != "storage.local"sv) {
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

    auto parsed_args = JsonValue::from_string(args_json);
    JsonArray empty_arr;
    JsonObject empty_obj;

    if (namespace_name == "runtime"sv) {
        if (method_name == "sendMessage"sv) {
            if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty()) {
                reply_err("runtime.sendMessage requires a message argument");
                return;
            }
            auto msg = parsed_args.value().as_array().at(0);
            JsonObject sender;
            sender.set("id"sv, ext->extension_id);
            if (auto tab_id = tab_id_for_page(caller, page_id); tab_id.has_value()) {
                if (auto view = ViewImplementation::find_view_by_id(*tab_id); view.has_value()) {
                    if (auto tinfo = tab_info_for_extension(ext->extension_id, *view); tinfo.has_value()) {
                        JsonObject tab_obj;
                        tab_obj.set("id"sv, static_cast<i64>(tinfo->tab_id));
                        tab_obj.set("windowId"sv, static_cast<i64>(tinfo->window_id));
                        if (tinfo->url.has_value())
                            tab_obj.set("url"sv, *tinfo->url);
                        if (tinfo->title.has_value())
                            tab_obj.set("title"sv, *tinfo->title);
                        sender.set("tab"sv, move(tab_obj));
                    }
                }
            }
            JsonArray ev_args;
            ev_args.must_append(msg);
            ev_args.must_append(move(sender));

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
        reply_err("Unsupported runtime API method");
        return;
    }

    if (namespace_name == "storage"sv || namespace_name == "storage.local"sv) {
        if (!ext->manifest.has_permission("storage"sv)) {
            reply_err("Extension lacks 'storage' permission");
            return;
        }
        auto normalized_method = method_name.bytes_as_string_view();
        if (normalized_method.starts_with("local."sv))
            normalized_method = normalized_method.substring_view(6);

        if (normalized_method == "get"sv) {
            JsonValue keys_spec;
            if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty())
                keys_spec = parsed_args.value().as_array().at(0);
            reply_ok(JsonValue(m_storage->get(extension_id, keys_spec)));
            return;
        }
        if (normalized_method == "set"sv) {
            JsonObject items;
            if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty() && parsed_args.value().as_array().at(0).is_object())
                items = parsed_args.value().as_array().at(0).as_object();
            auto changes = m_storage->set(extension_id, items);
            if (!changes.is_empty()) {
                JsonArray ev_args;
                ev_args.must_append(JsonValue(move(changes)));
                ev_args.must_append("local"_string);
                dispatch_event_to_extension(ext->extension_id, "storage"sv, "onChanged"sv, ev_args.serialized());
            }
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
            auto changes = m_storage->remove(extension_id, keys);
            if (!changes.is_empty()) {
                JsonArray ev_args;
                ev_args.must_append(JsonValue(move(changes)));
                ev_args.must_append("local"_string);
                dispatch_event_to_extension(ext->extension_id, "storage"sv, "onChanged"sv, ev_args.serialized());
            }
            reply_ok(JsonValue {});
            return;
        }
        if (normalized_method == "clear"sv) {
            auto changes = m_storage->clear(extension_id);
            if (!changes.is_empty()) {
                JsonArray ev_args;
                ev_args.must_append(JsonValue(move(changes)));
                ev_args.must_append("local"_string);
                dispatch_event_to_extension(ext->extension_id, "storage"sv, "onChanged"sv, ev_args.serialized());
            }
            reply_ok(JsonValue {});
            return;
        }
        if (normalized_method == "getBytesInUse"sv) {
            auto serialized = m_storage->get(extension_id, JsonValue {}).serialized();
            reply_ok(JsonValue(static_cast<i64>(serialized.bytes().size())));
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

    if (namespace_name == "windows"sv && method_name == "getCurrent"sv) {
        auto window_id = caller_window();
        auto active = Application::the().active_web_view();
        JsonObject window;
        window.set("id"sv, window_id);
        window.set("focused"sv, active.has_value() && active->extension_window_id() == window_id);
        window.set("incognito"sv, false);
        window.set("type"sv, "normal"sv);
        if (!parsed_args.is_error() && parsed_args.value().is_array() && !parsed_args.value().as_array().is_empty()) {
            auto const& options = parsed_args.value().as_array().at(0);
            if (options.is_object() && options.as_object().get_bool("populate"sv) == true) {
                JsonArray tabs;
                for (auto* view : ordinary_tab_views()) {
                    if (view->extension_window_id() != window_id)
                        continue;
                    if (auto tab = tab_info_for_extension(extension_id, *view); tab.has_value())
                        tabs.must_append(serialize_tab(*tab));
                }
                window.set("tabs"sv, move(tabs));
            }
        }
        reply_ok(JsonValue(move(window)));
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

            auto& target_page = view->traversable().active_document().host()
                ? *view->traversable().active_document().host()
                : view->page();
            bind_event_reply(rid, target_page);
            target_page.client().async_dispatch_extension_event(
                target_page.id(),
                ext->extension_id,
                "runtime"_string,
                "onMessage"_string,
                ev_args.serialized(),
                rid,
                view->traversable().active_document().id());
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

    if (namespace_name == "scripting"sv) {
        if (!ext->manifest.has_permission("scripting"sv)) {
            reply_err("Extension lacks 'scripting' permission");
            return;
        }
        if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty() || !parsed_args.value().as_array().at(0).is_object()) {
            reply_err("scripting method requires injection details object");
            return;
        }
        auto const& details = parsed_args.value().as_array().at(0).as_object();
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

    auto icon_from_details = [&](JsonObject const& details) -> ErrorOr<Optional<String>> {
        auto path = details.get("path"sv);
        if (!path.has_value())
            return Error::from_string_literal("setIcon requires a package path or size-to-path object");
        if (path->is_null())
            return Optional<String> {};
        auto icons = TRY(Web::Extensions::ExtensionManifest::parse_icon_map(*path));
        auto selected = Web::Extensions::ExtensionManifest::preferred_icon(icons);
        if (!selected.has_value())
            return Error::from_string_literal("No valid icon size was supplied");
        return selected;
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
            auto path = icon_from_details(arg_obj);
            if (path.is_error()) {
                reply_err("setIcon requires a package path or size-to-path object");
                return;
            }
            m_action_controller.set_icon(extension_id, path.release_value(), tab_id, window_id);
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
            auto path = icon_from_details(arg_obj);
            if (path.is_error()) {
                reply_err("setIcon requires a package path or size-to-path object");
                return;
            }
            m_sidebar_controller.set_icon(extension_id, path.release_value(), tab_id, scope);
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
        if (method_name != "contains"sv && method_name != "remove"sv) {
            reply_err("Unsupported permissions method");
            return;
        }
        if (parsed_args.is_error() || !parsed_args.value().is_array() || parsed_args.value().as_array().is_empty() || !parsed_args.value().as_array().at(0).is_object()) {
            reply_err("Permissions API requires a permissions object");
            return;
        }
        auto const& request = parsed_args.value().as_array().at(0).as_object();
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
            if (blocking && !ext->manifest.has_permission("webRequestBlocking"sv)) {
                reply_err("Blocking webRequest listener requires 'webRequestBlocking' permission");
                return;
            }
            Vector<String> urls;
            if (auto arr = obj.get_array("urls"sv); arr.has_value()) {
                for (auto const& item : arr->values()) {
                    if (item.is_string())
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
            m_web_request_coordinator.add_listener(ext->extension_id, listener_id, ext->grant_generation, move(urls), move(types), blocking);
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
        reply_err("Unsupported webRequest method");
        return;
    }

    reply_err("Unsupported extension API namespace");
}

void ExtensionManager::notify_tab_created(ViewImplementation const& view)
{
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

    for (auto const& [ext_id, ext] : m_extensions_by_id) {
        if (ext.enabled)
            dispatch_event_to_extension(ext_id, "tabs"sv, "onActivated"sv, serialized);
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
        item.set("enabled"sv, ext.enabled);
        item.set("allow_all_sites"sv, ext.allow_all_sites);
        item.set("verified_signature"sv, ext.verified_signature);
        item.set("has_options"sv, ext.enabled && ext.manifest.options_ui().has_value());
        item.set("can_check_update"sv, ext.from_catalog && m_catalog_client.enable_public_amo_catalog());
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

}
