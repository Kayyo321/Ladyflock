/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
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
    bool blocking,
    Web::Extensions::WebRequestEvent event,
    bool include_headers)
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
        .event = event,
        .include_headers = include_headers,
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

using Web::Extensions::WebRequestBeforeRequestDetails;
using Web::Extensions::WebRequestDecision;
using Web::Extensions::WebRequestEvent;
using Web::Extensions::WebRequestHeaders;

static JsonArray headers_to_json(WebRequestHeaders const& headers)
{
    JsonArray result;
    for (auto const& header : headers) {
        auto lower_name = header.name.to_lowercase();
        if (lower_name.is_one_of("authorization"sv, "proxy-authorization"sv, "cookie"sv, "set-cookie"sv, "set-cookie2"sv))
            continue;
        auto name = String::from_utf8(header.name.view());
        auto value = String::from_utf8(header.value.view());
        if (name.is_error() || value.is_error())
            continue;
        JsonObject item;
        item.set("name"sv, name.release_value());
        item.set("value"sv, value.release_value());
        result.must_append(move(item));
    }
    return result;
}

static Optional<WebRequestHeaders> read_response_header_edits(JsonArray const& replacement, WebRequestHeaders const& original)
{
    if (replacement.size() > 1024)
        return {};

    // Keep the original response policies, credentials and framing headers intact.
    // These ordinary metadata headers can be changed. A CSP may only be tightened
    // by appending a separate policy.
    auto can_replace = [](StringView name) {
        return name.equals_ignoring_ascii_case("Cache-Control"sv);
    };
    auto same_header = [](HTTP::Header const& a, HTTP::Header const& b) {
        return a.name.equals_ignoring_ascii_case(b.name) && a.value == b.value;
    };

    auto result = original;
    result.remove_all_matching([&](auto const& header) { return can_replace(header.name); });
    for (auto const& item : replacement.values()) {
        if (!item.is_object())
            return {};
        auto name = item.as_object().get_string("name"sv);
        auto value = item.as_object().get_string("value"sv);
        if (!name.has_value() || !value.has_value())
            continue;
        auto header = HTTP::Header::isomorphic_encode(*name, *value);
        if (!HTTP::is_header_name(header.name) || !HTTP::is_header_value(header.value))
            return {};
        if (can_replace(header.name)
            || (header.name.equals_ignoring_ascii_case("Content-Security-Policy"sv)
                && !any_of(original, [&](auto const& entry) { return same_header(entry, header); })))
            result.append(move(header));
    }
    return result;
}

struct EvaluationState : public RefCounted<EvaluationState> {
    Vector<String> extension_ids;
    size_t index { 0 };
    String args_json;
    String args_with_headers_json;
    HashTable<String> extensions_requesting_headers;
    WebRequestEvent event { WebRequestEvent::BeforeRequest };
    URL::URL request_url;
    Optional<URL::URL> initiating_url;
    WebRequestHeaders headers;
    WebRequestDecision decision;
    Function<void(WebRequestDecision)> callback;
    WebRequestCoordinator::DispatchEventCallback const* dispatch_event { nullptr };
    WebRequestCoordinator::RedirectCheckCallback const* check_redirect { nullptr };
    WebRequestCoordinator::PermissionCheckCallback const* has_permission { nullptr };

    bool can_intercept(StringView extension_id) const
    {
        return !*has_permission || ((*has_permission)(extension_id, request_url)
            && (!initiating_url.has_value() || (*has_permission)(extension_id, *initiating_url)));
    }

    void step()
    {
        while (index < extension_ids.size() && !can_intercept(extension_ids[index]))
            ++index;
        if (index >= extension_ids.size()) {
            callback(move(decision));
            return;
        }

        auto extension_id = extension_ids[index++];
        (*dispatch_event)(
            extension_id,
            "webRequest"sv,
            to_string(event),
            extensions_requesting_headers.contains(extension_id) ? args_with_headers_json : args_json,
            [self = NonnullRefPtr { *this }, extension_id](bool has_value, String const& result_json) {
                if (!self->can_intercept(extension_id)) {
                    self->step();
                    return;
                }
                if (!has_value && self->request_url.scheme() == "moz-extension"sv) {
                    self->callback(WebRequestDecision { .cancel = true });
                    return;
                }
                if (has_value) {
                    auto parsed = JsonValue::from_string(result_json);
                    if (!parsed.is_error() && parsed.value().is_object()) {
                        auto const& result = parsed.value().as_object();
                        if (result.get_bool("cancel"sv) == true) {
                            self->callback(WebRequestDecision { .cancel = true });
                            return;
                        }
                        if (self->event == WebRequestEvent::BeforeRequest && !self->decision.redirect_url.has_value()) {
                            if (auto redirect = result.get_string("redirectUrl"sv); redirect.has_value()) {
                                auto url = URL::Parser::basic_parse(*redirect);
                                if (url.has_value() && *url != self->request_url && *self->check_redirect && (*self->check_redirect)(extension_id, *url))
                                    self->decision.redirect_url = url.release_value();
                            }
                        }
                        if (self->event == WebRequestEvent::HeadersReceived && self->extensions_requesting_headers.contains(extension_id) && !self->decision.response_headers.has_value()) {
                            if (auto replacement = result.get_array("responseHeaders"sv); replacement.has_value())
                                self->decision.response_headers = read_response_header_edits(*replacement, self->headers);
                        }
                    }
                }
                self->step();
            });
    }
};

}

void WebRequestCoordinator::evaluate_before_request(WebRequestBeforeRequestDetails const& details, Function<void(WebRequestDecision)> callback) const
{
    auto event = details.event;
    bool is_extension_url = details.url.scheme() == "moz-extension"sv;
    bool is_web_socket = details.type == Web::Extensions::WebRequestResourceType::WebSocket && event == WebRequestEvent::BeforeRequest
        && details.url.scheme().is_one_of("ws"sv, "wss"sv);
    if (!is_extension_url && !is_web_socket && !details.url.scheme().is_one_of("http"sv, "https"sv)) {
        callback({});
        return;
    }

    Vector<String> matching_blocking_extensions;
    Vector<String> matching_nonblocking_extensions;
    HashTable<String> extensions_requesting_headers;
    bool reveal_request_source = !is_extension_url;
    auto initiator_url = details.document_url.has_value() ? details.document_url : details.initiator_origin;
    auto initiator = initiator_url.has_value() ? URL::Parser::basic_parse(*initiator_url) : Optional<URL::URL> {};

    WebRequestDecision declarative_decision;
    if (!is_extension_url && m_snapshot.has_declarative_rules && m_declarative_decision
        && (event == WebRequestEvent::BeforeRequest || event == WebRequestEvent::HeadersReceived)) {
        auto request_origin = details.initiator_origin.has_value() ? URL::Parser::basic_parse(*details.initiator_origin) : initiator;
        declarative_decision = m_declarative_decision(details, request_origin);
        if (declarative_decision.cancel) {
            callback(move(declarative_decision));
            return;
        }
    }

    if (is_web_socket || !m_dispatch_event) {
        callback(move(declarative_decision));
        return;
    }

    for (auto const& entry : m_entries) {
        if (entry.event != event || !entry_matches(entry, details.url, details.type))
            continue;
        if (m_has_permission && !m_has_permission(entry.extension_id, details.url))
            continue;
        bool allowed_to_see_initiator = initiator.has_value() && (!m_has_permission || m_has_permission(entry.extension_id, *initiator));
        if (!is_extension_url && details.type != Web::Extensions::WebRequestResourceType::MainFrame
            && initiator_url.has_value() && !allowed_to_see_initiator)
            continue;
        if (is_extension_url && allowed_to_see_initiator)
            reveal_request_source = true;
        if (entry.include_headers)
            extensions_requesting_headers.set(entry.extension_id);
        auto& collection = entry.blocking ? matching_blocking_extensions : matching_nonblocking_extensions;
        if (!collection.contains_slow(entry.extension_id))
            collection.append(entry.extension_id);
    }

    if (matching_blocking_extensions.is_empty() && matching_nonblocking_extensions.is_empty()) {
        callback(move(declarative_decision));
        return;
    }

    JsonObject details_obj;
    details_obj.set("requestId"sv, String::number(details.request_id));
    details_obj.set("url"sv, details.url.serialize());
    details_obj.set("method"sv, details.method);
    details_obj.set("type"sv, MUST(String::from_utf8(Web::Extensions::to_string(details.type))));
    details_obj.set("tabId"sv, reveal_request_source && details.tab_id > 0 ? static_cast<i64>(details.tab_id) : -1);
    details_obj.set("frameId"sv, reveal_request_source ? static_cast<i64>(details.frame_id) : 0);
    details_obj.set("parentFrameId"sv, reveal_request_source && details.frame_id > 0 ? static_cast<i64>(details.parent_frame_id) : -1);
    if (reveal_request_source && details.initiator_origin.has_value())
        details_obj.set("originUrl"sv, *details.initiator_origin);
    if (reveal_request_source && details.document_url.has_value())
        details_obj.set("documentUrl"sv, *details.document_url);
    if (details.status_code.has_value())
        details_obj.set("statusCode"sv, *details.status_code);
    if (details.redirect_url.has_value())
        details_obj.set("redirectUrl"sv, details.redirect_url->serialize());
    if (details.error.has_value())
        details_obj.set("error"sv, *details.error);
    auto serialize_details = [](JsonObject details) {
        JsonArray args;
        args.must_append(move(details));
        return args.serialized();
    };
    auto args_json = serialize_details(details_obj);
    String args_with_headers_json;
    if (!extensions_requesting_headers.is_empty()) {
        details_obj.set(event == WebRequestEvent::SendHeaders ? "requestHeaders"sv : "responseHeaders"sv, headers_to_json(details.headers));
        args_with_headers_json = serialize_details(move(details_obj));
    }

    for (auto const& extension_id : matching_nonblocking_extensions) {
        if (!matching_blocking_extensions.contains_slow(extension_id))
            m_dispatch_event(extension_id, "webRequest"sv, to_string(event), extensions_requesting_headers.contains(extension_id) ? args_with_headers_json : args_json, {});
    }

    if (matching_blocking_extensions.is_empty()) {
        callback(move(declarative_decision));
        return;
    }

    auto state = make_ref_counted<EvaluationState>();
    state->extension_ids = move(matching_blocking_extensions);
    state->args_json = move(args_json);
    state->args_with_headers_json = move(args_with_headers_json);
    state->extensions_requesting_headers = move(extensions_requesting_headers);
    state->event = event;
    state->request_url = details.url;
    if (!is_extension_url && details.type != Web::Extensions::WebRequestResourceType::MainFrame)
        state->initiating_url = move(initiator);
    state->headers = details.headers;
    state->decision = move(declarative_decision);
    state->callback = move(callback);
    state->dispatch_event = &m_dispatch_event;
    state->check_redirect = &m_check_redirect;
    state->has_permission = &m_has_permission;
    state->step();
}

}
