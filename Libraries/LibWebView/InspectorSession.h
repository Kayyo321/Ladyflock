/*
 * Copyright (c) 2026, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibHTTP/Header.h>
#include <LibRequests/CacheState.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <LibWebCommon/CSS/PseudoElement.h>
#include <LibWebCommon/CSS/StyleSheetIdentifier.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/RequestPriority.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/Scripting/ScriptRegistryTypes.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

struct PendingInspectTarget {
    RefPtr<WebContentPage> page;
    Optional<Web::HTML::CrossProcessId> document_id;
    Web::UniqueNodeID node_id { 0 };
    u64 menu_generation { 0 };
};

class WEBVIEW_API InspectorSession final
    : public RefCounted<InspectorSession>
    , public Weakable<InspectorSession> {
public:
    struct UICallbacks {
        Function<void(u64 target_view_id, String panel, Optional<Web::UniqueNodeID> node_id, u64 menu_generation)> on_open_requested;
        Function<void(u64 target_view_id)> on_close_requested;
        Function<void(u64 target_view_id, String dock_mode)> on_dock_mode_changed;
        Function<void(u64 target_view_id, bool picker_active)> on_picker_state_changed;
        Function<void(String text)> on_copy_text;
        Function<void(URL::URL url, u64 source_view_id, IsPrivate is_private)> on_open_url_in_new_tab;
    };

    static void set_ui_callbacks(UICallbacks);
    static UICallbacks const& ui_callbacks();

    static NonnullRefPtr<InspectorSession> ensure_for_target_view(ViewImplementation& target_view);
    static RefPtr<InspectorSession> find_for_target_view(u64 target_view_id);
    static RefPtr<InspectorSession> find_for_frontend_view(u64 frontend_view_id);
    static RefPtr<InspectorSession> find_by_attachment_id(u64 attachment_id);

    static RefPtr<InspectorSession> authorize_frontend_view(ViewImplementation& target_view, u64 frontend_view_id);
    static RefPtr<InspectorSession> bind_web_ui(u64 frontend_view_id, InspectorUI& web_ui);

    ~InspectorSession();

    u64 attachment_id() const { return m_attachment_id; }
    u64 target_view_id() const { return m_target_view_id; }
    Optional<u64> frontend_view_id() const { return m_frontend_view_id; }
    IsPrivate is_private() const { return m_is_private; }
    u64 document_generation() const { return m_document_generation; }
    String const& dock_mode() const { return m_dock_mode; }
    bool is_bound_to(InspectorUI const& ui) const { return m_web_ui.ptr() == &ui; }
    bool is_attached() const { return !m_web_ui.is_null(); }
    bool has_current_frontend() const;
    bool is_picker_active() const { return m_picker_active; }

    void set_dock_mode(String dock_mode);
    void set_initial_panel(String panel);
    void queue_inspect_target(PendingInspectTarget);
    void unbind_web_ui(InspectorUI&);
    void detach_frontend();
    void target_did_close();
    void target_did_navigate(URL::URL const& url);
    void target_did_crash();
    void target_did_recover();

    void toggle_node_picker();
    void set_node_picker_active(bool active);
    void select_panel(String const& panel);
    void select_node(Web::UniqueNodeID node_id, Optional<Web::HTML::CrossProcessId> document_id = {}, u64 menu_generation = 0);

    // Typed command dispatcher invoked only by a bound InspectorUI.
    void handle_command(u64 request_id, StringView command, JsonObject const& params);

    // Observation hooks called by ViewImplementation / WebContentPage alongside Firefox RDP.
    void did_receive_dom_node_properties(u64 request_id, DOMNodeProperties const& properties);
    void did_receive_dom_mutation(Mutation const& mutation);
    void did_finish_editing_dom_node(Optional<Web::UniqueNodeID> node_id);
    void did_receive_dom_node_html(String const& html);
    void did_receive_grid_layouts(JsonArray const& layouts);
    void did_receive_current_grid(Optional<JsonObject> const& grid);
    void did_receive_current_flexbox(Optional<JsonObject> const& flexbox);
    void did_receive_accessibility_tree(JsonObject const& tree);
    void did_receive_style_sheet_list(Vector<Web::CSS::StyleSheetIdentifier> const& stylesheets);
    void did_receive_style_sheet_source(Web::CSS::StyleSheetIdentifier const& identifier, URL::URL const& base_url, Utf16String const& source);
    void did_add_devtools_source(Web::HTML::ScriptRegistryDescription const& source);
    void did_pause_debugger(DebuggerPause const& pause);
    void did_resume_debugger();
    void did_receive_js_console_result(DebuggerValue const& result, bool thrown);
    void did_receive_console_message(ConsoleOutput const& output);
    void did_start_network_request(u64 request_id, URL::URL const& url, ByteString const& method, Vector<HTTP::Header> const& request_headers, ByteBuffer const& request_body, Optional<String> const& initiator_type, String const& referrer_policy, bool is_navigation_request, Web::Fetch::Infrastructure::RequestPriority priority);
    void did_receive_network_response_headers(u64 request_id, u32 status_code, Optional<String> const& reason_phrase, Vector<HTTP::Header> const& response_headers, Requests::CacheState cache_state);
    void did_receive_network_response_body(u64 request_id, ByteBuffer const& data);
    void did_finish_network_request(u64 request_id, u64 body_size, Requests::RequestTimingInfo const& timing_info, Optional<Requests::NetworkError> const& network_error);

    // Extended inspector event forwarding (cascade edits, DOM search, listeners, breakpoints, attribution, etc.).
    void send_event(StringView name, JsonObject payload);

    // Cosmetic filter save/undo contract (also directly testable).
    JsonObject save_cosmetic_filter_rule(String const& host, String const& selector);
    JsonObject undo_cosmetic_filter_rule(u64 filter_journal_id);

    // Flight Recorder / Changes journal.
    struct EditJournalEntry {
        u64 id { 0 };
        u64 request_id { 0 };
        u64 document_generation { 0 };
        String kind;
        String target_label;
        String group_key;
        String before_text;
        String after_text;
        u64 base_revision { 0 };
        u64 resulting_revision { 0 };
        bool is_writable_source { false };
        bool undone { false };
        JsonObject undo_payload;
    };
    Vector<EditJournalEntry> const& edit_journal() const { return m_edit_journal; }

private:
    explicit InspectorSession(ViewImplementation& target_view);

    void attach_to_target_client();
    void detach_from_target_client();
    void send_reply(u64 request_id, bool ok, JsonValue result, Optional<String> error = {});
    void send_session_state();
    void record_selection_history(Web::UniqueNodeID node_id);
    JsonObject serialize_edit_journal() const;
    JsonObject serialize_network_log() const;
    JsonObject serialize_console_log() const;
    JsonObject serialize_dom_breakpoints() const;
    void record_edit_entry(EditJournalEntry entry);
    // A load's own requests, its document's first of all, start before its document replaces the old one.
    enum class KeepRequestsOfCurrentLoad : bool {
        No,
        Yes,
    };
    void reset_for_new_document(KeepRequestsOfCurrentLoad = KeepRequestsOfCurrentLoad::No);
    void restore_emulation_overrides();
    void restore_dom_breakpoints(URL::URL const&);

    ViewImplementation* target_view() const { return m_target_view.ptr(); }

    u64 m_attachment_id { 0 };
    u64 m_target_view_id { 0 };
    WeakPtr<ViewImplementation> m_target_view;
    Optional<u64> m_frontend_view_id;
    WeakPtr<InspectorUI> m_web_ui;
    IsPrivate m_is_private { IsPrivate::No };

    bool m_connected_to_target { false };
    bool m_listening_for_mutations { false };
    bool m_picker_active { false };
    bool m_debugger_attached { false };
    bool m_target_crashed { false };
    bool m_preserve_console_log { false };
    bool m_preserve_network_log { false };

    u64 m_document_generation { 1 };
    u64 m_dom_sequence { 0 };
    String m_dock_mode { "right"_string };
    String m_initial_panel { "elements"_string };
    Optional<PendingInspectTarget> m_pending_inspect_target;
    Optional<Web::UniqueNodeID> m_selected_node_id;
    Vector<Web::UniqueNodeID> m_selection_history;

    // Flight Recorder bounded journal + idempotency cache.
    static constexpr size_t max_edit_journal_entries = 500;
    Vector<EditJournalEntry> m_edit_journal;
    bool m_edit_journal_truncated { false };
    u64 m_next_edit_journal_id { 1 };
    HashMap<u64, JsonObject> m_completed_mutation_replies;

    // Cosmetic filter save history for surgical undo.
    struct SavedCosmeticFilter {
        u64 id { 0 };
        String rule;
        bool undone { false };
    };
    Vector<SavedCosmeticFilter> m_saved_cosmetic_filters;
    u64 m_next_cosmetic_filter_id { 1 };

    // Bounded network log for Network panel & HAR export.
    struct NetworkEntry {
        u64 request_id { 0 };
        String url;
        String method;
        u32 status_code { 0 };
        String reason_phrase;
        String initiator_type;
        String referrer_policy;
        String cache_state;
        bool is_navigation_request { false };
        u64 body_size { 0 };
        double duration_ms { 0 };
        Optional<String> error;
        JsonArray request_headers;
        JsonArray response_headers;
        String response_body_preview;
        bool finished { false };
        u64 sequence_number { 0 };
    };
    static constexpr size_t max_network_entries = 500;
    Vector<NetworkEntry> m_network_entries;
    u64 m_next_network_entry_sequence_number { 1 };
    u64 m_first_network_entry_of_current_load { 1 };

    // Bounded console buffer.
    static constexpr size_t max_console_entries = 1000;
    Vector<JsonObject> m_console_entries;

    // DOM breakpoints persisted across reloads (in memory for private sessions).
    struct DOMBreakpointDescriptor {
        u64 id { 0 };
        String url;
        String node_path;
        Web::UniqueNodeID node_id { 0 };
        String type; // "subtree-modified" | "attribute-modified" | "node-removed"
        bool enabled { true };
        bool resolved { true };
    };
    Vector<DOMBreakpointDescriptor> m_dom_breakpoints;
    u64 m_next_dom_breakpoint_id { 1 };

    // Active emulation overrides so detach restores clean system/profile state.
    JsonObject m_active_rendering_overrides;
    JsonObject m_active_device_overrides;
    JsonObject m_active_sensor_overrides;
    Optional<u64> m_navigation_listener_id;
    HashMap<u64, u64> m_pending_storage_requests;
    u64 m_next_internal_storage_request_id { 900000 };
    struct NodePropertiesRequest {
        Web::UniqueNodeID node_id;
        Optional<String> pseudo_element;
    };
    HashMap<u64, NodePropertiesRequest> m_node_properties_requests;
    Optional<u64> m_pending_html_request_id;
    MonotonicTime m_pending_html_request_started { MonotonicTime::now_coarse() };
    Optional<u64> m_pending_edit_request_id;
    Optional<EditJournalEntry> m_pending_edit_journal_entry;
    Optional<u64> m_pending_a11y_request_id;
    MonotonicTime m_pending_a11y_request_started { MonotonicTime::now_coarse() };
    Optional<u64> m_pending_stylesheets_request_id;
    Optional<u64> m_pending_stylesheet_source_request_id;
    MonotonicTime m_pending_stylesheet_source_request_started { MonotonicTime::now_coarse() };
    Optional<u64> m_pending_grid_layouts_request_id;
    Optional<u64> m_pending_current_grid_request_id;
    Optional<u64> m_pending_current_flexbox_request_id;
    Optional<u64> m_pending_console_eval_request_id;
};

}
