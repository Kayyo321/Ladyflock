/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Match_patterns
class WEBCOMMON_API MatchPattern {
public:
    enum class Scheme : u8 {
        AllHttpHttps,
        Http,
        Https,
        MozExtension,
    };

    static ErrorOr<MatchPattern> parse(StringView pattern);

    bool matches(URL::URL const& url) const;
    bool covers_origin(MatchPattern const&) const;
    bool covers_host(StringView) const;
    String const& raw() const { return m_raw; }

    static bool is_valid_host_permission_pattern(StringView pattern);
    static bool matches_glob(StringView text, StringView glob_pattern);
    static bool matches_url_with_rules(
        URL::URL const& url,
        Vector<MatchPattern> const& matches,
        Vector<MatchPattern> const& exclude_matches,
        Vector<String> const& include_globs,
        Vector<String> const& exclude_globs);

    bool operator==(MatchPattern const& other) const { return m_raw == other.m_raw; }

private:
    MatchPattern(String raw, bool is_all_urls, Scheme scheme, String host, bool match_subdomains, String path_pattern)
        : m_raw(move(raw))
        , m_is_all_urls(is_all_urls)
        , m_scheme(scheme)
        , m_host(move(host))
        , m_match_subdomains(match_subdomains)
        , m_path_pattern(move(path_pattern))
    {
    }

    String m_raw;
    bool m_is_all_urls { false };
    Scheme m_scheme { Scheme::AllHttpHttps };
    String m_host;
    bool m_match_subdomains { false };
    String m_path_pattern;
};

}
