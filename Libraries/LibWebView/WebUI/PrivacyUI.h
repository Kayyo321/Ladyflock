/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/WebUI.h>

namespace WebView {

// about:privacy, a read-only view of what the privacy protections do for the identity that the page belongs to: its
// network route, the site controls, their counts, and an explicit capture of one tab's hosts. Everything it shows or
// accepts is bound to its own process's session, so one identity's dashboard never sees another identity's tabs.
class PrivacyUI final : public WebUI {
    WEB_UI(PrivacyUI);

private:
    virtual void register_interfaces() override;

    void load_status();
    void load_capture(JsonValue const&);
    void start_capture(JsonValue const&);
    void stop_capture(JsonValue const&);

    // The session's tab with this ID, if the page's own session has one.
    ViewImplementation* tab_for(JsonValue const&) const;
};

}
