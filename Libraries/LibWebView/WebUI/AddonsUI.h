/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/JsonValue.h>
#include <LibWebView/Extensions/ExtensionInstaller.h>
#include <LibWebView/WebUI.h>

namespace WebView {

namespace Extensions {

class ExtensionManager;

}

// Browser-owned interface for about:addons.
class WEBVIEW_API AddonsUI final : public WebUI {
    WEB_UI(AddonsUI);

public:
    virtual ~AddonsUI() override;
    static JsonValue handle_command_for_testing(StringView name, JsonValue const& data);

private:
    virtual void register_interfaces() override;
    Extensions::ExtensionManager* extension_manager() const;

    void get_installed_extensions(JsonValue const&);
    void set_extension_enabled(JsonValue const&);
    void remove_extension(JsonValue const&);
    void set_extension_site_access(JsonValue const&);
    void search_catalog(JsonValue const&);
    void pick_local_xpi(JsonValue const&);
    void stage_catalog_install(JsonValue const&);
    void stage_catalog_package(String const& extension_id, bool is_update);
    void send_staged_install(ErrorOr<Extensions::StagedInstall>, bool is_update, Extensions::InstallDiagnostic const& = {});
    void approve_install(JsonValue const&);
    void cancel_install(JsonValue const&);
    void stage_manual_update(JsonValue const&);
    void approve_update(JsonValue const&);
    void open_extension_options(JsonValue const&);
    void get_support_matrix(JsonValue const&);

    HashTable<String> m_pending_tokens;
    bool m_picker_open { false };
};

}
