/*
 * Copyright (c) 2026, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibWebView/InspectorSession.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI/InspectorUI.h>

namespace WebView {

InspectorUI::~InspectorUI()
{
    if (auto session = m_session.strong_ref())
        session->unbind_web_ui(*this);
}

void InspectorUI::register_interfaces()
{
    register_interface("inspectorCommand"sv, [this](JsonValue const& data) {
        handle_inspector_command(data);
    });

    if (!has_client())
        return;

    auto* page = client().page(page_id());
    if (!page || !page->displays_tab())
        return;

    auto frontend_view_id = page->view().view_id();
    if (auto session = InspectorSession::bind_web_ui(frontend_view_id, *this)) {
        m_session = session->make_weak_ptr();
        return;
    }

    // Ordinary navigation to about:inspector has zero target authority.
    JsonObject payload;
    payload.set("bound"sv, false);
    payload.set("reason"sv, "Open Inspect Element from a page to attach this Inspector."_string);
    send_to_frontend("inspectorState"sv, move(payload));
}

void InspectorUI::handle_inspector_command(JsonValue const& data)
{
    if (!data.is_object())
        return;

    auto const& object = data.as_object();
    auto request_id = object.get_u64("requestId"sv).value_or(0);
    auto command = object.get_string("command"sv);
    if (!command.has_value())
        return;

    auto session = m_session.strong_ref();
    if (!session) {
        JsonObject reply;
        reply.set("requestId"sv, request_id);
        reply.set("ok"sv, false);
        reply.set("error"sv, "Inspector is not bound to any target tab."_string);
        send_to_frontend("inspectorReply"sv, move(reply));
        return;
    }

    JsonObject params;
    if (auto params_value = object.get_object("params"sv); params_value.has_value())
        params = *params_value;

    session->handle_command(request_id, *command, params);
}

void InspectorUI::send_to_frontend(StringView name, JsonValue data)
{
    if (!has_client())
        return;
    async_send_message(MUST(String::from_utf8(name)), move(data));
}

}
