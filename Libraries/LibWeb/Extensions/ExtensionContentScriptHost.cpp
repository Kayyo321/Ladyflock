/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibWeb/Bindings/ExtensionWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ExtensionContentScriptHost.h>
#include <LibWeb/Page/Page.h>

namespace Web::Extensions {

GC_DEFINE_ALLOCATOR(ExtensionContentScriptHost);

GC::Ref<ExtensionContentScriptHost> ExtensionContentScriptHost::create(DOM::Document& document)
{
    return document.heap().allocate<ExtensionContentScriptHost>(document);
}

ExtensionContentScriptHost::ExtensionContentScriptHost(DOM::Document& document)
    : m_document(document)
{
}

ExtensionContentScriptHost::~ExtensionContentScriptHost() = default;

void ExtensionContentScriptHost::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    for (auto& [id, context] : m_extension_realms)
        visitor.visit(context);
    visitor.visit(m_extension_page_context);
}

GC::Ref<Bindings::ExtensionRealmContext> ExtensionContentScriptHost::ensure_content_script_realm(
    String const& extension_id,
    String const& extension_uuid,
    u64 grant_generation)
{
    if (auto existing = m_extension_realms.get(extension_id); existing.has_value() && !existing.value()->is_revoked()) {
        existing.value()->set_grant_generation(grant_generation);
        return *existing.value();
    }

    auto realm_ctx = Bindings::ExtensionRealmContext::create_for_content_script(
        *m_document,
        extension_id,
        extension_uuid,
        grant_generation);
    m_extension_realms.set(extension_id, realm_ctx);
    return realm_ctx;
}

void ExtensionContentScriptHost::initialize_extension_page_context(String extension_id, String extension_uuid, String manifest_json)
{
    if (m_extension_page_context)
        return;
    m_extension_page_context = Bindings::ExtensionRealmContext::create_for_extension_page(
        *m_document,
        move(extension_id),
        move(extension_uuid),
        move(manifest_json));
}

void ExtensionContentScriptHost::attach_scripts(Vector<AttachedContentScript> scripts)
{
    for (auto& script : scripts) {
        m_attached_scripts.append(script);

        // If the document has already passed the requested timing phase, inject CSS immediately and run JS immediately.
        bool already_passed = false;
        switch (script.run_at) {
        case RunAt::DocumentStart:
            already_passed = m_ran_document_start;
            break;
        case RunAt::DocumentEnd:
            already_passed = m_ran_document_end;
            break;
        case RunAt::DocumentIdle:
            already_passed = m_ran_document_idle;
            break;
        }

        if (already_passed) {
            for (auto const& css : script.css_files)
                m_document->add_extension_style_sheet(script.extension_id, css.path, css.content);

            if (!script.js_files.is_empty()) {
                auto realm_ctx = ensure_content_script_realm(script.extension_id, script.extension_uuid, script.grant_generation);
                for (auto const& js : script.js_files)
                    realm_ctx->evaluate_script(js.content, js.path);
            }
        }
    }
}

void ExtensionContentScriptHost::run_scripts_for(RunAt run_at)
{
    switch (run_at) {
    case RunAt::DocumentStart:
        if (m_ran_document_start)
            return;
        m_ran_document_start = true;
        break;
    case RunAt::DocumentEnd:
        if (m_ran_document_end)
            return;
        m_ran_document_end = true;
        break;
    case RunAt::DocumentIdle:
        if (m_ran_document_idle)
            return;
        m_ran_document_idle = true;
        break;
    }

    for (auto const& script : m_attached_scripts) {
        if (script.run_at != run_at)
            continue;

        for (auto const& css : script.css_files)
            m_document->add_extension_style_sheet(script.extension_id, css.path, css.content);

        if (!script.js_files.is_empty()) {
            auto realm_ctx = ensure_content_script_realm(script.extension_id, script.extension_uuid, script.grant_generation);
            for (auto const& js : script.js_files)
                realm_ctx->evaluate_script(js.content, js.path);
        }
    }
}

void ExtensionContentScriptHost::execute_script_dynamically(
    u64 call_id,
    String extension_id,
    String extension_uuid,
    u64 grant_generation,
    Vector<String> sources,
    Optional<String> func_source,
    Optional<String> args_json)
{
    auto realm_ctx = ensure_content_script_realm(extension_id, extension_uuid, grant_generation);

    Optional<String> last_outcome;
    if (func_source.has_value()) {
        last_outcome = realm_ctx->evaluate_script(""sv, "extension-func.js"sv, true, move(func_source), move(args_json));
    } else {
        for (size_t i = 0; i < sources.size(); ++i) {
            bool is_last = (i + 1 == sources.size());
            auto outcome = realm_ctx->evaluate_script(sources[i], "extension-script.js"sv, is_last);
            if (is_last)
                last_outcome = move(outcome);
        }
    }

    auto outcome_str = last_outcome.value_or(R"({"result":null})"_string);
    auto parsed = JsonValue::from_string(outcome_str);
    bool success = !parsed.is_error() && parsed.value().is_object() && !parsed.value().as_object().has("error"sv);
    if (!success && !parsed.is_error() && parsed.value().is_object()) {
        auto err = parsed.value().as_object().get_string("error"sv).value_or("Execution failed"_string);
        m_document->page().client().page_did_finish_extension_script_execution(call_id, false, err);
        return;
    }

    String result_json = "null"_string;
    if (!parsed.is_error() && parsed.value().is_object()) {
        if (auto res = parsed.value().as_object().get("result"sv); res.has_value())
            result_json = res->serialized();
    }
    m_document->page().client().page_did_finish_extension_script_execution(call_id, true, result_json);
}

void ExtensionContentScriptHost::handle_api_response(u64 call_id, bool success, StringView result_or_error_json)
{
    if (m_extension_page_context)
        m_extension_page_context->handle_api_response(call_id, success, result_or_error_json);
    for (auto& [id, ctx] : m_extension_realms)
        ctx->handle_api_response(call_id, success, result_or_error_json);
}

void ExtensionContentScriptHost::dispatch_event(StringView extension_id, StringView namespace_name, StringView event_name, StringView args_json, Optional<u64> reply_id)
{
    if (m_extension_page_context && m_extension_page_context->extension_id() == extension_id) {
        m_extension_page_context->dispatch_event(namespace_name, event_name, args_json, reply_id);
        return;
    }
    if (auto ctx = m_extension_realms.get(extension_id); ctx.has_value()) {
        ctx.value()->dispatch_event(namespace_name, event_name, args_json, reply_id);
        return;
    }
    if (reply_id.has_value())
        m_document->page().client().page_did_reply_extension_event(*reply_id, false, "null"_string);
}

void ExtensionContentScriptHost::revoke_extension(StringView extension_id, u64)
{
    m_document->remove_all_extension_style_sheets_for_extension(extension_id);
    m_attached_scripts.remove_all_matching([&](AttachedContentScript const& script) {
        return script.extension_id == extension_id;
    });

    if (auto ctx = m_extension_realms.take(extension_id); ctx.has_value())
        ctx.value()->revoke_and_detach();

    if (m_extension_page_context && m_extension_page_context->extension_id() == extension_id) {
        m_extension_page_context->revoke_and_detach();
        m_extension_page_context = nullptr;
    }
}

void ExtensionContentScriptHost::detach_all()
{
    m_attached_scripts.clear();
    auto realms = move(m_extension_realms);
    m_extension_realms.clear();
    for (auto& [id, ctx] : realms)
        ctx->revoke_and_detach();
    if (m_extension_page_context) {
        m_extension_page_context->revoke_and_detach();
        m_extension_page_context = nullptr;
    }
}

}
