/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>

namespace Web::Extensions {

// Attached to each Web::DOM::Document to manage per-extension isolated realms,
// content-script timings (document_start, document_end, document_idle), CSS injection, and revocation.
class WEB_API ExtensionContentScriptHost final : public GC::Cell {
    GC_CELL(ExtensionContentScriptHost, GC::Cell);
    GC_DECLARE_ALLOCATOR(ExtensionContentScriptHost);

public:
    static GC::Ref<ExtensionContentScriptHost> create(DOM::Document&);

    // The manifest and messages an extension's content scripts read, which the browser sends to each renderer once
    // before that renderer runs any of the extension's content scripts.
    struct ExtensionMetadata {
        String manifest_json;
        String locale_messages_json;
    };
    static void set_extension_metadata(String extension_id, String manifest_json, String locale_messages_json);
    static Optional<ExtensionMetadata const&> extension_metadata(StringView extension_id);
    virtual ~ExtensionContentScriptHost() override;

    void attach_scripts(Vector<AttachedContentScript>);
    void run_scripts_for(RunAt);
    void inject_attached_script(AttachedContentScript const&);

    void initialize_extension_page_context(String extension_id, String extension_uuid, String manifest_json, String locale_messages_json, bool is_background_page);
    // Whether the document is an extension's own page whose manifest asks for the permission. The browser checks
    // again before it acts on anything the permission allows.
    bool is_extension_page_with_permission(StringView permission) const;

    // Dynamic execution (tabs.executeScript / scripting.executeScript) in the isolated realm.
    void execute_script_dynamically(u64 call_id, String extension_id, String extension_uuid, u64 grant_generation, Vector<String> sources, Optional<String> func_source, Optional<String> args_json);

    void handle_api_response(u64 call_id, bool success, StringView result_or_error_json);
    void dispatch_event(StringView extension_id, StringView namespace_name, StringView event_name, StringView args_json, Optional<u64> reply_id);

    void revoke_extension(StringView extension_id, u64 new_grant_generation = 0);
    void detach_all();

private:
    explicit ExtensionContentScriptHost(DOM::Document&);

    virtual void visit_edges(Cell::Visitor&) override;

    void run_deferred_main_world_scripts();
    GC::Ref<Bindings::ExtensionRealmContext> ensure_content_script_realm(String const& extension_id, String const& extension_uuid, u64 grant_generation);

    GC::Ref<DOM::Document> m_document;
    Vector<AttachedContentScript> m_attached_scripts;
    bool m_ran_document_start { false };
    bool m_ran_document_end { false };
    bool m_ran_document_idle { false };
    Vector<AttachedContentScript> m_deferred_main_world_scripts;

    HashMap<String, GC::Ref<Bindings::ExtensionRealmContext>> m_extension_realms;
    GC::Ptr<Bindings::ExtensionRealmContext> m_extension_page_context;
};

}
