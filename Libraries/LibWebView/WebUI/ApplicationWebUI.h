/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>
#include <LibWebView/WebUI.h>

namespace WebView {

struct WEBVIEW_API ApplicationWebUIContext {
    u64 web_ui_id { 0 };
    u64 document_generation { 0 };
    u64 view_id { 0 };
    u64 browsing_session_id { 0 };
    String host;
    IsPrivate is_private { IsPrivate::No };
    String identity_id;
    String identity_name;
    bool is_ephemeral { false };
    bool persists_history { false };
};

// A page that the application answers, rather than anything in this process. What goes between the page and the
// application passes through here without being looked at.
class WEBVIEW_API ApplicationWebUI final : public WebUI {
    WEB_UI(ApplicationWebUI);

public:
    struct Callbacks {
        Function<void(ApplicationWebUIContext const&)> on_open;
        Function<void(u64 id, String name, JsonValue data)> on_message;
        Function<void(u64 id)> on_close;
    };
    static void set_callbacks(Callbacks);

    static ApplicationWebUI* find(u64 id);

    virtual ~ApplicationWebUI() override;

    ApplicationWebUIContext const& context() const { return m_context; }
    BrowsingSession* session() const;
    Optional<u64> view_id() const;

    void send_to_page(String const& name, JsonValue const& data);

    // NB: Some actions of a page, such as opening a download, need the user's own click in it.
    bool consume_user_activation();

private:
    virtual void register_interfaces() override;
    virtual void received_message(String name, JsonValue data) override;

    u64 m_id { 0 };
    ApplicationWebUIContext m_context;
};

}
