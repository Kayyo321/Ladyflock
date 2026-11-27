/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebCommon/Extensions/NavigationURLFilter.h>
#include <LibWebCommon/HTML/CrossProcessId.h>

namespace Web::Bindings {

// Per-(extension, document) isolated realm & API bridge for content scripts, or per-document API bridge for moz-extension:// pages.
class WEB_API ExtensionRealmContext final : public GC::Cell {
    GC_CELL(ExtensionRealmContext, GC::Cell);
    GC_DECLARE_ALLOCATOR(ExtensionRealmContext);

public:
    static GC::Ref<ExtensionRealmContext> create_for_content_script(
        DOM::Document&,
        String extension_id,
        String extension_uuid,
        u64 grant_generation);

    static GC::Ref<ExtensionRealmContext> create_for_extension_page(
        DOM::Document&,
        String extension_id,
        String extension_uuid,
        String manifest_json,
        String locale_messages_json = {});

    virtual ~ExtensionRealmContext() override;

    String const& extension_id() const { return m_extension_id; }
    String const& extension_uuid() const { return m_extension_uuid; }
    u64 grant_generation() const { return m_grant_generation; }
    void set_grant_generation(u64 generation) { m_grant_generation = generation; }
    bool is_revoked() const { return m_revoked; }
    bool has_manifest_permission(StringView permission) const;

    JS::Realm& realm() const { return *m_realm; }
    WrapperWorld& wrapper_world() const { return *m_wrapper_world; }
    Extensions::ContextType context_type() const { return m_context_type; }

    // Evaluates a script in this extension realm (either content script or dynamic injection).
    // Returns JSON-serialized result or error string if requested.
    Optional<String> evaluate_script(StringView source, StringView filename, bool serialize_result = false, Optional<String> func_source = {}, Optional<String> args_json = {});

    void handle_api_response(u64 call_id, bool success, StringView result_or_error_json);
    void dispatch_event(StringView namespace_name, StringView event_name, StringView args_json, Optional<u64> reply_id);

    void revoke_and_detach();

private:
    ExtensionRealmContext(
        DOM::Document&,
        String extension_id,
        String extension_uuid,
        u64 grant_generation,
        Extensions::ContextType,
        String manifest_json,
        GC::Ptr<WrapperWorld>,
        NonnullOwnPtr<JS::ExecutionContext>,
        GC::Ref<JS::Realm>,
        bool owns_realm_and_world);

    ExtensionRealmContext(
        DOM::Document&,
        String extension_id,
        String extension_uuid,
        Extensions::ContextType,
        String manifest_json,
        GC::Ref<WrapperWorld>,
        GC::Ref<JS::Realm>);

    virtual void visit_edges(Cell::Visitor&) override;

    void install_extension_namespaces(bool full_extension_page_apis);
    JS::Value create_i18n_object();
    JS::Value create_runtime_port(String const& id, String const& name, JS::Value sender);
    JS::ThrowCompletionOr<JS::Value> connect_runtime_port(JS::VM&);
    JS::ThrowCompletionOr<JS::Value> connect_native_port(JS::VM&);
    void notify_runtime_port(StringView method, String const& id, Optional<String> payload = {});
    void forget_runtime_port(String const& id);
    JS::Value create_namespace_object(StringView ns_name, ReadonlySpan<StringView> methods, bool is_chrome);
    JS::Value create_event_object(StringView ns_name, StringView event_name);
    JS::ThrowCompletionOr<JS::Value> invoke_api_call(JS::VM&, StringView ns_name, StringView method_name, bool is_chrome);

    // A method the renderer answers itself, as a promise or through a chrome.* callback.
    using LocalAnswer = String (*)(ExtensionRealmContext&);
    JS::ThrowCompletionOr<JS::Value> answer_locally(JS::VM&, bool is_chrome, StringView result_json);
    void define_local_method(JS::Object&, StringView name, bool is_chrome, LocalAnswer);

    GC::Ref<DOM::Document> m_document;
    String m_extension_id;
    String m_extension_uuid;
    u64 m_grant_generation { 0 };
    Extensions::ContextType m_context_type { Extensions::ContextType::ContentScript };
    String m_manifest_json;
    JsonObject m_locale_messages;
    bool m_owns_realm_and_world { false };
    bool m_revoked { false };

    GC::Ptr<WrapperWorld> m_wrapper_world;
    OwnPtr<JS::ExecutionContext> m_execution_context;
    GC::Ref<JS::Realm> m_realm;

    struct PendingCall {
        GC::Ref<WebIDL::Promise> promise;
        GC::Ptr<JS::FunctionObject> callback;
        bool is_chrome { false };
        bool clone_response { false };
        bool creates_menu { false };
        bool registers_legacy_content_script { false };
    };
    HashMap<u64, PendingCall> m_pending_calls;
    String m_last_error;

    struct EventListener {
        GC::Ref<JS::FunctionObject> callback;
        u64 listener_id { 0 };
        bool blocking { false };
        bool include_headers { false };
        RefPtr<Extensions::NavigationURLFilter> navigation_filter {};
        Vector<Extensions::MatchPattern> urls;
        Vector<Extensions::WebRequestResourceType> types;
    };
    HashMap<String, Vector<EventListener>> m_event_listeners;
    HashMap<String, GC::Ref<JS::Object>> m_runtime_ports;
};

}
