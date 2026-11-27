/*
 * Copyright (c) 2026, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class WEBVIEW_API InspectorUI final
    : public WebUI {
    WEB_UI(InspectorUI);

public:
    virtual ~InspectorUI() override;

    void send_to_frontend(StringView name, JsonValue data);
    bool is_bound() const;

private:
    virtual void register_interfaces() override;

    void handle_inspector_command(JsonValue const& data);
    bool is_current_frontend_document() const;
    RefPtr<InspectorSession> bound_session() const;

    Optional<Web::HTML::CrossProcessId> m_frontend_document_id;
    WeakPtr<InspectorSession> m_session;
};

}
