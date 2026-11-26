/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebView/Extensions/WebRequestCoordinator.h>

namespace WebView::Extensions {

void WebRequestCoordinator::add_listener(
    String extension_id,
    u64 listener_id,
    u64 grant_generation,
    Vector<String> url_patterns,
    Vector<Web::Extensions::WebRequestResourceType> types,
    bool blocking)
{
    m_entries.remove_all_matching([&](auto const& existing) {
        return existing.extension_id == extension_id && existing.listener_id == listener_id;
    });

    m_entries.append(Web::Extensions::WebRequestWatchEntry {
        .extension_id = move(extension_id),
        .listener_id = listener_id,
        .grant_generation = grant_generation,
        .url_patterns = move(url_patterns),
        .types = move(types),
        .blocking = blocking,
    });
    rebuild_snapshot();
}

void WebRequestCoordinator::remove_listener(StringView extension_id, u64 listener_id)
{
    bool removed = false;
    m_entries.remove_all_matching([&](auto const& entry) {
        if (entry.extension_id == extension_id && entry.listener_id == listener_id) {
            removed = true;
            return true;
        }
        return false;
    });
    if (removed)
        rebuild_snapshot();
}

void WebRequestCoordinator::remove_all_for_extension(StringView extension_id)
{
    bool removed = false;
    m_entries.remove_all_matching([&](auto const& entry) {
        if (entry.extension_id == extension_id) {
            removed = true;
            return true;
        }
        return false;
    });
    if (removed)
        rebuild_snapshot();
}

void WebRequestCoordinator::rebuild_snapshot()
{
    m_snapshot.revision = m_revision++;
    m_snapshot.entries = m_entries;
    if (m_on_snapshot_changed)
        m_on_snapshot_changed(m_snapshot);
}

void WebRequestCoordinator::set_has_declarative_rules(bool enabled)
{
    if (m_snapshot.has_declarative_rules == enabled)
        return;
    m_snapshot.has_declarative_rules = enabled;
    rebuild_snapshot();
}

static bool entry_matches(Web::Extensions::WebRequestWatchEntry const& entry, URL::URL const& url, Web::Extensions::WebRequestResourceType type)
{
    if (!entry.types.is_empty() && !entry.types.contains_slow(type))
        return false;
    for (auto const& raw_pattern : entry.url_patterns) {
        auto parsed = Web::Extensions::MatchPattern::parse(raw_pattern);
        if (!parsed.is_error() && parsed.value().matches(url))
            return true;
    }
    return false;
}

bool WebRequestCoordinator::has_matching_listener(URL::URL const& url, Web::Extensions::WebRequestResourceType type) const
{
    return m_snapshot.matches(url, type);
}

namespace {

struct EvaluationState : public RefCounted<EvaluationState> {
    Vector<String> extension_ids;
    size_t index { 0 };
    String args_json;
    Function<void(Web::Extensions::WebRequestDecision)> callback;
    WebRequestCoordinator::DispatchEventCallback const* dispatch_event { nullptr };

    void step()
    {
        if (index >= extension_ids.size()) {
            callback(Web::Extensions::WebRequestDecision { .cancel = false });
            return;
        }

        auto ext_id = extension_ids[index++];
        (*dispatch_event)(
            ext_id,
            "webRequest"sv,
            "onBeforeRequest"sv,
            args_json,
            [self = NonnullRefPtr { *this }](bool has_value, String const& result_json) mutable {
                if (has_value) {
                    auto parsed = JsonValue::from_string(result_json);
                    if (!parsed.is_error() && parsed.value().is_object()) {
                        if (parsed.value().as_object().get_bool("cancel"sv) == true) {
                            self->callback(Web::Extensions::WebRequestDecision { .cancel = true });
                            return;
                        }
                    }
                }
                self->step();
            });
    }
};

}

void WebRequestCoordinator::evaluate_before_request(
    Web::Extensions::WebRequestBeforeRequestDetails const& details,
    Function<void(Web::Extensions::WebRequestDecision)> callback) const
{
    if (!m_dispatch_event) {
        callback({ .cancel = false });
        return;
    }

    Vector<String> matching_blocking_extensions;
    Vector<String> matching_nonblocking_extensions;
    Optional<URL::URL> initiator;
    auto initiator_url = details.document_url.has_value() ? details.document_url : details.initiator_origin;
    if (initiator_url.has_value())
        initiator = URL::Parser::basic_parse(*initiator_url);

    auto request_origin = details.initiator_origin.has_value() ? URL::Parser::basic_parse(*details.initiator_origin) : initiator;
    if (m_snapshot.has_declarative_rules && m_declarative_block && m_declarative_block(details, request_origin)) {
        callback({ .cancel = true });
        return;
    }

    for (auto const& entry : m_entries) {
        if (!entry_matches(entry, details.url, details.type))
            continue;
        if (m_has_permission && !m_has_permission(entry.extension_id, details.url))
            continue;
        if (m_has_permission && details.type != Web::Extensions::WebRequestResourceType::MainFrame && initiator_url.has_value()
            && (!initiator.has_value() || !m_has_permission(entry.extension_id, *initiator)))
            continue;

        if (entry.blocking) {
            if (!matching_blocking_extensions.contains_slow(entry.extension_id))
                matching_blocking_extensions.append(entry.extension_id);
        } else {
            if (!matching_nonblocking_extensions.contains_slow(entry.extension_id))
                matching_nonblocking_extensions.append(entry.extension_id);
        }
    }

    if (matching_blocking_extensions.is_empty() && matching_nonblocking_extensions.is_empty()) {
        callback({ .cancel = false });
        return;
    }

    JsonObject details_obj;
    details_obj.set("requestId"sv, String::number(details.request_id));
    details_obj.set("url"sv, details.url.serialize());
    details_obj.set("method"sv, details.method);
    details_obj.set("type"sv, MUST(String::from_utf8(Web::Extensions::to_string(details.type))));
    details_obj.set("tabId"sv, static_cast<i64>(details.tab_id));
    details_obj.set("frameId"sv, static_cast<i64>(details.frame_id));
    details_obj.set("parentFrameId"sv, static_cast<i64>(details.parent_frame_id));
    if (details.initiator_origin.has_value())
        details_obj.set("originUrl"sv, *details.initiator_origin);
    if (details.document_url.has_value())
        details_obj.set("documentUrl"sv, *details.document_url);

    JsonArray args_array;
    args_array.must_append(move(details_obj));
    auto args_json = args_array.serialized();

    // Fire non-blocking observers asynchronously (observe-only).
    for (auto const& ext_id : matching_nonblocking_extensions) {
        if (!matching_blocking_extensions.contains_slow(ext_id))
            m_dispatch_event(ext_id, "webRequest"sv, "onBeforeRequest"sv, args_json, {});
    }

    if (matching_blocking_extensions.is_empty()) {
        callback({ .cancel = false });
        return;
    }

    auto state = make_ref_counted<EvaluationState>();
    state->extension_ids = move(matching_blocking_extensions);
    state->args_json = move(args_json);
    state->callback = move(callback);
    state->dispatch_event = &m_dispatch_event;
    state->step();
}

}
