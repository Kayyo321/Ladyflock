/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/JsonArray.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebView/Export.h>

namespace WebView::Extensions {

struct DeclarativeRequestRule {
    enum class FilterKind { Contains,
        Host,
        StartsWith,
        EndsWith,
        Exact };

    u32 priority { 1 };
    bool allow { false };
    FilterKind filter_kind { FilterKind::Contains };
    String filter;
    bool case_sensitive { false };
    Vector<String> request_domains;
    Vector<String> excluded_request_domains;
    Vector<String> initiator_domains;
    Vector<String> excluded_initiator_domains;
    Vector<Web::Extensions::WebRequestResourceType> types;
    Vector<Web::Extensions::WebRequestResourceType> excluded_types;

    bool matches(StringView url, StringView host, StringView initiator_host, Web::Extensions::WebRequestResourceType) const;
};

struct DeclarativeRuleset {
    String id;
    bool enabled { false };
    Vector<DeclarativeRequestRule> rules;
};

WEBVIEW_API ErrorOr<Vector<DeclarativeRuleset>> load_declarative_rulesets(Web::Extensions::ExtensionManifest const&, Function<ErrorOr<String>(StringView)> read_ruleset);
WEBVIEW_API bool should_block_declarative_request(ReadonlySpan<DeclarativeRuleset>, Web::Extensions::WebRequestBeforeRequestDetails const&, Optional<URL::URL> const& initiator);

}
