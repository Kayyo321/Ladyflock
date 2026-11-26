/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebCommon/Extensions/SupportMatrix.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Extensions/ExtensionManager.h>
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
}

void AddonsUI::register_interfaces()
{
    register_interface("getInstalledExtensions"sv, [this](auto const& data) { get_installed_extensions(data); });
    register_interface("setExtensionEnabled"sv, [this](auto const& data) { set_extension_enabled(data); });
    register_interface("removeExtension"sv, [this](auto const& data) { remove_extension(data); });
    register_interface("setExtensionSiteAccess"sv, [this](auto const& data) { set_extension_site_access(data); });
    register_interface("searchCatalog"sv, [this](auto const& data) { search_catalog(data); });
    register_interface("pickLocalXpi"sv, [this](auto const& data) { pick_local_xpi(data); });
    register_interface("stageCatalogInstall"sv, [this](auto const& data) { stage_catalog_install(data); });
    register_interface("approveInstall"sv, [this](auto const& data) { approve_install(data); });
    register_interface("cancelInstall"sv, [this](auto const& data) { cancel_install(data); });
    register_interface("checkManualUpdate"sv, [this](auto const& data) { stage_manual_update(data); });
    register_interface("approveUpdate"sv, [this](auto const& data) { approve_update(data); });
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

    ext_mgr->catalog_client().search(query, page, [weak = make_weak_ptr<AddonsUI>(), request_id = move(request_id)](ErrorOr<Extensions::CatalogSearchResult> res) {
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
                arr.must_append(move(o));
            }
            out.set("results"sv, move(arr));
            out.set("has_more"sv, res.value().has_more);
        }
        self->async_send_message("catalogSearchResults"_string, JsonValue(move(out)));
    });
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
    if (auto id = data.as_object().get_string("extension_id"sv); id.has_value())
        stage_catalog_package(*id, false);
}

void AddonsUI::stage_catalog_package(String const& extension_id, bool is_update)
{
    auto* ext_mgr = extension_manager();
    if (!ext_mgr)
        return;
    if (is_update) {
        auto installed = ext_mgr->extension_by_id(extension_id);
        if (!installed.has_value() || !installed->from_catalog) {
            send_staged_install(Error::from_string_literal("This extension has no Mozilla Add-ons update source"), true);
            return;
        }
    }
    ext_mgr->catalog_client().lookup(extension_id, [weak = make_weak_ptr<AddonsUI>(), is_update](ErrorOr<Extensions::CatalogEntry> response) {
        auto self = weak.strong_ref();
        if (!self || !self->extension_manager())
            return;
        if (response.is_error()) {
            self->send_staged_install(response.release_error(), is_update);
            return;
        }
        auto listing = response.release_value();
        auto& extension_manager = *self->extension_manager();
        if (is_update) {
            auto installed = extension_manager.extension_by_id(listing.addon_id);
            if (!installed.has_value() || !installed->from_catalog) {
                self->send_staged_install(Error::from_string_literal("Extension is no longer installed"), true);
                return;
            }
            if (installed->manifest.version() == listing.version) {
                JsonObject out;
                out.set("up_to_date"sv, true);
                self->async_send_message("updateStaged"_string, JsonValue(move(out)));
                return;
            }
        }
        auto listing_for_download = listing;
        extension_manager.catalog_client().download(listing_for_download, [weak, listing = move(listing), is_update](ErrorOr<ByteBuffer> package) {
            auto self = weak.strong_ref();
            if (!self || !self->extension_manager())
                return;
            if (package.is_error()) {
                self->send_staged_install(package.release_error(), is_update);
                return;
            }
            Extensions::InstallDiagnostic diagnostic;
            auto staged = self->extension_manager()->stage_catalog_package(listing, package.release_value(), is_update, &diagnostic);
            self->send_staged_install(move(staged), is_update, diagnostic);
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
        auto const& s = staged.value();
        m_pending_tokens.set(s.token);
        resp.set("token"sv, s.token);
        resp.set("addon_id"sv, s.addon_id);
        resp.set("name"sv, s.name);
        resp.set("version"sv, s.version);
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
    }
    async_send_message(is_update ? "updateStaged"_string : "installStaged"_string, JsonValue(move(resp)));
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
    if (auto id = data.as_object().get_string("extension_id"sv); id.has_value())
        stage_catalog_package(*id, true);
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
