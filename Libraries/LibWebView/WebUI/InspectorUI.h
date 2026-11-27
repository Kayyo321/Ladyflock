/*
 * Copyright (c) 2026, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
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
    bool is_bound() const { return !m_session.is_null(); }

private:
    virtual void register_interfaces() override;

    void handle_inspector_command(JsonValue const& data);

    WeakPtr<InspectorSession> m_session;
};

}
