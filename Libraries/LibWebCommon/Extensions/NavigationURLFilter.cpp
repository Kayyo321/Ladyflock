/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/AnyOf.h>
#include <AK/JsonArray.h>
#include <AK/Utf16String.h>
#include <LibWebCommon/Extensions/NavigationURLFilter.h>

namespace Web::Extensions {

NavigationURLFilter::~NavigationURLFilter() = default;

static bool is_port(JsonValue const& value)
{
    return value.is_integer<u16>() && value.get_double_with_precision_loss().value() == value.as_integer<u16>();
}

ErrorOr<NonnullRefPtr<NavigationURLFilter>> NavigationURLFilter::parse(JsonValue const& filter)
{
    if (!filter.is_object())
        return Error::from_string_literal("Invalid navigation filter");
    auto urls = filter.as_object().get_array("url"sv);
    if (!urls.has_value() || filter.as_object().size() != 1)
        return Error::from_string_literal("Navigation filter requires a URL array");
    auto result = adopt_ref(*new NavigationURLFilter);
    for (auto const& entry : urls->values()) {
        if (!entry.is_object())
            return Error::from_string_literal("Expected a URL filter object");
        Alternative alternative;
        TRY(entry.as_object().try_for_each_member([&](String const& key, JsonValue const& value) -> ErrorOr<void> {
            if (key == "urlMatches"sv || key == "originAndPathMatches"sv) {
                if (!value.is_string())
                    return Error::from_string_literal("Expected a regular expression string");
                auto pattern = Utf16String::from_utf8(value.as_string());
                auto expression = regex::ECMAScriptRegex::compile(pattern.utf16_view(), {});
                if (expression.is_error())
                    return Error::from_string_literal("Invalid URL filter expression");
                (key == "urlMatches"sv ? alternative.url_regex : alternative.origin_path_regex) = expression.release_value();
                return {};
            }
            if (key == "ports"sv) {
                if (!value.is_array())
                    return Error::from_string_literal("Expected a ports array");
                for (auto const& port : value.as_array().values()) {
                    if (is_port(port))
                        continue;
                    if (!port.is_array() || port.as_array().size() != 2 || !is_port(port.as_array().at(0)) || !is_port(port.as_array().at(1)) || port.as_array().at(0).as_integer<u16>() > port.as_array().at(1).as_integer<u16>())
                        return Error::from_string_literal("Invalid URL filter port range");
                }
            } else if (key == "schemes"sv) {
                if (!value.is_array() || !all_of(value.as_array().values(), [](auto const& scheme) { return scheme.is_string(); }))
                    return Error::from_string_literal("Expected an array of schemes");
            } else {
                auto name = key.bytes_as_string_view();
                auto prefix = name.find_any_of("CSEP"sv);
                if (!prefix.has_value() || !name.substring_view(0, *prefix).is_one_of("url"sv, "host"sv, "query"sv, "path"sv)
                    || !name.substring_view(*prefix).is_one_of("Contains"sv, "Suffix"sv, "Equals"sv, "Prefix"sv) || !value.is_string())
                    return Error::from_string_literal("Invalid URL filter criterion");
            }
            alternative.fields.set(key, value);
            return {};
        }));
        result->m_alternatives.append(move(alternative));
    }
    return result;
}

bool NavigationURLFilter::matches(URL::URL const& url) const
{
    auto serialized = url.serialize(URL::ExcludeFragment::Yes);
    auto full = serialized.bytes_as_string_view();
    return any_of(m_alternatives, [&](Alternative const& alternative) {
        if (alternative.url_regex.has_value()) {
            auto input = Utf16String::from_utf8(full);
            if (alternative.url_regex->test(input.utf16_view(), 0) != regex::MatchResult::Match)
                return false;
        }
        if (alternative.origin_path_regex.has_value()) {
            auto origin_path = full.substring_view(0, full.find('?').value_or(full.length()));
            auto input = Utf16String::from_utf8(origin_path);
            if (alternative.origin_path_regex->test(input.utf16_view(), 0) != regex::MatchResult::Match)
                return false;
        }
        bool matches = true;
        alternative.fields.for_each_member([&](auto const& key, auto const& value) {
            if (!matches)
                return;
            if (key == "schemes"sv) {
                matches = any_of(value.as_array().values(), [&](auto const& scheme) { return scheme.as_string() == url.scheme(); });
                return;
            }
            if (key == "ports"sv) {
                auto port = url.port().value_or_lazy_evaluated_optional([&] { return URL::default_port_for_scheme(url.scheme()); });
                matches = port.has_value() && any_of(value.as_array().values(), [&](JsonValue const& range) {
                    if (range.is_number())
                        return *port == range.as_integer<u16>();
                    return *port >= range.as_array().at(0).as_integer<u16>() && *port <= range.as_array().at(1).as_integer<u16>();
                });
                return;
            }
            auto name = key.bytes_as_string_view();
            auto needle = value.as_string().bytes_as_string_view();
            auto part = name.starts_with("host"sv) ? url.serialized_host() : name.starts_with("path"sv) ? url.serialize_path()
                : name.starts_with("query"sv)                                                           ? url.query().value_or({})
                                                                                                        : full;
            if (name.ends_with("Equals"sv))
                matches = part == needle;
            else if (name.ends_with("Prefix"sv))
                matches = part.starts_with(needle);
            else if (name.ends_with("Suffix"sv))
                matches = part.ends_with(needle);
            else
                matches = part.contains(needle) || (key == "hostContains"sv && needle.starts_with('.') && part.starts_with(needle.substring_view(1)));
        });
        return matches;
    });
}

}
