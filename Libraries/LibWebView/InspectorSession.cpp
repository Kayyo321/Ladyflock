/*
 * Copyright (c) 2026, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/NeverDestroyed.h>
#include <AK/StringBuilder.h>
#include <LibCore/EventLoop.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/InspectorSession.h>
#include <LibWebView/Settings.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI/InspectorUI.h>

namespace WebView {

static constexpr size_t max_custom_filter_bytes = 4 * MiB;

static InspectorSession::UICallbacks& s_ui_callbacks()
{
    static NeverDestroyed<InspectorSession::UICallbacks> callbacks;
    return *callbacks;
}

static HashMap<u64, NonnullRefPtr<InspectorSession>>& s_sessions_by_target()
{
    static NeverDestroyed<HashMap<u64, NonnullRefPtr<InspectorSession>>> map;
    return *map;
}

static HashMap<u64, u64>& s_target_by_frontend()
{
    static NeverDestroyed<HashMap<u64, u64>> map;
    return *map;
}

static HashMap<u64, WeakPtr<InspectorSession>>& s_sessions_by_attachment()
{
    static NeverDestroyed<HashMap<u64, WeakPtr<InspectorSession>>> map;
    return *map;
}

static u64 s_next_attachment_id { 1 };

static JsonObject serialize_debugger_value(DebuggerValue const& value)
{
    JsonObject obj;
    switch (value.type) {
    case DebuggerValueType::Undefined:
        obj.set("type"sv, "undefined"_string);
        break;
    case DebuggerValueType::Null:
        obj.set("type"sv, "null"_string);
        break;
    case DebuggerValueType::Boolean:
        obj.set("type"sv, "boolean"_string);
        obj.set("value"sv, value.boolean_value);
        break;
    case DebuggerValueType::Number:
        obj.set("type"sv, "number"_string);
        obj.set("value"sv, value.number_value);
        break;
    case DebuggerValueType::String:
        obj.set("type"sv, "string"_string);
        obj.set("value"sv, value.text.to_utf8_but_should_be_ported_to_utf16());
        break;
    case DebuggerValueType::BigInt:
        obj.set("type"sv, "bigint"_string);
        obj.set("text"sv, value.text.to_utf8_but_should_be_ported_to_utf16());
        break;
    case DebuggerValueType::Object:
        obj.set("type"sv, "object"_string);
        obj.set("objectId"sv, value.object_id);
        obj.set("className"sv, value.object_class);
        if (!value.grip_fields.is_null())
            obj.set("preview"sv, value.grip_fields);
        break;
    case DebuggerValueType::Uninitialized:
        obj.set("type"sv, "uninitialized"_string);
        break;
    }
    if (!value.text.is_empty() && !obj.has("text"sv))
        obj.set("text"sv, value.text.to_utf8_but_should_be_ported_to_utf16());
    return obj;
}

static JsonObject serialize_stack_frame(StackFrame const& frame)
{
    JsonObject obj;
    if (frame.function.has_value())
        obj.set("function"sv, *frame.function);
    if (frame.file.has_value())
        obj.set("file"sv, *frame.file);
    if (frame.line.has_value())
        obj.set("line"sv, static_cast<u64>(*frame.line));
    if (frame.column.has_value())
        obj.set("column"sv, static_cast<u64>(*frame.column));
    return obj;
}

static JsonObject serialize_style_sheet_identifier(Web::CSS::StyleSheetIdentifier const& id)
{
    JsonObject obj;
    obj.set("type"sv, MUST(String::from_utf8(Web::CSS::style_sheet_identifier_type_to_string(id.type))));
    if (id.dom_element_unique_id.has_value())
        obj.set("domElementUniqueId"sv, id.dom_element_unique_id->value());
    if (id.url.has_value())
        obj.set("url"sv, id.url->to_utf8_but_should_be_ported_to_utf16());
    obj.set("ruleCount"sv, static_cast<u64>(id.rule_count));
    return obj;
}

static Optional<Web::CSS::StyleSheetIdentifier> parse_style_sheet_identifier(JsonObject const& obj)
{
    auto type_str = obj.get_string("type"sv);
    if (!type_str.has_value())
        return {};
    auto type = Web::CSS::style_sheet_identifier_type_from_string(*type_str);
    if (!type.has_value())
        return {};
    Web::CSS::StyleSheetIdentifier id { .type = *type };
    if (auto dom_id = obj.get_i64("domElementUniqueId"sv); dom_id.has_value())
        id.dom_element_unique_id = Web::UniqueNodeID(*dom_id);
    if (auto url = obj.get_string("url"sv); url.has_value())
        id.url = Utf16String::from_utf8(*url);
    id.rule_count = obj.get_u64("ruleCount"sv).value_or(0);
    return id;
}

static JsonObject serialize_script_description(Web::HTML::ScriptRegistryDescription const& desc)
{
    JsonObject obj;
    JsonObject id_obj;
    id_obj.set("documentId"sv, desc.id.document_id.value());
    id_obj.set("scriptId"sv, desc.id.script_id);
    obj.set("id"sv, move(id_obj));
    if (desc.url.has_value())
        obj.set("url"sv, desc.url->serialize());
    obj.set("displayUrl"sv, desc.display_url.to_utf8_but_should_be_ported_to_utf16());
    obj.set("introductionType"sv, desc.introduction_type.to_utf8_but_should_be_ported_to_utf16());
    obj.set("contentType"sv, desc.content_type.to_utf8_but_should_be_ported_to_utf16());
    obj.set("isInlineSource"sv, desc.is_inline_source);
    obj.set("sourceStartLine"sv, desc.source_start_line);
    obj.set("sourceStartColumn"sv, desc.source_start_column);
    obj.set("sourceLength"sv, static_cast<u64>(desc.source_length));
    return obj;
}

static Optional<Web::CSS::PseudoElement> parse_pseudo_element(Optional<StringView> name)
{
    if (!name.has_value() || name->is_empty())
        return {};
    auto view = *name;
    if (view == "before"sv || view == "::before"sv)
        return Web::CSS::PseudoElement::Before;
    if (view == "after"sv || view == "::after"sv)
        return Web::CSS::PseudoElement::After;
    if (view == "marker"sv || view == "::marker"sv)
        return Web::CSS::PseudoElement::Marker;
    if (view == "backdrop"sv || view == "::backdrop"sv)
        return Web::CSS::PseudoElement::Backdrop;
    if (view == "first-line"sv || view == "::first-line"sv)
        return Web::CSS::PseudoElement::FirstLine;
    if (view == "first-letter"sv || view == "::first-letter"sv)
        return Web::CSS::PseudoElement::FirstLetter;
    if (view == "selection"sv || view == "::selection"sv)
        return Web::CSS::PseudoElement::Selection;
    if (view == "placeholder"sv || view == "::placeholder"sv)
        return Web::CSS::PseudoElement::Placeholder;
    return {};
}

static StringView console_log_level_to_string(JS::ConsoleLogLevel level)
{
    switch (level) {
    case JS::ConsoleLogLevel::Assert:
        return "assert"sv;
    case JS::ConsoleLogLevel::Count:
        return "count"sv;
    case JS::ConsoleLogLevel::CountReset:
        return "countReset"sv;
    case JS::ConsoleLogLevel::Debug:
        return "debug"sv;
    case JS::ConsoleLogLevel::Dir:
        return "dir"sv;
    case JS::ConsoleLogLevel::DirXML:
        return "dirxml"sv;
    case JS::ConsoleLogLevel::Error:
        return "error"sv;
    case JS::ConsoleLogLevel::Group:
        return "group"sv;
    case JS::ConsoleLogLevel::GroupCollapsed:
        return "groupCollapsed"sv;
    case JS::ConsoleLogLevel::Info:
        return "info"sv;
    case JS::ConsoleLogLevel::Log:
        return "log"sv;
    case JS::ConsoleLogLevel::Table:
        return "table"sv;
    case JS::ConsoleLogLevel::TimeEnd:
        return "timeEnd"sv;
    case JS::ConsoleLogLevel::TimeLog:
        return "timeLog"sv;
    case JS::ConsoleLogLevel::Trace:
        return "trace"sv;
    case JS::ConsoleLogLevel::Warn:
        return "warn"sv;
    }
    return "log"sv;
}

static StringView cache_state_to_string(Requests::CacheState state)
{
    switch (state) {
    case Requests::CacheState::NotCached:
        return "none"sv;
    case Requests::CacheState::Local:
        return "disk"sv;
    case Requests::CacheState::Validated:
        return "revalidated"sv;
    }
    return "none"sv;
}

void InspectorSession::set_ui_callbacks(UICallbacks callbacks)
{
    s_ui_callbacks() = move(callbacks);
}

InspectorSession::UICallbacks const& InspectorSession::ui_callbacks()
{
    return s_ui_callbacks();
}

NonnullRefPtr<InspectorSession> InspectorSession::ensure_for_target_view(ViewImplementation& target_view)
{
    if (auto existing = s_sessions_by_target().get(target_view.view_id()); existing.has_value())
        return NonnullRefPtr<InspectorSession> { **existing };

    auto session = adopt_ref(*new InspectorSession(target_view));
    s_sessions_by_target().set(target_view.view_id(), session);
    s_sessions_by_attachment().set(session->attachment_id(), session->make_weak_ptr());
    target_view.set_inspector_session({}, session->make_weak_ptr());
    return session;
}

RefPtr<InspectorSession> InspectorSession::find_for_target_view(u64 target_view_id)
{
    if (auto existing = s_sessions_by_target().get(target_view_id); existing.has_value())
        return *existing;
    return nullptr;
}

RefPtr<InspectorSession> InspectorSession::find_for_frontend_view(u64 frontend_view_id)
{
    auto target_id = s_target_by_frontend().get(frontend_view_id);
    if (!target_id.has_value())
        return nullptr;
    return find_for_target_view(*target_id);
}

RefPtr<InspectorSession> InspectorSession::find_by_attachment_id(u64 attachment_id)
{
    if (auto weak = s_sessions_by_attachment().get(attachment_id); weak.has_value())
        return *weak;
    return nullptr;
}

RefPtr<InspectorSession> InspectorSession::authorize_frontend_view(ViewImplementation& target_view, u64 frontend_view_id)
{
    auto session = ensure_for_target_view(target_view);
    if (session->m_frontend_view_id.has_value() && *session->m_frontend_view_id != frontend_view_id)
        s_target_by_frontend().remove(*session->m_frontend_view_id);

    session->m_frontend_view_id = frontend_view_id;
    s_target_by_frontend().set(frontend_view_id, target_view.view_id());
    return session;
}

RefPtr<InspectorSession> InspectorSession::bind_web_ui(u64 frontend_view_id, InspectorUI& web_ui)
{
    auto session = find_for_frontend_view(frontend_view_id);
    if (!session)
        return nullptr;

    session->m_web_ui = web_ui.make_weak_ptr<InspectorUI>();
    session->attach_to_target_client();
    session->send_session_state();

    if (session->m_pending_inspect_target.has_value()) {
        auto pending = session->m_pending_inspect_target.release_value();
        auto* target = session->target_view();
        if (target && target->is_current_context_menu(pending.menu_generation) && pending.page && pending.document_id.has_value()
            && pending.page.ptr() == &target->page() && pending.page->hosts_active_document(*pending.document_id))
            session->select_node(pending.node_id, pending.document_id, pending.menu_generation);
        else
            session->select_node(0, {}, pending.menu_generation);
    }

    return session;
}

InspectorSession::InspectorSession(ViewImplementation& target_view)
    : m_attachment_id(s_next_attachment_id++)
    , m_target_view_id(target_view.view_id())
    , m_target_view(target_view.make_weak_ptr())
    , m_is_private(target_view.is_private())
{
    m_navigation_listener_id = target_view.add_navigation_listener({
        .on_load_start = {},
        .on_load_finish = [weak_this = make_weak_ptr()](URL::URL const& url) {
            if (auto self = weak_this.strong_ref()) {
                JsonObject payload;
                payload.set("url"sv, url.serialize());
                payload.set("documentGeneration"sv, self->m_document_generation);
                self->send_event("targetLoadFinished"sv, move(payload));
                self->restore_dom_breakpoints(url);
            } },
        // NB: Not on load start: until a load commits the old document is still the page's, and the load can
        //     still be cancelled, become a download, or answer 204.
        .on_active_document_change = [weak_this = make_weak_ptr()](URL::URL const& url) {
            if (auto self = weak_this.strong_ref())
                self->target_did_navigate(url); },
    });
}

InspectorSession::~InspectorSession()
{
    detach_from_target_client();
    if (m_frontend_view_id.has_value())
        s_target_by_frontend().remove(*m_frontend_view_id);
    s_sessions_by_attachment().remove(m_attachment_id);
    if (auto* view = target_view()) {
        if (m_navigation_listener_id.has_value())
            view->remove_navigation_listener(*m_navigation_listener_id);
        if (view->inspector_session() == this)
            view->set_inspector_session({}, {});
    }
}

void InspectorSession::attach_to_target_client()
{
    auto* view = target_view();
    if (!view || m_connected_to_target)
        return;

    m_connected_to_target = true;
    view->did_connect_devtools_client();
    view->start_sending_console_output(true, true);
    if (!m_listening_for_mutations) {
        m_listening_for_mutations = true;
        view->add_dom_mutation_listener_ref();
    }
}

void InspectorSession::detach_from_target_client()
{
    auto* view = target_view();
    if (view) {
        if (m_picker_active)
            set_node_picker_active(false);
        if (m_connected_to_target) {
            view->clear_highlighted_dom_node();
            view->dispatch_inspector_engine_command(0, "cleanupLocalInspector"_string, {}, {});
            restore_emulation_overrides();
        }
        if (m_debugger_attached) {
            m_debugger_attached = false;
            view->detach_debugger_for_owner(ViewImplementation::InteractiveOwner::LocalInspector);
        }
        if (m_listening_for_mutations) {
            m_listening_for_mutations = false;
            view->remove_dom_mutation_listener_ref();
        }
        if (m_connected_to_target) {
            m_connected_to_target = false;
            view->did_disconnect_devtools_client();
        }
    } else {
        m_connected_to_target = false;
        m_listening_for_mutations = false;
        m_picker_active = false;
        m_debugger_attached = false;
    }
}

bool InspectorSession::has_current_frontend() const
{
    return m_web_ui && m_web_ui->is_current_frontend_document();
}

void InspectorSession::unbind_web_ui(InspectorUI& web_ui)
{
    if (m_web_ui.ptr() != &web_ui)
        return;

    // The frontend's document went away, as it does when it reloads or its process is lost, but the frontend view is
    // still the one Inspect Element opened for this tab: let go of the target until the WebUI of its next document
    // binds again. Closing the Inspector, or the frontend view, detaches the frontend for good.
    m_web_ui = nullptr;
    detach_from_target_client();
}

void InspectorSession::detach_frontend()
{
    if (m_frontend_view_id.has_value()) {
        s_target_by_frontend().remove(*m_frontend_view_id);
        m_frontend_view_id.clear();
    }
    m_web_ui = nullptr;
    detach_from_target_client();
    reset_for_new_document();
    m_pending_inspect_target.clear();
    m_console_entries.clear();
    m_network_entries.clear();
    m_dom_breakpoints.clear();
    m_saved_cosmetic_filters.clear();

    if (auto* view = target_view(); view && view->inspector_session() == this) {
        view->set_inspector_session({}, {});
        if (auto listener_id = exchange(m_navigation_listener_id, {}); listener_id.has_value())
            view->remove_navigation_listener(*listener_id);
        m_target_view = nullptr;
        s_sessions_by_attachment().remove(m_attachment_id);
        s_sessions_by_target().remove(m_target_view_id);
    }
}

void InspectorSession::target_did_close()
{
    JsonObject payload;
    payload.set("attachmentId"sv, m_attachment_id);
    payload.set("targetViewId"sv, m_target_view_id);
    payload.set("isPrivate"sv, m_is_private == IsPrivate::Yes);
    send_event("targetClosed"sv, move(payload));

    detach_from_target_client();
    auto target_id = m_target_view_id;
    m_target_view = nullptr;

    if (auto& on_close = ui_callbacks().on_close_requested)
        on_close(target_id);

    if (m_frontend_view_id.has_value()) {
        s_target_by_frontend().remove(*m_frontend_view_id);
        m_frontend_view_id.clear();
    }
    s_sessions_by_target().remove(target_id);
}

void InspectorSession::target_did_navigate(URL::URL const& url)
{
    reset_for_new_document();
    JsonObject payload;
    payload.set("url"sv, url.serialize());
    payload.set("documentGeneration"sv, m_document_generation);
    send_event("targetDocumentChanged"sv, move(payload));
}

void InspectorSession::target_did_crash()
{
    m_target_crashed = true;
    reset_for_new_document();
    JsonObject payload;
    payload.set("crashed"sv, true);
    payload.set("documentGeneration"sv, m_document_generation);
    send_event("targetCrashed"sv, move(payload));
}

void InspectorSession::target_did_recover()
{
    if (!m_target_crashed)
        return;
    m_target_crashed = false;
    reset_for_new_document();
    if (m_web_ui)
        attach_to_target_client();
    JsonObject payload;
    payload.set("crashed"sv, false);
    payload.set("documentGeneration"sv, m_document_generation);
    send_event("targetRecovered"sv, move(payload));
}

void InspectorSession::reset_for_new_document()
{
    ++m_document_generation;
    if (auto* view = target_view())
        view->cancel_pending_inspector_engine_commands();
    m_dom_sequence = 0;
    m_selected_node_id.clear();
    m_selection_history.clear();
    m_completed_mutation_replies.clear();
    m_node_properties_requests.clear();
    m_pending_html_request_id.clear();
    m_pending_edit_request_id.clear();
    m_pending_edit_journal_entry.clear();
    m_pending_a11y_request_id.clear();
    m_pending_stylesheets_request_id.clear();
    m_pending_stylesheet_source_request_id.clear();
    m_pending_grid_layouts_request_id.clear();
    m_pending_current_grid_request_id.clear();
    m_pending_current_flexbox_request_id.clear();
    m_pending_console_eval_request_id.clear();
    m_edit_journal.clear();
    m_edit_journal_truncated = false;
    if (!m_preserve_console_log)
        m_console_entries.clear();
    if (!m_preserve_network_log)
        m_network_entries.clear();
    for (auto& bp : m_dom_breakpoints) {
        bp.node_id = 0;
        bp.resolved = false;
    }
}

void InspectorSession::restore_dom_breakpoints(URL::URL const& url)
{
    auto* view = target_view();
    if (!view || !m_web_ui || view->debugger_owner() != ViewImplementation::InteractiveOwner::LocalInspector)
        return;
    auto serialized_url = url.serialize();
    for (auto const& breakpoint : m_dom_breakpoints) {
        if (breakpoint.resolved || breakpoint.url != serialized_url || breakpoint.node_path.is_empty())
            continue;
        JsonObject params;
        params.set("nodePath"sv, breakpoint.node_path);
        params.set("type"sv, breakpoint.type);
        params.set("enabled"sv, breakpoint.enabled);
        view->dispatch_inspector_engine_command(0, "restoreDomBreakpoint"_string, params, [weak_this = make_weak_ptr(), generation = m_document_generation, id = breakpoint.id](bool ok, JsonValue result, Optional<String>) {
            auto self = weak_this.strong_ref();
            if (!self || self->m_document_generation != generation || !ok || !result.is_object())
                return;
            auto node_id = result.as_object().get_i64("nodeId"sv).value_or(0);
            for (auto& descriptor : self->m_dom_breakpoints) {
                if (descriptor.id == id) {
                    descriptor.node_id = Web::UniqueNodeID(node_id);
                    descriptor.resolved = node_id != 0;
                    break;
                }
            }
            self->send_event("domBreakpointsChanged"sv, self->serialize_dom_breakpoints());
        });
    }
}

void InspectorSession::restore_emulation_overrides()
{
    auto* view = target_view();
    if (!view)
        return;

    if (!m_active_rendering_overrides.is_empty()) {
        view->send_preferences_to_page(view->page());
        m_active_rendering_overrides = {};
    }
    JsonObject disabled;
    disabled.set("enabled"sv, false);
    if (!m_active_device_overrides.is_empty()) {
        view->page().async_set_viewport(view->viewport_size(), view->device_pixel_ratio(), view->is_fullscreen());
        m_active_device_overrides = {};
    }
    if (!m_active_sensor_overrides.is_empty()) {
        view->dispatch_inspector_engine_command(0, "setSensorOverrides"_string, disabled, {});
        m_active_sensor_overrides = {};
    }
}

void InspectorSession::set_dock_mode(String dock_mode)
{
    m_dock_mode = move(dock_mode);
    JsonObject payload;
    payload.set("dockMode"sv, m_dock_mode);
    send_event("dockModeChanged"sv, move(payload));
}

void InspectorSession::set_initial_panel(String panel)
{
    m_initial_panel = move(panel);
}

void InspectorSession::queue_inspect_target(PendingInspectTarget target)
{
    m_pending_inspect_target = move(target);
    if (m_web_ui) {
        auto pending = m_pending_inspect_target.release_value();
        auto* view = target_view();
        if (view && view->is_current_context_menu(pending.menu_generation) && pending.page.ptr() == &view->page() && pending.document_id.has_value()
            && pending.page->hosts_active_document(*pending.document_id))
            select_node(pending.node_id, pending.document_id, pending.menu_generation);
        else
            select_node(0, {}, pending.menu_generation);
    }
}

void InspectorSession::select_panel(String const& panel)
{
    m_initial_panel = panel;
    JsonObject payload;
    payload.set("panel"sv, panel);
    send_event("selectPanel"sv, move(payload));
}

void InspectorSession::select_node(Web::UniqueNodeID node_id, Optional<Web::HTML::CrossProcessId> document_id, u64 menu_generation)
{
    if (node_id != 0) {
        m_selected_node_id = node_id;
        record_selection_history(node_id);
    }

    JsonObject payload;
    payload.set("nodeId"sv, node_id.value());
    if (document_id.has_value())
        payload.set("documentId"sv, MUST(String::formatted("{}", *document_id)));
    payload.set("menuGeneration"sv, menu_generation);
    payload.set("documentGeneration"sv, m_document_generation);
    send_event("selectNode"sv, move(payload));
}

void InspectorSession::record_selection_history(Web::UniqueNodeID node_id)
{
    m_selection_history.remove_all_matching([node_id](auto existing) { return existing == node_id; });
    m_selection_history.prepend(node_id);
    if (m_selection_history.size() > 5)
        m_selection_history.shrink(5);
}

void InspectorSession::toggle_node_picker()
{
    set_node_picker_active(!m_picker_active);
}

void InspectorSession::set_node_picker_active(bool active)
{
    auto* view = target_view();
    if (!view)
        return;

    if (m_picker_active == active)
        return;

    m_picker_active = active;
    if (active) {
        view->start_node_picker_for_owner(ViewImplementation::InteractiveOwner::LocalInspector, [weak_this = make_weak_ptr()](auto event) {
            auto self = weak_this.strong_ref();
            if (!self)
                return;

            JsonObject payload;
            switch (event.type) {
            case DevTools::DevToolsDelegate::NodePickerEvent::Type::Hovered:
                payload.set("type"sv, "hovered"_string);
                if (event.node_id.has_value()) {
                    payload.set("nodeId"sv, event.node_id->value());
                    if (auto* target = self->target_view())
                        target->highlight_dom_node(*event.node_id, {});
                }
                break;
            case DevTools::DevToolsDelegate::NodePickerEvent::Type::Previewed:
                payload.set("type"sv, "previewed"_string);
                if (event.node_id.has_value())
                    payload.set("nodeId"sv, event.node_id->value());
                break;
            case DevTools::DevToolsDelegate::NodePickerEvent::Type::Picked:
                payload.set("type"sv, "picked"_string);
                if (event.node_id.has_value()) {
                    payload.set("nodeId"sv, event.node_id->value());
                    self->m_selected_node_id = *event.node_id;
                    self->record_selection_history(*event.node_id);
                }
                self->set_node_picker_active(false);
                break;
            case DevTools::DevToolsDelegate::NodePickerEvent::Type::Canceled:
                payload.set("type"sv, "canceled"_string);
                self->set_node_picker_active(false);
                break;
            }
            self->send_event("pickerEvent"sv, move(payload));
        });
    } else {
        view->stop_node_picker_for_owner(ViewImplementation::InteractiveOwner::LocalInspector);
    }

    if (auto& on_picker = ui_callbacks().on_picker_state_changed)
        on_picker(m_target_view_id, m_picker_active);

    JsonObject state;
    state.set("active"sv, m_picker_active);
    send_event("pickerStateChanged"sv, move(state));
}

void InspectorSession::send_session_state()
{
    auto* view = target_view();
    JsonObject payload;
    payload.set("bound"sv, true);
    payload.set("attachmentId"sv, m_attachment_id);
    payload.set("targetViewId"sv, m_target_view_id);
    payload.set("isPrivate"sv, m_is_private == IsPrivate::Yes);
    payload.set("dockMode"sv, m_dock_mode);
    payload.set("initialPanel"sv, m_initial_panel);
    payload.set("documentGeneration"sv, m_document_generation);
    payload.set("pickerActive"sv, m_picker_active);
    payload.set("crashed"sv, m_target_crashed);
    if (view) {
        payload.set("targetUrl"sv, view->url().serialize());
        payload.set("targetTitle"sv, view->title().to_utf8_but_should_be_ported_to_utf16());
        payload.set("devicePixelRatio"sv, view->device_pixel_ratio());
        payload.set("zoomLevel"sv, view->zoom_level());
    }
    if (m_selected_node_id.has_value())
        payload.set("selectedNodeId"sv, m_selected_node_id->value());
    payload.set("consoleEntries"sv, serialize_console_log());
    payload.set("networkEntries"sv, serialize_network_log());
    payload.set("editJournal"sv, serialize_edit_journal());
    payload.set("domBreakpoints"sv, serialize_dom_breakpoints());
    send_event("inspectorState"sv, move(payload));
}

void InspectorSession::send_event(StringView name, JsonObject payload)
{
    auto ui = m_web_ui.strong_ref();
    if (!ui)
        return;

    payload.set("attachmentId"sv, m_attachment_id);
    ui->send_to_frontend(name, move(payload));
}

void InspectorSession::send_reply(u64 request_id, bool ok, JsonValue result, Optional<String> error)
{
    auto ui = m_web_ui.strong_ref();
    if (!ui)
        return;

    JsonObject reply;
    reply.set("requestId"sv, request_id);
    reply.set("ok"sv, ok);
    reply.set("documentGeneration"sv, m_document_generation);
    if (ok)
        reply.set("result"sv, move(result));
    else
        reply.set("error"sv, error.value_or("Inspector command failed."_string));

    ui->send_to_frontend("inspectorReply"sv, move(reply));
}

void InspectorSession::record_edit_entry(EditJournalEntry entry)
{
    entry.id = m_next_edit_journal_id++;
    entry.document_generation = m_document_generation;
    static constexpr size_t max_journal_text_bytes = 64 * KiB;
    if (entry.before_text.byte_count() > max_journal_text_bytes || entry.after_text.byte_count() > max_journal_text_bytes) {
        entry.before_text = "(Edit too large to retain)"_string;
        entry.after_text = {};
        entry.undo_payload = {};
        m_edit_journal_truncated = true;
    }
    if (m_edit_journal.size() >= max_edit_journal_entries) {
        m_edit_journal.remove(0);
        m_edit_journal_truncated = true;
    }
    m_edit_journal.append(move(entry));
    send_event("editJournalUpdated"sv, serialize_edit_journal());
}

JsonObject InspectorSession::serialize_edit_journal() const
{
    JsonObject out;
    out.set("truncated"sv, m_edit_journal_truncated);
    out.set("documentGeneration"sv, m_document_generation);
    JsonArray entries;
    for (auto const& e : m_edit_journal) {
        JsonObject obj;
        obj.set("id"sv, e.id);
        obj.set("kind"sv, e.kind);
        obj.set("targetLabel"sv, e.target_label);
        obj.set("groupKey"sv, e.group_key);
        obj.set("beforeText"sv, e.before_text);
        obj.set("afterText"sv, e.after_text);
        obj.set("baseRevision"sv, e.base_revision);
        obj.set("resultingRevision"sv, e.resulting_revision);
        obj.set("isWritableSource"sv, e.is_writable_source);
        obj.set("undone"sv, e.undone);
        obj.set("canUndo"sv, !e.undo_payload.is_empty() && !e.undone);
        entries.must_append(move(obj));
    }
    out.set("entries"sv, move(entries));
    return out;
}

JsonObject InspectorSession::serialize_network_log() const
{
    JsonObject out;
    JsonArray list;
    for (auto const& n : m_network_entries) {
        JsonObject obj;
        obj.set("requestId"sv, n.request_id);
        obj.set("url"sv, n.url);
        obj.set("method"sv, n.method);
        obj.set("statusCode"sv, n.status_code);
        obj.set("reasonPhrase"sv, n.reason_phrase);
        obj.set("initiatorType"sv, n.initiator_type);
        obj.set("referrerPolicy"sv, n.referrer_policy);
        obj.set("cacheState"sv, n.cache_state);
        obj.set("isNavigationRequest"sv, n.is_navigation_request);
        obj.set("bodySize"sv, n.body_size);
        obj.set("durationMs"sv, n.duration_ms);
        obj.set("finished"sv, n.finished);
        obj.set("requestHeaders"sv, n.request_headers);
        obj.set("responseHeaders"sv, n.response_headers);
        if (!n.response_body_preview.is_empty())
            obj.set("responseBodyPreview"sv, n.response_body_preview);
        if (n.error.has_value())
            obj.set("error"sv, *n.error);
        list.must_append(move(obj));
    }
    out.set("entries"sv, move(list));
    return out;
}

JsonObject InspectorSession::serialize_console_log() const
{
    JsonObject out;
    JsonArray list;
    for (auto const& entry : m_console_entries)
        list.must_append(entry);
    out.set("entries"sv, move(list));
    return out;
}

JsonObject InspectorSession::serialize_dom_breakpoints() const
{
    JsonObject out;
    JsonArray list;
    for (auto const& bp : m_dom_breakpoints) {
        JsonObject obj;
        obj.set("id"sv, bp.id);
        obj.set("url"sv, bp.url);
        obj.set("nodePath"sv, bp.node_path);
        obj.set("nodeId"sv, bp.node_id.value());
        obj.set("type"sv, bp.type);
        obj.set("enabled"sv, bp.enabled);
        obj.set("resolved"sv, bp.resolved);
        list.must_append(move(obj));
    }
    out.set("breakpoints"sv, move(list));
    return out;
}

JsonObject InspectorSession::save_cosmetic_filter_rule(String const& host, String const& selector)
{
    JsonObject result;
    if (m_is_private == IsPrivate::Yes) {
        result.set("saved"sv, false);
        result.set("privateSession"sv, true);
        result.set("error"sv, "Private inspection cannot save persistent content-blocker rules to the regular profile."_string);
        return result;
    }

    auto trimmed_host = host.bytes_as_string_view().trim_whitespace();
    auto trimmed_selector = selector.bytes_as_string_view().trim_whitespace();
    auto host_url = URL::Parser::basic_parse(MUST(String::formatted("https://{}/", trimmed_host)));
    if (!host_url.has_value() || host_url->serialized_host() != trimmed_host || trimmed_selector.is_empty()
        || trimmed_selector.contains('\n') || trimmed_selector.contains('\r')) {
        result.set("saved"sv, false);
        result.set("error"sv, "Cosmetic filter requires a valid single-line host and CSS selector."_string);
        return result;
    }

    auto rule = MUST(String::formatted("{}##{}", trimmed_host, trimmed_selector));
    auto& settings = Application::settings();
    auto current = settings.custom_content_blocker_filters();

    // Check for duplicate rule line.
    bool already_present = false;
    current.bytes_as_string_view().for_each_split_view('\n', SplitBehavior::Nothing, [&](StringView line) {
        if (line.trim_whitespace() == rule.bytes_as_string_view())
            already_present = true;
    });

    if (already_present) {
        result.set("saved"sv, true);
        result.set("duplicate"sv, true);
        result.set("rule"sv, rule);
        result.set("scope"sv, "regular-profile"_string);
        return result;
    }

    StringBuilder updated;
    updated.append(current);
    if (!current.is_empty() && !current.bytes_as_string_view().ends_with('\n'))
        updated.append('\n');
    updated.append(rule);
    updated.append('\n');

    if (updated.length() > max_custom_filter_bytes) {
        result.set("saved"sv, false);
        result.set("error"sv, "Adding this cosmetic filter would exceed the 4 MiB custom filter limit."_string);
        return result;
    }

    settings.set_custom_content_blocker_filters(MUST(updated.to_string()));
    auto id = m_next_cosmetic_filter_id++;
    m_saved_cosmetic_filters.append({ id, rule, false });

    result.set("saved"sv, true);
    result.set("duplicate"sv, false);
    result.set("filterJournalId"sv, id);
    result.set("rule"sv, rule);
    result.set("scope"sv, "regular-profile"_string);
    return result;
}

JsonObject InspectorSession::undo_cosmetic_filter_rule(u64 filter_journal_id)
{
    JsonObject result;
    if (m_is_private == IsPrivate::Yes) {
        result.set("undone"sv, false);
        result.set("error"sv, "No persistent filter rules exist in private inspection."_string);
        return result;
    }

    SavedCosmeticFilter* entry = nullptr;
    for (auto& candidate : m_saved_cosmetic_filters) {
        if (candidate.id == filter_journal_id && !candidate.undone) {
            entry = &candidate;
            break;
        }
    }
    if (!entry) {
        result.set("undone"sv, false);
        result.set("error"sv, "Filter journal entry not found or already undone."_string);
        return result;
    }

    // Surgical removal: remove only the exact line we added, preserving any concurrent settings edits.
    auto& settings = Application::settings();
    auto current = settings.custom_content_blocker_filters();
    StringBuilder rebuilt;
    bool removed = false;
    bool first = true;
    current.bytes_as_string_view().for_each_split_view('\n', SplitBehavior::KeepEmpty, [&](StringView line) {
        if (!removed && line.trim_whitespace() == entry->rule.bytes_as_string_view()) {
            removed = true;
            return;
        }
        if (!first)
            rebuilt.append('\n');
        first = false;
        rebuilt.append(line);
    });

    if (!removed) {
        result.set("undone"sv, false);
        result.set("error"sv, "The filter rule was already modified or removed externally."_string);
        return result;
    }

    entry->undone = true;
    settings.set_custom_content_blocker_filters(MUST(rebuilt.to_string()));
    result.set("undone"sv, true);
    result.set("rule"sv, entry->rule);
    return result;
}

void InspectorSession::handle_command(u64 request_id, StringView command, JsonObject const& params)
{
    // Replay idempotency for mutation requests.
    if (auto previous = m_completed_mutation_replies.get(request_id); previous.has_value()) {
        send_reply(request_id, true, *previous);
        return;
    }

    // UI/session-level commands that remain responsive even when target is paused or crashed.
    if (command == "getSessionState"sv) {
        send_session_state();
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "closeInspector"sv) {
        send_reply(request_id, true, JsonObject {});
        if (auto& on_close = ui_callbacks().on_close_requested)
            on_close(m_target_view_id);
        return;
    }
    if (command == "setDockMode"sv) {
        auto mode = params.get_string("mode"sv).value_or("right"_string);
        if (!mode.is_one_of("right"sv, "bottom"sv, "left"sv, "window"sv)) {
            send_reply(request_id, false, JsonValue {}, "Invalid dock mode."_string);
            return;
        }
        m_dock_mode = mode;
        if (auto& on_dock = ui_callbacks().on_dock_mode_changed)
            on_dock(m_target_view_id, mode);
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "copyText"sv) {
        auto text = params.get_string("text"sv).value_or(String {});
        if (auto& on_copy = ui_callbacks().on_copy_text) {
            on_copy(text);
        } else {
            Web::Clipboard::SystemClipboardItem item;
            item.system_clipboard_representations.append({ "text/plain"_string, text.to_byte_string() });
            Application::the().insert_clipboard_item(move(item));
        }
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "openUrl"sv) {
        auto raw_url = params.get_string("url"sv);
        if (!raw_url.has_value()) {
            send_reply(request_id, false, JsonValue {}, "Missing URL."_string);
            return;
        }
        auto parsed = URL::Parser::basic_parse(*raw_url);
        if (!parsed.has_value() || !parsed->scheme().is_one_of("http"sv, "https"sv)) {
            send_reply(request_id, false, JsonValue {}, "Only http(s) URLs may be opened from Inspector."_string);
            return;
        }
        if (auto& on_open = ui_callbacks().on_open_url_in_new_tab)
            on_open(*parsed, m_target_view_id, m_is_private);
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "saveCosmeticFilter"sv) {
        auto host = params.get_string("host"sv).value_or(String {});
        auto selector = params.get_string("selector"sv).value_or(String {});
        auto* view = target_view();
        if (!view || !view->url().scheme().is_one_of("http"sv, "https"sv) || view->url().serialized_host() != host) {
            send_reply(request_id, false, {}, "A cosmetic filter must use the inspected site's host."_string);
            return;
        }
        JsonObject preview;
        preview.set("selector"sv, selector);
        preview.set("highlight"sv, false);
        view->dispatch_inspector_engine_command(request_id, "previewCosmeticFilter"_string, preview, [weak_this = make_weak_ptr(), generation = m_document_generation, request_id, host, selector](bool ok, JsonValue result, Optional<String> error) {
            auto self = weak_this.strong_ref();
            if (!self || self->m_document_generation != generation)
                return;
            if (!ok || !result.is_object() || !result.as_object().get_bool("valid"sv).value_or(false)) {
                self->send_reply(request_id, false, {}, error.value_or("Invalid cosmetic filter selector."_string));
                return;
            }
            auto saved = self->save_cosmetic_filter_rule(host, selector);
            auto is_saved = saved.get_bool("saved"sv).value_or(false);
            auto message = saved.get_string("error"sv).map([](auto const& text) { return String { text }; });
            self->send_reply(request_id, is_saved, move(saved), move(message));
        });
        return;
    }
    if (command == "undoCosmeticFilter"sv) {
        auto id = params.get_u64("filterJournalId"sv).value_or(0);
        auto res = undo_cosmetic_filter_rule(id);
        auto undone = res.get_bool("undone"sv).value_or(false);
        auto error = res.get_string("error"sv).map([](auto const& text) { return String { text }; });
        send_reply(request_id, undone, move(res), move(error));
        return;
    }
    if (command == "getEditJournal"sv) {
        send_reply(request_id, true, serialize_edit_journal());
        return;
    }
    if (command == "getNetworkLog"sv) {
        send_reply(request_id, true, serialize_network_log());
        return;
    }
    if (command == "clearNetworkLog"sv) {
        m_network_entries.clear();
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "setPreserveLog"sv) {
        if (auto console_p = params.get_bool("console"sv); console_p.has_value())
            m_preserve_console_log = *console_p;
        if (auto net_p = params.get_bool("network"sv); net_p.has_value())
            m_preserve_network_log = *net_p;
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "clearConsoleLog"sv) {
        m_console_entries.clear();
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "getDomBreakpoints"sv) {
        send_reply(request_id, true, serialize_dom_breakpoints());
        return;
    }

    auto* view = target_view();
    if (!view) {
        send_reply(request_id, false, JsonValue {}, "Target tab is no longer open."_string);
        return;
    }

    // Reloading is how a crashed target comes back, and it acts on no document, so it needs no current generation.
    if (command == "reconnectTarget"sv) {
        view->reload();
        send_reply(request_id, true, JsonObject {});
        return;
    }

    // Validate document generation when caller supplies one for mutations.
    if (auto expected_gen = params.get_u64("documentGeneration"sv); expected_gen.has_value() && *expected_gen != m_document_generation) {
        send_reply(request_id, false, JsonValue {}, "Document changed before command could apply."_string);
        return;
    }

    if (m_target_crashed) {
        send_reply(request_id, false, {}, "Reload the target tab to reconnect."_string);
        return;
    }

    // Node Picker
    if (command == "setPickerActive"sv) {
        set_node_picker_active(params.get_bool("active"sv).value_or(!m_picker_active));
        send_reply(request_id, true, JsonObject {});
        return;
    }

    // DOM Tree & Inspection
    if (command == "getDomTree"sv) {
        view->dispatch_inspector_engine_command(request_id, "getDomTree"_string, {}, [weak_this = make_weak_ptr(), request_id, generation = m_document_generation](bool ok, JsonValue result, Optional<String> error) {
            auto self = weak_this.strong_ref();
            if (!self || self->m_document_generation != generation)
                return;
            if (!ok) {
                self->send_reply(request_id, false, {}, move(error));
                return;
            }
            JsonObject snapshot;
            snapshot.set("tree"sv, move(result));
            snapshot.set("sequence"sv, self->m_dom_sequence);
            snapshot.set("documentGeneration"sv, generation);
            self->send_reply(request_id, true, move(snapshot));
        });
        return;
    }
    if (command == "inspectNode"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto type_str = params.get_string("type"sv).value_or("appliedStyleRules"_string);
        auto pseudo_str = params.get_string("pseudoElement"sv);
        auto pseudo = parse_pseudo_element(pseudo_str.has_value() ? Optional<StringView> { pseudo_str->bytes_as_string_view() } : OptionalNone {});
        JsonValue options = params.get("options"sv).value_or(JsonValue {});

        if (node_id != 0) {
            m_selected_node_id = node_id;
            record_selection_history(node_id);
        }

        DOMNodeProperties::Type prop_type = DOMNodeProperties::Type::AppliedStyleRules;
        if (type_str == "computedStyle"sv)
            prop_type = DOMNodeProperties::Type::ComputedStyle;
        else if (type_str == "layout"sv)
            prop_type = DOMNodeProperties::Type::Layout;
        else if (type_str == "usedFonts"sv)
            prop_type = DOMNodeProperties::Type::UsedFonts;

        if (m_node_properties_requests.size() >= max_edit_journal_entries) {
            send_reply(request_id, false, JsonValue {}, "Too many pending property requests."_string);
            return;
        }
        auto renderer_request_id = view->allocate_inspector_request_id();
        m_node_properties_requests.set(renderer_request_id, NodePropertiesRequest { node_id, pseudo_str.map([](auto const& name) { return String { name }; }) });
        view->inspect_dom_node(node_id, prop_type, pseudo, move(options), renderer_request_id);
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "highlightNode"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto pseudo_str = params.get_string("pseudoElement"sv);
        auto pseudo = parse_pseudo_element(pseudo_str.has_value() ? Optional<StringView> { pseudo_str->bytes_as_string_view() } : OptionalNone {});
        if (node_id == 0)
            view->clear_highlighted_dom_node();
        else
            view->highlight_dom_node(node_id, pseudo);
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "clearHighlight"sv) {
        view->clear_highlighted_dom_node();
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "highlightGrid"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto options = params.get("options"sv).value_or(JsonObject {});
        view->highlight_grid(node_id, move(options));
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "clearGridHighlight"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        view->clear_grid_highlight(node_id);
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "highlightFlexbox"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto options = params.get("options"sv).value_or(JsonObject {});
        view->highlight_flexbox(node_id, move(options));
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "clearFlexboxHighlight"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        view->clear_flexbox_highlight(node_id);
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "inspectGridLayouts"sv) {
        auto root_id = Web::UniqueNodeID(params.get_i64("rootNodeId"sv).value_or(0));
        if (m_pending_grid_layouts_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_grid_layouts_request_id = request_id;
        view->inspect_grid_layouts(root_id);
        return;
    }
    if (command == "inspectCurrentGrid"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        if (m_pending_current_grid_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_current_grid_request_id = request_id;
        view->inspect_current_grid(node_id);
        return;
    }
    if (command == "inspectCurrentFlexbox"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto only_parents = params.get_bool("onlyLookAtParents"sv).value_or(false);
        if (m_pending_current_flexbox_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_current_flexbox_request_id = request_id;
        view->inspect_current_flexbox(node_id, only_parents);
        return;
    }

    // WebContent does not answer some reads it cannot serve, such as the markup of a node that left the document, and
    // the frontend gives up on a read after ten seconds. A read still waiting by then is abandoned, so that one
    // unanswerable read does not block every later one of its kind until the page navigates.
    auto take_read_slot = [&](Optional<u64>& slot, MonotonicTime& started) {
        auto now = MonotonicTime::now_coarse();
        if (slot.has_value()) {
            if (now - started < AK::Duration::from_seconds(10)) {
                send_reply(request_id, false, {}, "The previous request is still running."_string);
                return false;
            }
            send_reply(*slot, false, {}, "The target did not answer in time."_string);
        }
        slot = request_id;
        started = now;
        return true;
    };

    // DOM Editing (with Flight Recorder journal entries!)
    if (command == "getNodeHtml"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto outer = params.get_bool("outer"sv).value_or(true);
        if (!take_read_slot(m_pending_html_request_id, m_pending_html_request_started))
            return;
        if (outer)
            view->get_dom_node_outer_html(node_id);
        else
            view->get_dom_node_inner_html(node_id);
        return;
    }
    if (command == "setNodeOuterHtml"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto html = params.get_string("html"sv).value_or(String {});
        auto before_html = params.get_string("beforeHtml"sv).value_or(String {});
        if (m_pending_edit_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_edit_request_id = request_id;
        view->set_dom_node_outer_html(node_id, html);
        EditJournalEntry entry;
        entry.request_id = request_id;
        entry.kind = "dom-outer-html"_string;
        entry.target_label = params.get_string("label"sv).value_or(MUST(String::formatted("Node #{}", node_id.value())));
        entry.group_key = MUST(String::formatted("node:{}", node_id.value()));
        entry.before_text = before_html;
        entry.after_text = html;
        m_pending_edit_journal_entry = move(entry);
        return;
    }
    if (command == "setNodeTag"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto tag = params.get_string("tag"sv).value_or(String {});
        auto before_tag = params.get_string("beforeTag"sv).value_or(String {});
        if (m_pending_edit_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_edit_request_id = request_id;
        view->set_dom_node_tag(node_id, Utf16FlyString::from_utf8(tag));
        EditJournalEntry entry;
        entry.request_id = request_id;
        entry.kind = "dom-tag"_string;
        entry.target_label = MUST(String::formatted("<{}> -> <{}>", before_tag, tag));
        entry.group_key = MUST(String::formatted("node:{}", node_id.value()));
        entry.before_text = before_tag;
        entry.after_text = tag;
        m_pending_edit_journal_entry = move(entry);
        return;
    }
    if (command == "removeNode"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        if (m_pending_edit_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_edit_request_id = request_id;
        view->remove_dom_node(node_id);
        EditJournalEntry entry;
        entry.request_id = request_id;
        entry.kind = "dom-remove"_string;
        entry.target_label = params.get_string("label"sv).value_or(MUST(String::formatted("Node #{}", node_id.value())));
        entry.group_key = MUST(String::formatted("node:{}", node_id.value()));
        entry.before_text = params.get_string("beforeHtml"sv).value_or(String {});
        entry.after_text = "(removed)"_string;
        m_pending_edit_journal_entry = move(entry);
        return;
    }
    if (command == "cloneNode"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        if (m_pending_edit_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_edit_request_id = request_id;
        view->clone_dom_node(node_id);
        return;
    }
    if (command == "createChildElement"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        if (m_pending_edit_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_edit_request_id = request_id;
        view->create_child_element(node_id);
        return;
    }
    if (command == "createChildText"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        if (m_pending_edit_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_edit_request_id = request_id;
        view->create_child_text_node(node_id);
        return;
    }
    if (command == "insertNodeBefore"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto parent_id = Web::UniqueNodeID(params.get_i64("parentNodeId"sv).value_or(0));
        Optional<Web::UniqueNodeID> sibling_id;
        if (auto raw = params.get_i64("siblingNodeId"sv); raw.has_value() && *raw != 0)
            sibling_id = Web::UniqueNodeID(*raw);
        if (m_pending_edit_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_edit_request_id = request_id;
        view->insert_dom_node_before(node_id, parent_id, sibling_id);
        return;
    }
    if (command == "takeNodeScreenshot"sv) {
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto promise = view->take_dom_node_screenshot(node_id);
        promise->when_resolved([weak_this = make_weak_ptr(), request_id](LexicalPath const& path) {
            if (auto self = weak_this.strong_ref()) {
                JsonObject res;
                res.set("path"sv, MUST(String::from_byte_string(path.string())));
                self->send_reply(request_id, true, move(res));
            }
        });
        promise->when_rejected([weak_this = make_weak_ptr(), request_id](Error const& err) {
            if (auto self = weak_this.strong_ref())
                self->send_reply(request_id, false, JsonValue {}, MUST(String::formatted("{}", err)));
        });
        return;
    }

    // Accessibility
    if (command == "getAccessibilityTree"sv) {
        if (!take_read_slot(m_pending_a11y_request_id, m_pending_a11y_request_started))
            return;
        view->inspect_accessibility_tree();
        return;
    }

    // Stylesheets
    if (command == "listStyleSheets"sv) {
        if (m_pending_stylesheets_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_stylesheets_request_id = request_id;
        view->list_style_sheets();
        return;
    }
    if (command == "getStyleSheetSource"sv) {
        auto id = parse_style_sheet_identifier(params);
        if (!id.has_value()) {
            send_reply(request_id, false, JsonValue {}, "Invalid stylesheet identifier."_string);
            return;
        }
        if (!take_read_slot(m_pending_stylesheet_source_request_id, m_pending_stylesheet_source_request_started))
            return;
        view->request_style_sheet_source(*id);
        return;
    }

    // Console
    if (command == "consoleEvaluate"sv) {
        auto source = params.get_string("expression"sv).value_or(String {});
        if (m_pending_console_eval_request_id.has_value()) {
            send_reply(request_id, false, {}, "The previous request is still running."_string);
            return;
        }
        m_pending_console_eval_request_id = request_id;
        view->js_console_input(source);
        return;
    }
    if (command == "consoleComplete"sv) {
        auto input = params.get_string("input"sv).value_or(String {});
        view->complete_js_console_input(input, [weak_this = make_weak_ptr(), request_id](ErrorOr<DevTools::DevToolsDelegate::ConsoleInputCompletion> completion) {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            if (completion.is_error()) {
                self->send_reply(request_id, false, JsonValue {}, MUST(String::formatted("{}", completion.error())));
                return;
            }
            JsonObject res;
            res.set("matchProp"sv, completion.value().match_prop);
            JsonArray matches;
            if (completion.value().matches.has_value()) {
                for (auto const& m : *completion.value().matches)
                    matches.must_append(m);
            }
            res.set("matches"sv, move(matches));
            self->send_reply(request_id, true, move(res));
        });
        return;
    }

    // Sources & Debugger
    if (command == "listSources"sv) {
        view->retrieve_devtools_sources([weak_this = make_weak_ptr(), request_id](ErrorOr<Vector<Web::HTML::ScriptRegistryDescription>> sources) {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            if (sources.is_error()) {
                self->send_reply(request_id, false, JsonValue {}, MUST(String::formatted("{}", sources.error())));
                return;
            }
            JsonArray arr;
            for (auto const& src : sources.value())
                arr.must_append(serialize_script_description(src));
            JsonObject res;
            res.set("sources"sv, move(arr));
            self->send_reply(request_id, true, move(res));
        });
        return;
    }
    if (command == "getSourceContent"sv) {
        auto doc_id = Web::UniqueNodeID(params.get_i64("documentId"sv).value_or(0));
        auto script_id = params.get_u64("scriptId"sv).value_or(0);
        Web::HTML::ScriptRegistryIdentifier id { doc_id, script_id };
        view->on_received_devtools_source.set(id, [weak_this = make_weak_ptr(), request_id](Optional<Web::HTML::ScriptRegistryContent> content) {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            if (!content.has_value()) {
                self->send_reply(request_id, false, JsonValue {}, "Source content is no longer available."_string);
                return;
            }
            JsonObject res;
            res.set("contentType"sv, content->content_type.to_utf8_but_should_be_ported_to_utf16());
            res.set("text"sv, content->text.to_utf8_but_should_be_ported_to_utf16());
            self->send_reply(request_id, true, move(res));
        });
        view->request_devtools_source(id);
        return;
    }
    auto acquire_debugger = [&] {
        m_debugger_attached = view->attach_debugger_for_owner(ViewImplementation::InteractiveOwner::LocalInspector);
        if (!m_debugger_attached)
            send_reply(request_id, false, {}, "Remote DevTools controls the debugger."_string);
        return m_debugger_attached;
    };
    if (command == "attachDebugger"sv) {
        if (acquire_debugger())
            send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "detachDebugger"sv) {
        m_debugger_attached = false;
        view->detach_debugger_for_owner(ViewImplementation::InteractiveOwner::LocalInspector);
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "interruptDebugger"sv) {
        if (!acquire_debugger())
            return;
        view->interrupt_debugger();
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "resumeDebugger"sv) {
        if (view->debugger_owner() != ViewImplementation::InteractiveOwner::LocalInspector) {
            send_reply(request_id, false, {}, "This Inspector does not control the debugger."_string);
            return;
        }
        auto mode_str = params.get_string("mode"sv).value_or("continue"_string);
        auto mode = DebuggerResumeMode::Continue;
        if (mode_str == "stepInto"sv)
            mode = DebuggerResumeMode::StepInto;
        else if (mode_str == "stepOut"sv)
            mode = DebuggerResumeMode::StepOut;
        else if (mode_str == "stepOver"sv)
            mode = DebuggerResumeMode::StepOver;
        view->resume_debugger(mode);
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (command == "setBreakpoint"sv || command == "removeBreakpoint"sv) {
        if (!acquire_debugger())
            return;
        DebuggerBreakpointLocation loc;
        if (auto doc_id = params.get_i64("documentId"sv); doc_id.has_value()) {
            if (auto script_id = params.get_u64("scriptId"sv); script_id.has_value())
                loc.source_id = Web::HTML::ScriptRegistryIdentifier { Web::UniqueNodeID(*doc_id), *script_id };
        }
        loc.filename = Utf16String::from_utf8(params.get_string("filename"sv).value_or(String {}));
        loc.line = params.get_u32("line"sv).value_or(1);
        if (auto col = params.get_u32("column"sv); col.has_value())
            loc.column = *col;

        auto cb = [weak_this = make_weak_ptr(), request_id](ErrorOr<void> result) {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            if (result.is_error())
                self->send_reply(request_id, false, JsonValue {}, MUST(String::formatted("{}", result.error())));
            else
                self->send_reply(request_id, true, JsonObject {});
        };

        if (command == "setBreakpoint"sv) {
            DebuggerBreakpointOptions opts;
            if (auto cond = params.get_string("condition"sv); cond.has_value() && !cond->is_empty())
                opts.condition = Utf16String::from_utf8(*cond);
            if (auto log_val = params.get_string("logValue"sv); log_val.has_value() && !log_val->is_empty())
                opts.log_value = Utf16String::from_utf8(*log_val);
            opts.show_stacktrace = params.get_bool("showStacktrace"sv).value_or(false);
            view->set_debugger_breakpoint(move(loc), move(opts), move(cb));
        } else {
            view->remove_debugger_breakpoint(move(loc), move(cb));
        }
        return;
    }
    if (command == "getDebuggerEnvironments"sv) {
        auto frame_id = params.get_u64("frameId"sv).value_or(0);
        view->retrieve_debugger_environments(frame_id, [weak_this = make_weak_ptr(), request_id](ErrorOr<Vector<DebuggerEnvironment>> result) {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            if (result.is_error()) {
                self->send_reply(request_id, false, JsonValue {}, MUST(String::formatted("{}", result.error())));
                return;
            }
            JsonArray envs;
            for (auto const& env : result.value()) {
                JsonObject obj;
                obj.set("id"sv, env.id);
                obj.set("type"sv, env.type == DebuggerEnvironmentType::Function ? "function"_string : (env.type == DebuggerEnvironmentType::Object ? "object"_string : "block"_string));
                if (env.function_name.has_value())
                    obj.set("functionName"sv, env.function_name->to_utf8_but_should_be_ported_to_utf16());
                JsonArray bindings;
                for (auto const& b : env.bindings) {
                    JsonObject bo;
                    bo.set("name"sv, b.name.to_utf8_but_should_be_ported_to_utf16());
                    bo.set("value"sv, serialize_debugger_value(b.value));
                    bo.set("writable"sv, b.writable);
                    bindings.must_append(move(bo));
                }
                obj.set("bindings"sv, move(bindings));
                envs.must_append(move(obj));
            }
            JsonObject res;
            res.set("environments"sv, move(envs));
            self->send_reply(request_id, true, move(res));
        });
        return;
    }
    if (command == "evaluateInDebuggerFrame"sv) {
        auto frame_id = params.get_u64("frameId"sv).value_or(0);
        auto expr = params.get_string("expression"sv).value_or(String {});
        view->evaluate_javascript_in_debugger_frame(frame_id, expr, [weak_this = make_weak_ptr(), request_id](ErrorOr<DebuggerEvaluationResult, String> result) {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            if (result.is_error()) {
                self->send_reply(request_id, false, JsonValue {}, result.error());
                return;
            }
            JsonObject res;
            res.set("value"sv, serialize_debugger_value(result.value().value));
            res.set("isThrow"sv, result.value().is_throw);
            self->send_reply(request_id, true, move(res));
        });
        return;
    }
    if (command == "getObjectProperties"sv) {
        auto object_id = params.get_u64("objectId"sv).value_or(0);
        auto scope = params.get_string("scope"sv).value_or("console"_string) == "pause"sv
            ? DebuggerObjectScope::Pause
            : DebuggerObjectScope::Console;
        view->retrieve_debugger_object_properties(object_id, scope, [weak_this = make_weak_ptr(), request_id](ErrorOr<DebuggerObjectProperties, String> result) {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            if (result.is_error()) {
                self->send_reply(request_id, false, JsonValue {}, result.error());
                return;
            }
            JsonObject res;
            if (result.value().prototype.has_value())
                res.set("prototype"sv, serialize_debugger_value(*result.value().prototype));
            JsonArray props;
            for (auto const& p : result.value().properties) {
                JsonObject po;
                po.set("name"sv, p.name.to_utf8_but_should_be_ported_to_utf16());
                if (p.value.has_value())
                    po.set("value"sv, serialize_debugger_value(*p.value));
                po.set("hasGetter"sv, p.getter.has_value());
                po.set("hasSetter"sv, p.setter.has_value());
                po.set("writable"sv, p.writable);
                po.set("enumerable"sv, p.enumerable);
                po.set("configurable"sv, p.configurable);
                props.must_append(move(po));
            }
            res.set("properties"sv, move(props));
            self->send_reply(request_id, true, move(res));
        });
        return;
    }

    // Storage & Cookies (Application panel)
    if (command == "getStorage"sv) {
        auto storage_type_str = params.get_string("storageType"sv).value_or("localStorage"_string);
        if (storage_type_str == "cookies"sv) {
            auto cookies = view->session().cookie_jar->get_all_cookies_webdriver(view->url());
            JsonArray arr;
            for (auto const& c : cookies) {
                JsonObject obj;
                obj.set("name"sv, c.name);
                obj.set("value"sv, c.value);
                obj.set("domain"sv, c.domain);
                obj.set("path"sv, c.path);
                obj.set("secure"sv, c.secure);
                obj.set("httpOnly"sv, c.http_only);
                obj.set("persistent"sv, c.persistent);
                arr.must_append(move(obj));
            }
            JsonObject res;
            res.set("items"sv, move(arr));
            send_reply(request_id, true, move(res));
            return;
        }
        auto endpoint = storage_type_str == "sessionStorage"sv
            ? Web::StorageAPI::StorageEndpointType::SessionStorage
            : Web::StorageAPI::StorageEndpointType::LocalStorage;
        auto internal_id = m_next_internal_storage_request_id++;
        view->on_received_storage_items.set(internal_id, [weak_this = make_weak_ptr(), request_id](ErrorOr<Vector<DevTools::DevToolsDelegate::StorageItem>> items) {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            if (items.is_error()) {
                self->send_reply(request_id, false, JsonValue {}, MUST(String::formatted("{}", items.error())));
                return;
            }
            JsonArray arr;
            for (auto const& item : items.value()) {
                JsonObject obj;
                obj.set("key"sv, item.name);
                obj.set("value"sv, item.value);
                arr.must_append(move(obj));
            }
            JsonObject res;
            res.set("items"sv, move(arr));
            self->send_reply(request_id, true, move(res));
        });
        view->inspect_storage(endpoint, internal_id);
        return;
    }
    if (command == "inspectIndexedDB"sv) {
        view->inspect_indexed_database_storage([weak_this = make_weak_ptr(), request_id](ErrorOr<JsonObject> result) {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            if (result.is_error())
                self->send_reply(request_id, false, JsonValue {}, MUST(String::formatted("{}", result.error())));
            else
                self->send_reply(request_id, true, result.release_value());
        });
        return;
    }

    // Engine Lens (live LayoutTree, DisplayList, StackingContextTree from LibWeb!)
    if (command == "getEngineLens"sv) {
        auto lens = params.get_string("lens"sv).value_or("layout"_string);
        PageInfoType type = PageInfoType::LayoutTree;
        if (lens == "paint"sv || lens == "displayList"sv)
            type = PageInfoType::DisplayList;
        else if (lens == "stacking"sv)
            type = PageInfoType::StackingContextTree;
        else if (lens == "accessibility"sv)
            type = PageInfoType::AccessibilityTree;

        auto promise = view->request_internal_page_info(type);
        promise->when_resolved([weak_this = make_weak_ptr(), request_id, lens](String const& text) {
            if (auto self = weak_this.strong_ref()) {
                JsonObject res;
                res.set("lens"sv, lens);
                res.set("documentGeneration"sv, self->m_document_generation);
                res.set("text"sv, text);
                self->send_reply(request_id, true, move(res));
            }
        });
        promise->when_rejected([weak_this = make_weak_ptr(), request_id](Error const& err) {
            if (auto self = weak_this.strong_ref())
                self->send_reply(request_id, false, JsonValue {}, MUST(String::formatted("{}", err)));
        });
        return;
    }

    if (command == "undoEdit"sv) {
        auto id = params.get_u64("id"sv).value_or(0);
        auto entry = m_edit_journal.find_if([&](auto const& candidate) { return candidate.id == id; });
        if (entry == m_edit_journal.end() || entry->undone || entry->undo_payload.is_empty()) {
            send_reply(request_id, false, {}, "This edit cannot be undone."_string);
            return;
        }
        // The inverse came from the renderer, which is untrusted: one without an action cannot be undone, and must not
        // bring down this process.
        auto const action = entry->undo_payload.get_string("action"sv);
        if (!action.has_value() || action->is_empty()) {
            send_reply(request_id, false, {}, "This edit cannot be undone."_string);
            return;
        }
        view->dispatch_inspector_engine_command(request_id, *action, entry->undo_payload, [weak_this = make_weak_ptr(), generation = m_document_generation, request_id, id](bool ok, JsonValue result, Optional<String> error) {
            auto self = weak_this.strong_ref();
            if (!self || self->m_document_generation != generation)
                return;
            if (!ok || !result.is_object() || result.as_object().get_bool("conflict"sv).value_or(false)) {
                self->send_reply(request_id, false, {}, error.value_or("The page changed since this edit."_string));
                return;
            }
            for (auto& edit : self->m_edit_journal) {
                if (edit.id == id) {
                    edit.undone = true;
                    break;
                }
            }
            auto journal = self->serialize_edit_journal();
            self->send_event("editJournalUpdated"sv, journal);
            self->send_reply(request_id, true, move(journal));
        });
        return;
    }

    if (command == "setRenderingOverrides"sv) {
        auto scheme = params.get_string("prefersColorScheme"sv).value_or("auto"_string);
        if (scheme == "auto"sv) {
            view->send_preferences_to_page(view->page());
            m_active_rendering_overrides = {};
        } else {
            JsonObject overrides;
            overrides.set("colorScheme"sv, scheme);
            view->dispatch_inspector_engine_command(0, "setRenderingOverrides"_string, overrides, {});
            m_active_rendering_overrides = params;
        }
        send_reply(request_id, true, JsonObject {});
        return;
    }
    auto action = command == "engineCommand"sv ? params.get_string("action"sv).value_or(String {}) : MUST(String::from_utf8(command));
    if (action == "setDeviceMode"sv && !params.get_bool("enabled"sv).value_or(true)) {
        view->page().async_set_viewport(view->viewport_size(), view->device_pixel_ratio(), view->is_fullscreen());
        m_active_device_overrides = {};
        send_reply(request_id, true, JsonObject {});
        return;
    }
    if (action.is_one_of(
            "setForcedPseudoState"sv,
            "clearForcedPseudoStates"sv,
            "editStyleDeclaration"sv,
            "toggleStyleDeclaration"sv,
            "editStyleRuleSelector"sv,
            "addStyleRule"sv,
            "setNodeText"sv,
            "setAttribute"sv,
            "searchDom"sv,
            "setNodeHidden"sv,
            "getEventListeners"sv,
            "removeEventListener"sv,
            "getAccessibilityNodeDetails"sv,
            "previewCosmeticFilter"sv,
            "clearCosmeticFilterPreview"sv,
            "getAnimations"sv,
            "pauseAllAnimations"sv,
            "setAnimationsPlaybackRate"sv,
            "setDeviceMode"sv,
            "setSensorOverrides"sv,
            "runCssOverviewAudit"sv)) {
        bool edits_style = action.is_one_of("editStyleDeclaration"sv, "toggleStyleDeclaration"sv, "editStyleRuleSelector"sv, "addStyleRule"sv);
        bool edits_dom = action.is_one_of("setNodeText"sv, "setAttribute"sv);
        if (action == "setDeviceMode"sv)
            m_active_device_overrides = params;
        if (action == "setSensorOverrides"sv)
            m_active_sensor_overrides = params.get_bool("enabled"sv).value_or(true) ? params : JsonObject {};
        view->dispatch_inspector_engine_command(request_id, action, params, [weak_this = make_weak_ptr(), generation = m_document_generation, request_id, action, params, edits_style, edits_dom](bool ok, JsonValue result, Optional<String> error) {
            auto self = weak_this.strong_ref();
            if (!self || generation != self->m_document_generation)
                return;
            if (ok && (edits_style || edits_dom) && result.is_object() && !result.as_object().get_bool("conflict"sv).value_or(false)) {
                auto const& edit = result.as_object();
                EditJournalEntry entry;
                entry.request_id = request_id;
                entry.kind = edits_style ? "style"_string : "dom"_string;
                entry.target_label = edit.get_string("selector"sv).value_or(params.get_string("property"sv).value_or(action));
                entry.group_key = edit.get_string("sheetId"sv).value_or(MUST(String::formatted("node:{}", params.get_i64("nodeId"sv).value_or(0))));
                entry.before_text = edit.get_string("beforeText"sv).value_or(String {});
                entry.after_text = edit.get_string("afterText"sv).value_or(String {});
                entry.base_revision = params.get_u64("baseRevision"sv).value_or(0);
                entry.resulting_revision = edit.get_u64("sheetRevision"sv).value_or(0);
                if (auto inverse = edit.get_object("inverse"sv); inverse.has_value())
                    entry.undo_payload = *inverse;
                self->record_edit_entry(move(entry));
                if (self->m_completed_mutation_replies.size() >= max_edit_journal_entries)
                    self->m_completed_mutation_replies.clear();
                self->m_completed_mutation_replies.set(request_id, edit);
            }
            self->send_reply(request_id, ok, move(result), move(error));
        });
        return;
    }

    if (command == "setDomBreakpoint"sv || command == "removeDomBreakpoint"sv) {
        bool removing = command == "removeDomBreakpoint"sv;
        auto descriptor_id = params.get_u64("id"sv);
        auto node_id = Web::UniqueNodeID(params.get_i64("nodeId"sv).value_or(0));
        auto type = params.get_string("type"sv).value_or(String {});
        auto existing = m_dom_breakpoints.find_if([&](auto const& candidate) {
            if (descriptor_id.has_value())
                return candidate.id == *descriptor_id;
            return candidate.node_id == node_id && candidate.type == type;
        });
        if (removing && existing == m_dom_breakpoints.end()) {
            send_reply(request_id, false, {}, "DOM breakpoint is no longer available."_string);
            return;
        }
        bool enabled = !removing && params.get_bool("enabled"sv).value_or(true);
        if (enabled && !acquire_debugger())
            return;

        auto descriptor = existing == m_dom_breakpoints.end()
            ? DOMBreakpointDescriptor {
                  .id = m_next_dom_breakpoint_id++,
                  .url = view->url().serialize(),
                  .node_path = params.get_string("nodePath"sv).value_or(String {}),
                  .node_id = node_id,
                  .type = type,
                  .enabled = enabled,
              }
            : *existing;
        descriptor.enabled = enabled;
        auto complete = [weak_this = make_weak_ptr(), generation = m_document_generation, request_id, descriptor, removing](bool ok, JsonValue result, Optional<String> error) mutable {
            auto self = weak_this.strong_ref();
            if (!self || self->m_document_generation != generation)
                return;
            if (!ok) {
                self->send_reply(request_id, false, {}, move(error));
                return;
            }
            if (result.is_object() && !result.as_object().get_bool("canRestore"sv).value_or(false) && descriptor.enabled)
                descriptor.node_path = {};
            self->m_dom_breakpoints.remove_all_matching([&](auto const& candidate) { return candidate.id == descriptor.id; });
            if (!removing)
                self->m_dom_breakpoints.append(move(descriptor));
            auto data = self->serialize_dom_breakpoints();
            self->send_event("domBreakpointsChanged"sv, data);
            self->send_reply(request_id, true, move(data));
        };
        if (!descriptor.resolved || descriptor.node_id == 0) {
            complete(removing, {}, "DOM breakpoint target could not be resolved."_string);
            return;
        }
        JsonObject engine_params;
        engine_params.set("nodeId"sv, descriptor.node_id.value());
        engine_params.set("type"sv, descriptor.type);
        engine_params.set("nodePath"sv, descriptor.node_path);
        view->dispatch_inspector_engine_command(request_id, enabled ? "setDomBreakpoint"_string : "removeDomBreakpoint"_string, engine_params, move(complete));
        return;
    }

    send_reply(request_id, false, JsonValue {}, MUST(String::formatted("Unknown inspector command: {}", command)));
}

void InspectorSession::did_receive_dom_node_properties(u64 request_id, DOMNodeProperties const& properties)
{
    auto request = m_node_properties_requests.take(request_id);
    if (!request.has_value())
        return;
    JsonObject payload;
    payload.set("nodeId"sv, request->node_id.value());
    if (request->pseudo_element.has_value())
        payload.set("pseudoElement"sv, *request->pseudo_element);
    switch (properties.type) {
    case DOMNodeProperties::Type::AppliedStyleRules:
        payload.set("type"sv, "appliedStyleRules"_string);
        break;
    case DOMNodeProperties::Type::ComputedStyle:
        payload.set("type"sv, "computedStyle"_string);
        break;
    case DOMNodeProperties::Type::Layout:
        payload.set("type"sv, "layout"_string);
        break;
    case DOMNodeProperties::Type::UsedFonts:
        payload.set("type"sv, "usedFonts"_string);
        break;
    }
    payload.set("properties"sv, properties.properties);
    payload.set("documentGeneration"sv, m_document_generation);
    send_event("domNodeProperties"sv, move(payload));
}

void InspectorSession::did_receive_dom_mutation(Mutation const& mutation)
{
    ++m_dom_sequence;
    JsonObject payload;
    payload.set("sequence"sv, m_dom_sequence);
    payload.set("documentGeneration"sv, m_document_generation);
    payload.set("type"sv, mutation.type);
    payload.set("target"sv, mutation.target.value());
    payload.set("serializedTarget"sv, mutation.serialized_target);

    mutation.mutation.visit(
        [&](AttributeMutation const& attr) {
            payload.set("attributeName"sv, MUST(String::formatted("{}", attr.attribute_name)));
            if (attr.new_value.has_value())
                payload.set("newValue"sv, attr.new_value->to_utf8_but_should_be_ported_to_utf16());
        },
        [&](CharacterDataMutation const& cd) {
            payload.set("newValue"sv, cd.new_value);
        },
        [&](ChildListMutation const& cl) {
            JsonArray added;
            for (auto id : cl.added)
                added.must_append(id.value());
            JsonArray removed;
            for (auto id : cl.removed)
                removed.must_append(id.value());
            payload.set("added"sv, move(added));
            payload.set("removed"sv, move(removed));
            payload.set("targetChildCount"sv, static_cast<u64>(cl.target_child_count));
        });

    send_event("domMutation"sv, move(payload));
}

void InspectorSession::did_finish_editing_dom_node(Optional<Web::UniqueNodeID> node_id)
{
    JsonObject payload;
    if (node_id.has_value())
        payload.set("nodeId"sv, node_id->value());
    if (auto req = exchange(m_pending_edit_request_id, {}); req.has_value()) {
        auto journal_entry = exchange(m_pending_edit_journal_entry, {});
        if (!node_id.has_value()) {
            send_reply(*req, false, {}, "The DOM edit could not be applied."_string);
            return;
        }
        if (journal_entry.has_value())
            record_edit_entry(journal_entry.release_value());
        if (m_completed_mutation_replies.size() >= max_edit_journal_entries)
            m_completed_mutation_replies.clear();
        m_completed_mutation_replies.set(*req, payload);
        send_reply(*req, true, payload);
    }
    send_event("domNodeEdited"sv, move(payload));
}

void InspectorSession::did_receive_dom_node_html(String const& html)
{
    JsonObject payload;
    payload.set("html"sv, html);
    if (auto req = exchange(m_pending_html_request_id, {}); req.has_value())
        send_reply(*req, true, payload);
    send_event("domNodeHtml"sv, move(payload));
}

void InspectorSession::did_receive_grid_layouts(JsonArray const& layouts)
{
    JsonObject payload;
    payload.set("layouts"sv, layouts);
    if (auto req = exchange(m_pending_grid_layouts_request_id, {}); req.has_value())
        send_reply(*req, true, payload);
    send_event("gridLayouts"sv, move(payload));
}

void InspectorSession::did_receive_current_grid(Optional<JsonObject> const& grid)
{
    JsonObject payload;
    if (grid.has_value())
        payload.set("grid"sv, *grid);
    if (auto req = exchange(m_pending_current_grid_request_id, {}); req.has_value())
        send_reply(*req, true, payload);
    send_event("currentGrid"sv, move(payload));
}

void InspectorSession::did_receive_current_flexbox(Optional<JsonObject> const& flexbox)
{
    JsonObject payload;
    if (flexbox.has_value())
        payload.set("flexbox"sv, *flexbox);
    if (auto req = exchange(m_pending_current_flexbox_request_id, {}); req.has_value())
        send_reply(*req, true, payload);
    send_event("currentFlexbox"sv, move(payload));
}

void InspectorSession::did_receive_accessibility_tree(JsonObject const& tree)
{
    JsonObject payload;
    payload.set("tree"sv, tree);
    if (auto req = exchange(m_pending_a11y_request_id, {}); req.has_value())
        send_reply(*req, true, payload);
    send_event("accessibilityTree"sv, move(payload));
}

void InspectorSession::did_receive_style_sheet_list(Vector<Web::CSS::StyleSheetIdentifier> const& stylesheets)
{
    JsonArray arr;
    for (auto const& sheet : stylesheets)
        arr.must_append(serialize_style_sheet_identifier(sheet));
    JsonObject payload;
    payload.set("styleSheets"sv, move(arr));
    if (auto req = exchange(m_pending_stylesheets_request_id, {}); req.has_value())
        send_reply(*req, true, payload);
    send_event("styleSheetList"sv, move(payload));
}

void InspectorSession::did_receive_style_sheet_source(Web::CSS::StyleSheetIdentifier const& identifier, URL::URL const& base_url, Utf16String const& source)
{
    JsonObject payload;
    payload.set("identifier"sv, serialize_style_sheet_identifier(identifier));
    payload.set("baseUrl"sv, base_url.serialize());
    payload.set("source"sv, source.to_utf8_but_should_be_ported_to_utf16());
    if (auto req = exchange(m_pending_stylesheet_source_request_id, {}); req.has_value())
        send_reply(*req, true, payload);
    send_event("styleSheetSource"sv, move(payload));
}

void InspectorSession::did_add_devtools_source(Web::HTML::ScriptRegistryDescription const& source)
{
    send_event("sourceAdded"sv, serialize_script_description(source));
}

void InspectorSession::did_pause_debugger(DebuggerPause const& pause)
{
    JsonObject payload;
    StringView reason = "breakpoint"sv;
    switch (pause.reason) {
    case DebuggerPauseReason::Breakpoint:
        reason = "breakpoint"sv;
        break;
    case DebuggerPauseReason::BreakpointConditionThrown:
        reason = "breakpointConditionError"sv;
        break;
    case DebuggerPauseReason::DebuggerStatement:
        reason = "debuggerStatement"sv;
        break;
    case DebuggerPauseReason::Entry:
        reason = "entry"sv;
        break;
    case DebuggerPauseReason::Exception:
        reason = "exception"sv;
        break;
    case DebuggerPauseReason::ResumeLimit:
        reason = "step"sv;
        break;
    }
    payload.set("reason"sv, MUST(String::from_utf8(reason)));
    if (pause.reason_message.has_value())
        payload.set("reasonMessage"sv, pause.reason_message->to_utf8_but_should_be_ported_to_utf16());
    if (pause.exception.has_value())
        payload.set("exception"sv, serialize_debugger_value(*pause.exception));

    JsonArray frames;
    for (auto const& f : pause.frames) {
        JsonObject fo;
        fo.set("id"sv, f.id);
        fo.set("displayName"sv, f.display_name.to_utf8_but_should_be_ported_to_utf16());
        fo.set("line"sv, f.location.line);
        fo.set("column"sv, f.location.column);
        fo.set("source"sv, serialize_script_description(f.location.source));
        fo.set("thisValue"sv, serialize_debugger_value(f.this_value));
        frames.must_append(move(fo));
    }
    payload.set("frames"sv, move(frames));
    send_event("debuggerPaused"sv, move(payload));
}

void InspectorSession::did_resume_debugger()
{
    send_event("debuggerResumed"sv, JsonObject {});
}

void InspectorSession::did_receive_js_console_result(DebuggerValue const& result, bool thrown)
{
    auto serialized = serialize_debugger_value(result);
    // The exception itself reaches the console as an error message; this only tells the frontend the input finished.
    if (thrown)
        serialized.set("thrown"sv, true);
    if (auto req = exchange(m_pending_console_eval_request_id, {}); req.has_value())
        send_reply(*req, true, serialized);
    send_event("consoleResult"sv, move(serialized));
}

void InspectorSession::did_receive_console_message(ConsoleOutput const& output)
{
    JsonObject entry;
    entry.set("timestamp"sv, static_cast<i64>(output.timestamp.milliseconds_since_epoch()));

    output.output.visit(
        [&](ConsoleLog const& log) {
            entry.set("kind"sv, "log"_string);
            entry.set("level"sv, MUST(String::from_utf8(console_log_level_to_string(log.level))));
            JsonArray args;
            for (auto const& arg : log.arguments)
                args.must_append(arg);
            entry.set("arguments"sv, move(args));
            if (log.location.has_value())
                entry.set("location"sv, serialize_stack_frame(*log.location));
        },
        [&](ConsoleError const& err) {
            entry.set("kind"sv, "error"_string);
            entry.set("level"sv, "error"_string);
            entry.set("name"sv, err.name);
            entry.set("message"sv, err.message);
            entry.set("insidePromise"sv, err.inside_promise);
            JsonArray trace;
            for (auto const& frame : err.trace)
                trace.must_append(serialize_stack_frame(frame));
            entry.set("trace"sv, move(trace));
        },
        [&](ConsoleTrace const& trace) {
            entry.set("kind"sv, "trace"_string);
            entry.set("level"sv, "log"_string);
            entry.set("label"sv, trace.label);
            JsonArray frames;
            for (auto const& frame : trace.stack)
                frames.must_append(serialize_stack_frame(frame));
            entry.set("trace"sv, move(frames));
        },
        [&](ConsoleGroupEnd const&) {
            entry.set("kind"sv, "groupEnd"_string);
        });

    if (m_console_entries.size() >= max_console_entries)
        m_console_entries.remove(0);
    m_console_entries.append(entry);
    send_event("consoleMessage"sv, move(entry));
}

void InspectorSession::did_start_network_request(u64 request_id, URL::URL const& url, ByteString const& method, Vector<HTTP::Header> const& request_headers, ByteBuffer const&, Optional<String> const& initiator_type, String const& referrer_policy, bool is_navigation_request, Web::Fetch::Infrastructure::RequestPriority)
{
    NetworkEntry entry;
    entry.request_id = request_id;
    entry.url = url.serialize();
    entry.method = MUST(String::from_byte_string(method));
    entry.initiator_type = initiator_type.value_or("other"_string);
    entry.referrer_policy = referrer_policy;
    entry.is_navigation_request = is_navigation_request;
    for (auto const& h : request_headers) {
        JsonObject ho;
        ho.set("name"sv, MUST(String::from_byte_string(h.name)));
        ho.set("value"sv, MUST(String::from_byte_string(h.value)));
        entry.request_headers.must_append(move(ho));
    }

    if (m_network_entries.size() >= max_network_entries)
        m_network_entries.remove(0);
    m_network_entries.append(move(entry));

    JsonObject payload;
    payload.set("requestId"sv, request_id);
    payload.set("url"sv, url.serialize());
    payload.set("method"sv, MUST(String::from_byte_string(method)));
    payload.set("initiatorType"sv, initiator_type.value_or("other"_string));
    send_event("networkRequestStarted"sv, move(payload));
}

void InspectorSession::did_receive_network_response_headers(u64 request_id, u32 status_code, Optional<String> const& reason_phrase, Vector<HTTP::Header> const& response_headers, Requests::CacheState cache_state)
{
    JsonArray headers_arr;
    for (auto const& h : response_headers) {
        JsonObject ho;
        ho.set("name"sv, MUST(String::from_byte_string(h.name)));
        ho.set("value"sv, MUST(String::from_byte_string(h.value)));
        headers_arr.must_append(move(ho));
    }
    auto cache_str = MUST(String::from_utf8(cache_state_to_string(cache_state)));

    for (auto& entry : m_network_entries) {
        if (entry.request_id == request_id) {
            entry.status_code = status_code;
            entry.reason_phrase = reason_phrase.value_or(String {});
            entry.cache_state = cache_str;
            entry.response_headers = headers_arr;
            break;
        }
    }

    JsonObject payload;
    payload.set("requestId"sv, request_id);
    payload.set("statusCode"sv, status_code);
    payload.set("reasonPhrase"sv, reason_phrase.value_or(String {}));
    payload.set("cacheState"sv, cache_str);
    payload.set("responseHeaders"sv, move(headers_arr));
    send_event("networkResponseHeaders"sv, move(payload));
}

void InspectorSession::did_receive_network_response_body(u64 request_id, ByteBuffer const& data)
{
    static constexpr size_t max_preview_bytes = 32768;
    auto slice = data.bytes().trim(max_preview_bytes);
    auto preview = String::from_utf8_with_replacement_character(StringView { slice });

    for (auto& entry : m_network_entries) {
        if (entry.request_id == request_id) {
            if (entry.response_body_preview.bytes().size() < max_preview_bytes) {
                StringBuilder sb;
                sb.append(entry.response_body_preview);
                sb.append(preview);
                entry.response_body_preview = MUST(sb.to_string());
            }
            break;
        }
    }
}

void InspectorSession::did_finish_network_request(u64 request_id, u64 body_size, Requests::RequestTimingInfo const& timing_info, Optional<Requests::NetworkError> const& network_error)
{
    double duration_ms = static_cast<double>(timing_info.response_end_microseconds - timing_info.request_start_microseconds) / 1000.0;
    if (duration_ms < 0)
        duration_ms = 0;

    Optional<String> error_str;
    if (network_error.has_value())
        error_str = MUST(String::formatted("{}", Requests::network_error_to_string(*network_error)));

    for (auto& entry : m_network_entries) {
        if (entry.request_id == request_id) {
            entry.body_size = body_size;
            entry.duration_ms = duration_ms;
            entry.finished = true;
            entry.error = error_str;
            break;
        }
    }

    JsonObject payload;
    payload.set("requestId"sv, request_id);
    payload.set("bodySize"sv, body_size);
    payload.set("durationMs"sv, duration_ms);
    if (error_str.has_value())
        payload.set("error"sv, *error_str);
    send_event("networkRequestFinished"sv, move(payload));
}

}
