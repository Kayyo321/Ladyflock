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

static JsonObject unbound_inspector_state()
{
    JsonObject payload;
    payload.set("bound"sv, false);
    payload.set("reason"sv, "Open Inspect Element from a page to attach this Inspector."_string);
    return payload;
}

InspectorUI::~InspectorUI()
{
    if (auto session = m_session.strong_ref())
        session->unbind_web_ui(*this);
}

bool InspectorUI::is_bound() const
{
    return !bound_session().is_null();
}

RefPtr<InspectorSession> InspectorUI::bound_session() const
{
    auto session = m_session.strong_ref();
    if (!session || !session->is_bound_to(*this) || !is_current_frontend_document())
        return nullptr;
    return session;
}

bool InspectorUI::is_current_frontend_document() const
{
    auto* page = has_client() ? client().page(page_id()) : nullptr;
    if (!page || !page->is_open() || !page->has_traversable() || !page->displays_tab())
        return false;
    auto const& document = page->traversable().active_document();
    return document.host().ptr() == page && m_frontend_document_id == document.id();
}

void InspectorUI::register_interfaces()
{
    register_interface("inspectorCommand"sv, [this](JsonValue const& data) {
        handle_inspector_command(data);
    });

    if (!has_client())
        return;

    auto* page = client().page(page_id());
    if (!page || !page->is_open() || !page->has_traversable() || !page->displays_tab())
        return;

    auto frontend = page->traversable().view();
    if (!frontend.has_value())
        return;
    m_frontend_document_id = page->traversable().active_document().id();
    auto frontend_view_id = frontend->view_id();
    if (auto authorized = InspectorSession::find_for_frontend_view(frontend_view_id); authorized && !authorized->is_attached()) {
        if (auto session = InspectorSession::bind_web_ui(frontend_view_id, *this)) {
            m_session = session->make_weak_ptr();
            return;
        }
    }

    // Ordinary navigation to about:inspector has zero target authority.
    send_to_frontend("inspectorState"sv, unbound_inspector_state());
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

    auto session = bound_session();
    if (!session) {
        JsonObject reply;
        reply.set("requestId"sv, request_id);
        if (*command == "getSessionState"sv) {
            send_to_frontend("inspectorState"sv, unbound_inspector_state());
            reply.set("ok"sv, true);
            reply.set("result"sv, unbound_inspector_state());
        } else {
            reply.set("ok"sv, false);
            reply.set("error"sv, "Inspector is not bound to any target tab."_string);
        }
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
    if (!is_current_frontend_document())
        return;
    async_send_message(MUST(String::from_utf8(name)), move(data));
}

}
