/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebView/Export.h>

namespace WebView::Extensions {

struct DeclarativeRequestRule {
    enum class FilterKind : u8 { Contains,
        Host,
        StartsWith,
        EndsWith,
        Exact,
        Pattern };
    enum class Action : u8 {
        ModifyHeaders,
        Redirect,
        Block,
        Allow,
    };
    enum class DomainType : u8 {
        Any,
        FirstParty,
        ThirdParty,
    };

    u32 priority { 1 };
    Action action { Action::Block };
    FilterKind filter_kind { FilterKind::Contains };
    DomainType domain_type { DomainType::Any };
    String filter;
    bool case_sensitive { false };
    bool anchor_domain { false };
    bool anchor_start { false };
    bool anchor_end { false };
    HashTable<String> request_domains;
    HashTable<String> excluded_request_domains;
    HashTable<String> initiator_domains;
    HashTable<String> excluded_initiator_domains;
    Vector<Web::Extensions::WebRequestResourceType> types;
    Vector<Web::Extensions::WebRequestResourceType> excluded_types;
    Vector<String> methods;
    Vector<String> excluded_methods;
    Optional<String> extension_path;
    Optional<String> replacement_query;
    Vector<String> remove_query_parameters;

    struct HeaderModification {
        String name;
        Optional<String> append_value;
    };
    Vector<HeaderModification> response_headers;

    bool matches(StringView url, StringView host, size_t host_offset, StringView initiator_host, Web::Extensions::WebRequestResourceType, StringView method, bool first_party) const;
};

struct DeclarativeRuleset {
    String id;
    bool enabled { false };
    Vector<DeclarativeRequestRule> rules;
};

struct MutableDeclarativeRuleset {
    JsonArray source;
    Vector<DeclarativeRequestRule> rules;
};

WEBVIEW_API ErrorOr<Vector<DeclarativeRuleset>> load_declarative_rulesets(Web::Extensions::ExtensionManifest const&, Function<ErrorOr<String>(StringView)> read_ruleset);
WEBVIEW_API bool declarative_ruleset_limits_ok(ReadonlySpan<DeclarativeRuleset>, ReadonlySpan<bool> enabled);
WEBVIEW_API size_t available_static_declarative_rules(ReadonlySpan<DeclarativeRuleset>);
WEBVIEW_API ErrorOr<MutableDeclarativeRuleset> parse_mutable_ruleset(JsonArray);

struct DeclarativeRequestMatch {
    DeclarativeRequestRule const* winner { nullptr };
    Vector<DeclarativeRequestRule const*> response_header_rules;
};

WEBVIEW_API DeclarativeRequestMatch match_declarative_request(ReadonlySpan<DeclarativeRuleset>, Web::Extensions::WebRequestBeforeRequestDetails const&, Optional<URL::URL> const& initiator, bool has_host_access, ReadonlySpan<DeclarativeRequestRule> dynamic_rules = {}, ReadonlySpan<DeclarativeRequestRule> session_rules = {});
WEBVIEW_API Optional<URL::URL> declarative_redirect(DeclarativeRequestRule const&, URL::URL const& request_url, StringView extension_uuid);

}
