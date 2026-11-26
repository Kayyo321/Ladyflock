/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWebCommon/Extensions/SupportMatrix.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <LibWebView/ImageDecoderManager.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI/AddonsUI.h>

namespace WebView {

static Extensions::ExtensionManager* manager()
{
    if (!Application::is_initialized())
        return nullptr;
    return Application::default_session().extension_manager.ptr();
}

Extensions::ExtensionManager* AddonsUI::extension_manager() const
{
    if (!has_client() || client().session().is_private() == IsPrivate::Yes)
        return nullptr;
    return client().session().extension_manager.ptr();
}

AddonsUI::~AddonsUI()
{
    if (auto* ext_mgr = extension_manager()) {
        for (auto const& token : m_pending_tokens)
            (void)ext_mgr->cancel_install(token);
    }
    if (m_icon_decoder_generation.has_value() && Application::is_initialized())
        Application::image_decoder_manager().process_did_release_decoder(0, *m_icon_decoder_generation);
}

void AddonsUI::register_interfaces()
{
    register_interface("getInstalledExtensions"sv, [this](auto const& data) { get_installed_extensions(data); });
    register_interface("setExtensionEnabled"sv, [this](auto const& data) { set_extension_enabled(data); });
    register_interface("removeExtension"sv, [this](auto const& data) { remove_extension(data); });
    register_interface("setExtensionSiteAccess"sv, [this](auto const& data) { set_extension_site_access(data); });
    register_interface("searchCatalog"sv, [this](auto const& data) { search_catalog(data); });
    register_interface("getExtensionIcon"sv, [this](auto const& data) { get_extension_icon(data); });
    register_interface("pickLocalXpi"sv, [this](auto const& data) { pick_local_xpi(data); });
    register_interface("stageCatalogInstall"sv, [this](auto const& data) { stage_catalog_install(data); });
    register_interface("approveInstall"sv, [this](auto const& data) { approve_install(data); });
    register_interface("cancelInstall"sv, [this](auto const& data) { cancel_install(data); });
    register_interface("checkManualUpdate"sv, [this](auto const& data) { stage_manual_update(data); });
    register_interface("approveUpdate"sv, [this](auto const& data) { approve_update(data); });
    register_interface("checkForUpdates"sv, [this](auto const& data) { check_for_updates(data); });
    register_interface("stageUpdates"sv, [this](auto const& data) { stage_updates(data); });
    register_interface("approveUpdates"sv, [this](auto const& data) { approve_updates(data); });
    register_interface("cancelUpdates"sv, [this](auto const& data) { cancel_updates(data); });
    register_interface("openExtensionOptions"sv, [this](auto const& data) { open_extension_options(data); });
    register_interface("getSupportMatrix"sv, [this](auto const& data) { get_support_matrix(data); });
}

void AddonsUI::get_installed_extensions(JsonValue const&)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr) {
        JsonObject response;
        response.set("error"sv, "Extensions are unavailable in private browsing."_string);
        async_send_message("installedExtensions"_string, JsonValue(move(response)));
        return;
    }
    async_send_message("installedExtensions"_string, JsonValue(ext_mgr->summary_json()));
}

void AddonsUI::set_extension_enabled(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || !data.is_object())
        return;
    auto id = data.as_object().get_string("extension_id"sv);
    auto enabled = data.as_object().get_bool("enabled"sv);
    if (!id.has_value() || !enabled.has_value())
        return;

    JsonObject resp;
    if (auto err = ext_mgr->set_extension_enabled(*id, *enabled); err.is_error())
        resp.set("error"sv, MUST(String::formatted("{}", err.error())));
    else
        resp.set("summary"sv, ext_mgr->summary_json());
    async_send_message("extensionStateUpdated"_string, JsonValue(move(resp)));
}

void AddonsUI::remove_extension(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || !data.is_object())
        return;
    auto id = data.as_object().get_string("extension_id"sv);
    if (!id.has_value())
        return;

    JsonObject resp;
    if (auto err = ext_mgr->remove_extension(*id); err.is_error())
        resp.set("error"sv, MUST(String::formatted("{}", err.error())));
    else
        resp.set("summary"sv, ext_mgr->summary_json());
    async_send_message("extensionRemoved"_string, JsonValue(move(resp)));
}

void AddonsUI::set_extension_site_access(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || !data.is_object())
        return;
    auto const& obj = data.as_object();
    auto id = obj.get_string("extension_id"sv);
    if (!id.has_value())
        return;
    bool allow_all = obj.get_bool("allow_all_sites"sv).value_or(true);
    auto host = obj.get_string("host"sv);
    Optional<String> host_copy;
    if (host.has_value())
        host_copy = *host;
    bool grant = obj.get_bool("grant"sv).value_or(true);

    JsonObject resp;
    if (auto err = ext_mgr->set_site_access(*id, allow_all, move(host_copy), grant); err.is_error())
        resp.set("error"sv, MUST(String::formatted("{}", err.error())));
    else
        resp.set("summary"sv, ext_mgr->summary_json());
    async_send_message("extensionSiteAccessUpdated"_string, JsonValue(move(resp)));
}

static JsonArray string_array(ReadonlySpan<String> values)
{
    JsonArray array;
    for (auto const& value : values)
        array.must_append(value);
    return array;
}

void AddonsUI::search_catalog(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr) {
        JsonObject err;
        err.set("error"sv, "Extensions manager unavailable"_string);
        async_send_message("catalogSearchResults"_string, JsonValue(move(err)));
        return;
    }

    if (!data.is_object())
        return;
    auto const& arguments = data.as_object();
    auto query = arguments.get_string("query"sv).value_or({});
    auto page = arguments.get_u32("page"sv).value_or(1);
    auto request_id = arguments.get("request_id"sv).value_or(JsonValue {});

    auto generation = ++m_catalog_search_generation;
    m_catalog_icon_urls.clear();
    ext_mgr->catalog_client().search(query, page, [weak = make_weak_ptr<AddonsUI>(), request_id = move(request_id), generation](ErrorOr<Extensions::CatalogSearchResult> res) {
        auto self = weak.strong_ref();
        if (!self || !self->has_client())
            return;
        JsonObject out;
        out.set("request_id"sv, request_id);
        if (res.is_error()) {
            out.set("error"sv, MUST(String::formatted("{}", res.error())));
        } else {
            JsonArray arr;
            for (auto const& e : res.value().entries) {
                JsonObject o;
                o.set("addon_id"sv, e.addon_id);
                o.set("slug"sv, e.slug);
                o.set("name"sv, e.name);
                o.set("summary"sv, e.summary);
                o.set("version"sv, e.version);
                auto compatibility = e.compatibility_status == Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested
                    ? "unverified"sv
                    : Web::Extensions::to_string(e.compatibility_status);
                o.set("compatibility_status"sv, MUST(String::from_utf8(compatibility)));
                o.set("compatibility_reasons"sv, string_array(e.compatibility_reasons.span()));
                o.set("listing_url"sv, e.listing_url);
                o.set("author"sv, e.author);
                if (e.icon_url.has_value() && generation == self->m_catalog_search_generation && self->m_catalog_icon_urls.size() < 50) {
                    auto key = MUST(String::formatted("catalog:{}:{}", generation, arr.size()));
                    self->m_catalog_icon_urls.set(key, *e.icon_url);
                    o.set("icon_key"sv, key);
                }
                arr.must_append(move(o));
            }
            out.set("results"sv, move(arr));
            out.set("has_more"sv, res.value().has_more);
        }
        self->async_send_message("catalogSearchResults"_string, JsonValue(move(out)));
    });
}

void AddonsUI::get_extension_icon(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || !data.is_object())
        return;
    auto key = data.as_object().get_string("icon_key"sv);
    if (!key.has_value() || key->bytes().size() > 1024 || m_pending_icons.contains(*key))
        return;
    if (m_pending_icons.size() >= 50) {
        send_extension_icon(*key);
        return;
    }
    m_pending_icons.set(*key);

    if (auto url = m_catalog_icon_urls.get(*key); url.has_value()) {
        ext_mgr->catalog_client().download_icon(*url, [weak = make_weak_ptr<AddonsUI>(), key = *key](ErrorOr<ByteBuffer> image) {
            if (auto self = weak.strong_ref())
                self->decode_extension_icon(key, move(image), ByteString("image/png"sv));
        });
        return;
    }

    auto read_icon = [&](ByteString const& directory, StringView path) {
        Optional<ByteString> mime_type;
        if (path.ends_with(".svg"sv, CaseSensitivity::CaseInsensitive))
            mime_type = ByteString("image/svg+xml"sv);
        decode_extension_icon(*key, Extensions::ExtensionManager::read_package_icon(directory, path), move(mime_type));
    };
    auto key_view = key->bytes_as_string_view();
    if (key_view.starts_with("installed:"sv)) {
        auto suffix = key_view.substring_view("installed:"sv.length());
        if (auto separator = suffix.find(':'); separator.has_value()) {
            auto extension = ext_mgr->extension_by_uuid(suffix.substring_view(0, *separator));
            if (extension.has_value() && extension->manifest.version() == suffix.substring_view(*separator + 1)) {
                if (auto path = extension->manifest.package_icon_path(); path.has_value())
                    return read_icon(extension->install_directory, *path);
            }
        }
    } else if (key_view.starts_with("staged:"sv)) {
        auto token = key_view.substring_view("staged:"sv.length());
        if (m_pending_tokens.contains(MUST(String::from_utf8(token)))) {
            if (auto staged = ext_mgr->installer().staged_install(token); staged.has_value() && staged->icon_path.has_value())
                return read_icon(LexicalPath::join(staged->staged_directory, "payload"sv).string(), *staged->icon_path);
        }
    }
    send_extension_icon(*key);
}

void AddonsUI::decode_extension_icon(String key, ErrorOr<ByteBuffer> bytes, Optional<ByteString> mime_type)
{
    if (!has_client() || !is_open() || bytes.is_error() || bytes.value().is_empty()) {
        send_extension_icon(key);
        return;
    }
    if (!m_icon_decoder || !m_icon_decoder->is_open()) {
        auto& manager = Application::image_decoder_manager();
        auto handle = manager.connect_new_client(0, &client().session(), {});
        if (handle.is_error()) {
            send_extension_icon(key);
            return;
        }
        manager.note_connected_client(0, m_icon_decoder_generation);
        auto transport = handle.value().create_transport();
        if (transport.is_error()) {
            send_extension_icon(key);
            return;
        }
        m_icon_decoder = make_ref_counted<ImageDecoderClient::Client>(transport.release_value());
    }
    auto weak = make_weak_ptr<AddonsUI>();
    (void)m_icon_decoder->decode_image(bytes.value().bytes(), [weak, key](auto& image) -> ErrorOr<void> {
        auto self = weak.strong_ref();
        if (!self)
            return {};
        if (image.session_id && self->m_icon_decoder)
            self->m_icon_decoder->stop_animation_decode(image.session_id);
        if (!image.frames.is_empty()) {
            auto& bitmap = *image.frames.first().bitmap;
            if (bitmap.width() <= 512 && bitmap.height() <= 512) {
                auto png = Gfx::PNGWriter::encode(bitmap);
                if (!png.is_error() && png.value().size() <= 256 * KiB) {
                    auto encoded = encode_base64(png.value().bytes());
                    if (!encoded.is_error()) {
                        self->send_extension_icon(key, MUST(String::formatted("data:image/png;base64,{}", encoded.value())));
                        return {};
                    }
                }
            }
        }
        self->send_extension_icon(key);
        return {}; }, [weak, key](Error&) {
        if (auto self = weak.strong_ref())
            self->send_extension_icon(key); }, Gfx::IntSize { 96, 96 }, move(mime_type));
}

void AddonsUI::send_extension_icon(String const& key, Optional<String> data_url)
{
    m_pending_icons.remove(key);
    if (!has_client() || !is_open())
        return;
    JsonObject result;
    result.set("icon_key"sv, key);
    if (data_url.has_value())
        result.set("icon_data_url"sv, *data_url);
    async_send_message("extensionIcon"_string, JsonValue(move(result)));
}

void AddonsUI::pick_local_xpi(JsonValue const& data)
{
    if (!extension_manager() || m_picker_open)
        return;
    auto* page = client().page(page_id());
    if (!page || !page->displays_tab())
        return;
    Optional<String> update_id;
    if (data.is_object()) {
        if (auto id = data.as_object().get_string("extension_id"sv); id.has_value())
            update_id = *id;
    }
    m_picker_open = true;
    page->view().pick_local_extension_package([weak = make_weak_ptr<AddonsUI>(), update_id = move(update_id)](Optional<ByteString> path) {
        auto self = weak.strong_ref();
        if (!self)
            return;
        self->m_picker_open = false;
        auto* ext_mgr = self->extension_manager();
        if (!ext_mgr)
            return;
        if (!path.has_value()) {
            JsonObject response;
            response.set("cancelled"sv, true);
            self->async_send_message(update_id.has_value() ? "updateStaged"_string : "installStaged"_string, JsonValue(move(response)));
            return;
        }
        Extensions::InstallDiagnostic diagnostic;
        auto staged = update_id.has_value() ? ext_mgr->stage_update(*update_id, *path, &diagnostic) : ext_mgr->stage_xpi(*path, {}, &diagnostic);
        self->send_staged_install(move(staged), update_id.has_value(), diagnostic);
    });
}

void AddonsUI::stage_catalog_install(JsonValue const& data)
{
    if (!data.is_object())
        return;
    if (auto id = data.as_object().get_string("extension_id"sv); id.has_value()) {
        stage_catalog_package(*id, false, [this](auto staged, auto const& diagnostic, bool) {
            send_staged_install(move(staged), false, diagnostic);
        });
    }
}

void AddonsUI::stage_catalog_package(String const& extension_id, bool is_update, StageCompletion on_complete)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr)
        return;
    if (is_update) {
        auto installed = ext_mgr->extension_by_id(extension_id);
        if (!installed.has_value() || !installed->from_catalog) {
            on_complete(Error::from_string_literal("This extension has no Mozilla Add-ons update source"), {}, false);
            return;
        }
    }
    // NB: The completion runs only while this page is alive, so it may refer to the page.
    ext_mgr->catalog_client().lookup(extension_id, [weak = make_weak_ptr<AddonsUI>(), is_update, on_complete = move(on_complete)](ErrorOr<Extensions::CatalogEntry> response) mutable {
        auto self = weak.strong_ref();
        if (!self || !self->extension_manager())
            return;
        if (response.is_error()) {
            on_complete(response.release_error(), {}, false);
            return;
        }
        auto listing = response.release_value();
        auto& extension_manager = *self->extension_manager();
        if (is_update) {
            auto installed = extension_manager.extension_by_id(listing.addon_id);
            if (!installed.has_value() || !installed->from_catalog) {
                on_complete(Error::from_string_literal("Extension is no longer installed"), {}, false);
                return;
            }
            // Only a newer listed version is an update. This also refreshes the hint the toolbar shows.
            if (!extension_manager.record_listed_version(listing.addon_id, listing.version)) {
                on_complete(Error::from_string_literal("Extension is up to date"), {}, true);
                return;
            }
        }
        auto listing_for_download = listing;
        extension_manager.catalog_client().download(listing_for_download, [weak, listing = move(listing), is_update, on_complete = move(on_complete)](ErrorOr<ByteBuffer> package) mutable {
            auto self = weak.strong_ref();
            if (!self || !self->extension_manager())
                return;
            if (package.is_error()) {
                on_complete(package.release_error(), {}, false);
                return;
            }
            Extensions::InstallDiagnostic diagnostic;
            auto staged = self->extension_manager()->stage_catalog_package(listing, package.release_value(), is_update, &diagnostic);
            on_complete(move(staged), diagnostic, false);
        });
    });
}

void AddonsUI::send_staged_install(ErrorOr<Extensions::StagedInstall> staged, bool is_update, Extensions::InstallDiagnostic const& diagnostic)
{
    JsonObject resp;
    if (staged.is_error()) {
        resp.set("error"sv, diagnostic.verification_error.is_empty() ? MUST(String::formatted("{}", staged.error())) : diagnostic.verification_error);
        if (!diagnostic.compatibility_reasons.is_empty())
            resp.set("compatibility_reasons"sv, string_array(diagnostic.compatibility_reasons.span()));
    } else {
        resp = staged_install_json(staged.value());
    }
    async_send_message(is_update ? "updateStaged"_string : "installStaged"_string, JsonValue(move(resp)));
}

JsonObject AddonsUI::staged_install_json(Extensions::StagedInstall const& s)
{
    m_pending_tokens.set(s.token);
    JsonObject resp;
    resp.set("token"sv, s.token);
    resp.set("addon_id"sv, s.addon_id);
    resp.set("name"sv, s.name);
    resp.set("version"sv, s.version);
    if (s.icon_path.has_value())
        resp.set("icon_key"sv, MUST(String::formatted("staged:{}", s.token)));
    resp.set("verified_signature"sv, s.verified_signature);
    resp.set("compatibility_status"sv, MUST(String::from_utf8(Web::Extensions::to_string(s.compatibility_status))));
    resp.set("compatibility_reasons"sv, string_array(s.compatibility_reasons.span()));
    resp.set("permissions"sv, string_array(s.permissions.permissions.span()));
    resp.set("host_permissions"sv, string_array(s.permissions.host_permissions.span()));
    resp.set("optional_permissions"sv, string_array(s.permissions.optional_permissions.span()));
    resp.set("optional_host_permissions"sv, string_array(s.permissions.optional_host_permissions.span()));
    JsonObject data_collection;
    data_collection.set("required"sv, string_array(s.permissions.data_collection.required.span()));
    data_collection.set("optional"sv, string_array(s.permissions.data_collection.optional.span()));
    resp.set("data_collection"sv, move(data_collection));
    resp.set("requires_permission_prompt"sv, s.requires_permission_prompt);
    resp.set("newly_requested_permissions"sv, string_array(s.newly_requested_permissions.span()));
    return resp;
}

void AddonsUI::approve_install(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || !data.is_object())
        return;
    auto token = data.as_object().get_string("token"sv);
    JsonObject resp;
    if (!token.has_value() || !m_pending_tokens.contains(*token) || !data.as_object().get_bool("consented"sv).value_or(false))
        resp.set("error"sv, "Installation requires consent for the current staged package."_string);
    else if (auto res = ext_mgr->approve_install(*token); res.is_error()) {
        resp.set("error"sv, MUST(String::formatted("{}", res.error())));
        m_pending_tokens.remove(*token);
        (void)ext_mgr->cancel_install(*token);
    } else {
        m_pending_tokens.remove(*token);
        resp.set("summary"sv, ext_mgr->summary_json());
    }
    async_send_message("installCompleted"_string, JsonValue(move(resp)));
}

void AddonsUI::cancel_install(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || !data.is_object())
        return;
    auto token = data.as_object().get_string("token"sv);
    if (!token.has_value() || !m_pending_tokens.remove(*token))
        return;
    (void)ext_mgr->cancel_install(*token);
    async_send_message("installCancelled"_string, JsonValue(JsonObject {}));
}

void AddonsUI::stage_manual_update(JsonValue const& data)
{
    if (!data.is_object())
        return;
    if (auto id = data.as_object().get_string("extension_id"sv); id.has_value()) {
        stage_catalog_package(*id, true, [this](auto staged, auto const& diagnostic, bool up_to_date) {
            if (up_to_date) {
                JsonObject out;
                out.set("up_to_date"sv, true);
                if (auto* ext_mgr = extension_manager())
                    out.set("summary"sv, ext_mgr->summary_json());
                async_send_message("updateStaged"_string, JsonValue(move(out)));
                return;
            }
            send_staged_install(move(staged), true, diagnostic);
        });
    }
}

void AddonsUI::check_for_updates(JsonValue const&)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || m_checking_for_updates)
        return;
    m_checking_for_updates = true;
    ext_mgr->check_for_updates([weak = make_weak_ptr<AddonsUI>()](Extensions::ExtensionManager::UpdateCheckResult result) {
        auto self = weak.strong_ref();
        if (!self)
            return;
        self->m_checking_for_updates = false;
        auto* ext_mgr = self->extension_manager();
        if (!ext_mgr)
            return;
        JsonObject out;
        out.set("checked"sv, result.checked);
        JsonArray failed;
        for (auto const& id : result.failed_extension_ids) {
            auto extension = ext_mgr->extension_by_id(id);
            failed.must_append(extension.has_value() ? extension->manifest.name() : id);
        }
        out.set("failed"sv, move(failed));
        out.set("summary"sv, ext_mgr->summary_json());
        self->async_send_message("updatesChecked"_string, JsonValue(move(out)));
    });
}

struct AddonsUI::BulkUpdateStaging : public RefCounted<BulkUpdateStaging> {
    Vector<String> extension_ids;
    size_t next { 0 };
    JsonArray staged;
    JsonArray errors;
};

static JsonObject update_error(String const& extension_id, Optional<Extensions::InstalledExtension const&> extension, String message)
{
    JsonObject error;
    error.set("extension_id"sv, extension_id);
    error.set("name"sv, extension.has_value() ? extension->manifest.name() : extension_id);
    error.set("error"sv, move(message));
    return error;
}

void AddonsUI::stage_updates(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || !data.is_object())
        return;
    auto ids = data.as_object().get_array("extension_ids"sv);
    if (!ids.has_value())
        return;
    auto staging = make_ref_counted<BulkUpdateStaging>();
    ids->for_each([&](JsonValue const& value) {
        // Stage only what the last check found, once each, and no more than a page of extensions.
        if (!value.is_string() || staging->extension_ids.size() >= 100)
            return;
        auto const& id = value.as_string();
        if (ext_mgr->available_update_version(id).has_value() && !staging->extension_ids.contains_slow(id))
            staging->extension_ids.append(id);
    });
    stage_next_update(move(staging));
}

// Stages one update at a time, so that downloads don't compete and each package is verified before the next begins.
void AddonsUI::stage_next_update(NonnullRefPtr<BulkUpdateStaging> staging)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr)
        return;
    if (staging->next == staging->extension_ids.size()) {
        JsonObject out;
        out.set("updates"sv, move(staging->staged));
        out.set("errors"sv, move(staging->errors));
        out.set("summary"sv, ext_mgr->summary_json());
        async_send_message("updatesStaged"_string, JsonValue(move(out)));
        return;
    }
    auto id = staging->extension_ids[staging->next++];
    stage_catalog_package(id, true, [this, staging, id](auto staged, auto const& diagnostic, bool up_to_date) {
        auto* ext_mgr = extension_manager();
        if (!ext_mgr)
            return;
        auto extension = ext_mgr->extension_by_id(id);
        if (staged.is_error()) {
            if (!up_to_date) {
                auto message = diagnostic.verification_error.is_empty() ? MUST(String::formatted("{}", staged.error())) : diagnostic.verification_error;
                staging->errors.must_append(update_error(id, extension, move(message)));
            }
        } else {
            auto item = staged_install_json(staged.value());
            if (extension.has_value())
                item.set("previous_version"sv, extension->manifest.version());
            staging->staged.must_append(move(item));
        }
        stage_next_update(staging);
    });
}

static Vector<String> tokens_from(JsonValue const& data)
{
    Vector<String> tokens;
    if (!data.is_object())
        return tokens;
    if (auto values = data.as_object().get_array("tokens"sv); values.has_value()) {
        values->for_each([&](JsonValue const& value) {
            if (value.is_string() && tokens.size() < 100)
                tokens.append(value.as_string());
        });
    }
    return tokens;
}

void AddonsUI::approve_updates(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || !data.is_object())
        return;
    // The page shows every staged update in one review, and approving that review is consent for each of them.
    bool consented = data.as_object().get_bool("consented"sv).value_or(false);
    JsonArray updated;
    JsonArray errors;
    for (auto const& token : tokens_from(data)) {
        if (!m_pending_tokens.remove(token))
            continue;
        auto staged = ext_mgr->installer().staged_install(token);
        auto id = staged.has_value() ? staged->addon_id : String {};
        if (auto result = ext_mgr->approve_update(token, consented); result.is_error()) {
            errors.must_append(update_error(id, ext_mgr->extension_by_id(id), MUST(String::formatted("{}", result.error()))));
            (void)ext_mgr->cancel_install(token);
        } else {
            updated.must_append(result.value().manifest.name());
        }
    }
    JsonObject out;
    out.set("updated"sv, move(updated));
    out.set("errors"sv, move(errors));
    out.set("summary"sv, ext_mgr->summary_json());
    async_send_message("updatesCompleted"_string, JsonValue(move(out)));
}

void AddonsUI::cancel_updates(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr)
        return;
    for (auto const& token : tokens_from(data)) {
        if (m_pending_tokens.remove(token))
            (void)ext_mgr->cancel_install(token);
    }
    async_send_message("updatesCancelled"_string, JsonValue(JsonObject {}));
}

void AddonsUI::approve_update(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || !data.is_object())
        return;
    auto token = data.as_object().get_string("token"sv);
    bool consented = data.as_object().get_bool("consented"sv).value_or(false);

    JsonObject resp;
    if (!token.has_value() || !m_pending_tokens.contains(*token))
        resp.set("error"sv, "Update is no longer staged in this page."_string);
    else if (auto res = ext_mgr->approve_update(*token, consented); res.is_error()) {
        resp.set("error"sv, MUST(String::formatted("{}", res.error())));
        m_pending_tokens.remove(*token);
        (void)ext_mgr->cancel_install(*token);
    } else {
        m_pending_tokens.remove(*token);
        resp.set("summary"sv, ext_mgr->summary_json());
    }
    async_send_message("updateCompleted"_string, JsonValue(move(resp)));
}

void AddonsUI::open_extension_options(JsonValue const& data)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr || !data.is_object())
        return;
    auto id = data.as_object().get_string("extension_id"sv);
    if (!id.has_value())
        return;
    auto extension = ext_mgr->extension_by_id(*id);
    JsonObject response;
    if (extension.has_value() && extension->enabled && extension->manifest.options_ui().has_value()) {
        auto url = Extensions::ExtensionManager::package_page_url(*extension, extension->manifest.options_ui()->page);
        if (url.has_value())
            Application::the().open_url_in_new_tab(*url, Web::HTML::ActivateTab::Yes);
        else
            response.set("error"sv, "Extension options URL is invalid."_string);
    } else {
        response.set("error"sv, "Extension options are unavailable."_string);
    }
    async_send_message("extensionOptionsOpened"_string, JsonValue(move(response)));
}

void AddonsUI::get_support_matrix(JsonValue const&)
{
    auto matrix = Web::Extensions::SupportMatrix::to_json();
    auto* ext_mgr = extension_manager();
    matrix.set("catalog_enabled"sv, ext_mgr && ext_mgr->catalog_client().enable_public_amo_catalog());
    async_send_message("supportMatrix"_string, JsonValue(move(matrix)));
}

JsonValue AddonsUI::handle_command_for_testing(StringView name, JsonValue const& data)
{
    auto* ext_mgr = manager();
    if (!ext_mgr)
        return JsonValue {};

    if (name == "getInstalledExtensions"sv)
        return JsonValue(ext_mgr->summary_json());
    if (name == "getSupportMatrix"sv)
        return JsonValue(Web::Extensions::SupportMatrix::to_json());
    if (name == "setExtensionEnabled"sv && data.is_object()) {
        auto id = data.as_object().get_string("extension_id"sv);
        auto enabled = data.as_object().get_bool("enabled"sv);
        JsonObject out;
        if (id.has_value() && enabled.has_value()) {
            if (auto err = ext_mgr->set_extension_enabled(*id, *enabled); err.is_error())
                out.set("error"sv, MUST(String::formatted("{}", err.error())));
            else
                out.set("summary"sv, ext_mgr->summary_json());
        }
        return JsonValue(move(out));
    }
    if (name == "removeExtension"sv && data.is_object()) {
        auto id = data.as_object().get_string("extension_id"sv);
        JsonObject out;
        if (id.has_value()) {
            if (auto err = ext_mgr->remove_extension(*id); err.is_error())
                out.set("error"sv, MUST(String::formatted("{}", err.error())));
            else
                out.set("summary"sv, ext_mgr->summary_json());
        }
        return JsonValue(move(out));
    }
    return JsonValue {};
}

}
