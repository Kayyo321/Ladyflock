/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI/ApplicationWebUI.h>

namespace WebView {

static ApplicationWebUI::Callbacks& callbacks()
{
    static NeverDestroyed<ApplicationWebUI::Callbacks> s_callbacks;
    return *s_callbacks;
}

static HashMap<u64, ApplicationWebUI*>& web_uis()
{
    static NeverDestroyed<HashMap<u64, ApplicationWebUI*>> s_web_uis;
    return *s_web_uis;
}

static u64 s_next_id { 1 };
static u64 s_next_document_generation { 1 };

void ApplicationWebUI::set_callbacks(Callbacks callbacks)
{
    ::WebView::callbacks() = move(callbacks);
}

ApplicationWebUI* ApplicationWebUI::find(u64 id)
{
    return web_uis().get(id).value_or(nullptr);
}

ApplicationWebUI::~ApplicationWebUI()
{
    web_uis().remove(m_id);
    if (callbacks().on_close)
        callbacks().on_close(m_id);
}

BrowsingSession* ApplicationWebUI::session() const
{
    if (!has_client())
        return nullptr;
    return &client().session();
}

Optional<u64> ApplicationWebUI::view_id() const
{
    if (!has_client())
        return {};
    auto* page = client().page(page_id());
    if (!page || !page->has_traversable())
        return {};
    return page->view().view_id();
}

void ApplicationWebUI::register_interfaces()
{
    m_id = s_next_id++;
    web_uis().set(m_id, this);

    m_context = ApplicationWebUIContext {
        .web_ui_id = m_id,
        .document_generation = s_next_document_generation++,
        .view_id = view_id().value_or(0),
        .browsing_session_id = has_client() ? client().session().id() : 0,
        .host = host(),
        .is_private = has_client() ? client().is_private() : IsPrivate::Yes,
        .identity_id = has_client() ? client().session().identity().id : String {},
        .identity_name = has_client() ? client().session().identity().display_name : String {},
        .is_ephemeral = !has_client() || client().session().is_ephemeral(),
        .persists_history = has_client() && client().session().persists_history(),
    };

    if (callbacks().on_open)
        callbacks().on_open(m_context);
}

void ApplicationWebUI::received_message(String name, JsonValue data)
{
    if (has_client() && callbacks().on_message)
        callbacks().on_message(m_id, move(name), move(data));
}

bool ApplicationWebUI::consume_user_activation()
{
    if (!has_client())
        return false;
    auto* page = client().page(page_id());
    return page && page->consume_transient_user_activation();
}

void ApplicationWebUI::send_to_page(String const& name, JsonValue const& data)
{
    if (has_client())
        async_send_message(name, data);
}

}
