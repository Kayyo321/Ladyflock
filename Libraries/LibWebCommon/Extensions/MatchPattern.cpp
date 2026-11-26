/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/StringBuilder.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

namespace Web::Extensions {

bool MatchPattern::matches_glob(StringView text, StringView glob_pattern)
{
    // Standard glob supporting '*' (any sequence) and '?' (single byte).
    size_t ti = 0;
    size_t pi = 0;
    size_t star_idx = NumericLimits<size_t>::max();
    size_t match_idx = 0;

    while (ti < text.length()) {
        if (pi < glob_pattern.length() && (glob_pattern[pi] == '?' || glob_pattern[pi] == text[ti])) {
            ++ti;
            ++pi;
        } else if (pi < glob_pattern.length() && glob_pattern[pi] == '*') {
            star_idx = pi;
            match_idx = ti;
            ++pi;
        } else if (star_idx != NumericLimits<size_t>::max()) {
            pi = star_idx + 1;
            ++match_idx;
            ti = match_idx;
        } else {
            return false;
        }
    }

    while (pi < glob_pattern.length() && glob_pattern[pi] == '*')
        ++pi;

    return pi == glob_pattern.length();
}

static bool is_valid_host_component(StringView host)
{
    if (host.is_empty())
        return false;
    for (auto ch : host) {
        if (ch == '/' || ch == '?' || ch == '#' || ch == ':' || ch == ' ')
            return false;
    }
    return true;
}

ErrorOr<MatchPattern> MatchPattern::parse(StringView pattern)
{
    if (pattern == "<all_urls>"sv) {
        return MatchPattern(
            "<all_urls>"_string,
            true,
            Scheme::AllHttpHttps,
            "*"_string,
            false,
            "/*"_string);
    }

    auto scheme_sep = pattern.find("://"sv);
    if (!scheme_sep.has_value())
        return Error::from_string_literal("Match pattern missing '://'");

    auto scheme_view = pattern.substring_view(0, *scheme_sep);
    Scheme scheme;
    if (scheme_view == "*"sv)
        scheme = Scheme::AllHttpHttps;
    else if (scheme_view == "http"sv)
        scheme = Scheme::Http;
    else if (scheme_view == "https"sv)
        scheme = Scheme::Https;
    else if (scheme_view == "file"sv)
        scheme = Scheme::File;
    else if (scheme_view == "moz-extension"sv)
        scheme = Scheme::MozExtension;
    else
        return Error::from_string_literal("Unsupported scheme in match pattern");

    auto remainder = pattern.substring_view(*scheme_sep + 3);
    auto path_sep = remainder.find('/');
    if (!path_sep.has_value())
        return Error::from_string_literal("Match pattern missing path '/'");

    auto host_view = remainder.substring_view(0, *path_sep);
    auto path_view = remainder.substring_view(*path_sep);

    if (scheme == Scheme::File && !host_view.is_one_of(""sv, "*"sv))
        return Error::from_string_literal("File match patterns only accept an empty or wildcard host");
    if (host_view.is_empty() && scheme != Scheme::File)
        return Error::from_string_literal("Match pattern has empty host");

    bool match_subdomains = false;
    String normalized_host;
    if (host_view == "*"sv || scheme == Scheme::File) {
        normalized_host = "*"_string;
    } else if (host_view.starts_with("*."sv)) {
        auto base = host_view.substring_view(2);
        if (base.contains('*') || !is_valid_host_component(base))
            return Error::from_string_literal("Invalid wildcard host in match pattern");
        match_subdomains = true;
        normalized_host = base.to_ascii_lowercase_string();
    } else {
        if (host_view.contains('*') || !is_valid_host_component(host_view))
            return Error::from_string_literal("Invalid host in match pattern");
        normalized_host = host_view.to_ascii_lowercase_string();
    }

    return MatchPattern(
        TRY(String::from_utf8(pattern)),
        false,
        scheme,
        move(normalized_host),
        match_subdomains,
        TRY(String::from_utf8(path_view)));
}

bool MatchPattern::is_valid_host_permission_pattern(StringView pattern)
{
    auto parsed = parse(pattern);
    return !parsed.is_error() && parsed.value().is_web_host_permission();
}

bool MatchPattern::covers_host(StringView host) const
{
    if (m_host == "*"sv || m_host.bytes_as_string_view().equals_ignoring_ascii_case(host))
        return true;
    if (!m_match_subdomains || host.length() <= m_host.bytes_as_string_view().length())
        return false;
    auto prefix_length = host.length() - m_host.bytes_as_string_view().length();
    return host[prefix_length - 1] == '.'
        && host.substring_view(prefix_length).equals_ignoring_ascii_case(m_host.bytes_as_string_view());
}

bool MatchPattern::covers_origin(MatchPattern const& requested) const
{
    if (m_scheme != requested.m_scheme && !(m_scheme == Scheme::AllHttpHttps && (requested.m_scheme == Scheme::Http || requested.m_scheme == Scheme::Https)))
        return false;
    if (m_host == "*"sv)
        return true;
    if (requested.m_match_subdomains && !m_match_subdomains)
        return false;
    return covers_host(requested.m_host.bytes_as_string_view());
}

bool MatchPattern::matches(URL::URL const& url) const
{
    auto url_scheme = url.scheme();
    bool scheme_ok = false;
    switch (m_scheme) {
    case Scheme::AllHttpHttps:
        scheme_ok = url_scheme == "http"sv || url_scheme == "https"sv;
        break;
    case Scheme::Http:
        scheme_ok = url_scheme == "http"sv;
        break;
    case Scheme::Https:
        scheme_ok = url_scheme == "https"sv;
        break;
    case Scheme::File:
        scheme_ok = url_scheme == "file"sv;
        break;
    case Scheme::MozExtension:
        scheme_ok = url_scheme == "moz-extension"sv;
        break;
    }

    if (!scheme_ok)
        return false;

    if (m_is_all_urls)
        return true;

    if (!covers_host(url.serialized_host()))
        return false;

    // Match path + optional ?query (WebExtensions match patterns match path and query, ignoring fragment).
    StringBuilder path_and_query;
    path_and_query.append(url.serialize_path());
    if (url.query().has_value()) {
        path_and_query.append('?');
        path_and_query.append(*url.query());
    }

    return matches_glob(path_and_query.string_view(), m_path_pattern.bytes_as_string_view());
}

bool MatchPattern::matches_url_with_rules(
    URL::URL const& url,
    Vector<MatchPattern> const& matches,
    Vector<MatchPattern> const& exclude_matches,
    Vector<String> const& include_globs,
    Vector<String> const& exclude_globs)
{
    bool matched = false;
    for (auto const& pattern : matches) {
        if (pattern.matches(url)) {
            matched = true;
            break;
        }
    }
    if (!matched)
        return false;

    for (auto const& pattern : exclude_matches) {
        if (pattern.matches(url))
            return false;
    }

    auto serialized_url = url.serialize(URL::ExcludeFragment::Yes);
    if (!include_globs.is_empty()) {
        bool glob_matched = false;
        for (auto const& glob : include_globs) {
            if (matches_glob(serialized_url, glob)) {
                glob_matched = true;
                break;
            }
        }
        if (!glob_matched)
            return false;
    }

    for (auto const& glob : exclude_globs) {
        if (matches_glob(serialized_url, glob))
            return false;
    }

    return true;
}

}
