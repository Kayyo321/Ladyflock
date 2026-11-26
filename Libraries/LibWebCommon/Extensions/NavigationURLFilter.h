/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibRegex/ECMAScriptRegex.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

// The alternatives in events.UrlFilter[] are disjunctive. A single object's criteria are conjunctive.
class WEBCOMMON_API NavigationURLFilter : public RefCounted<NavigationURLFilter> {
public:
    ~NavigationURLFilter();

    static ErrorOr<NonnullRefPtr<NavigationURLFilter>> parse(JsonValue const&);
    bool matches(URL::URL const&) const;

private:
    struct Alternative {
        JsonObject fields;
        Optional<regex::ECMAScriptRegex> url_regex;
        Optional<regex::ECMAScriptRegex> origin_path_regex;
    };
    Vector<Alternative> m_alternatives;
};

}
