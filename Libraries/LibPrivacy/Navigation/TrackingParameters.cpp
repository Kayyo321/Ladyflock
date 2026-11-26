/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/StringBuilder.h>
#include <LibPrivacy/Navigation/TrackingParameters.h>
#include <LibURL/URL.h>

namespace Privacy {

bool is_tracking_query_parameter(StringView raw_name)
{
    static constexpr Array tracking_parameter_names {
        "_ga"sv, "_gl"sv, "dclid"sv, "fbclid"sv, "gbraid"sv, "gclid"sv, "igshid"sv, "mc_cid"sv, "mc_eid"sv, "msclkid"sv,
        "srsltid"sv, "wbraid"sv, "yclid"sv
    };

    // A name may be percent-encoded (utm%5Fsource), which does not make it a different parameter.
    auto decoded = raw_name.contains('%') ? URL::percent_decode(raw_name) : raw_name.to_byte_string();
    auto name = decoded.view();
    if (name.starts_with("utm_"sv, CaseSensitivity::CaseInsensitive))
        return true;
    return any_of(tracking_parameter_names, [&](auto tracking_name) { return name.equals_ignoring_ascii_case(tracking_name); });
}

Optional<StrippedQuery> strip_tracking_parameters_from_query(StringView query)
{
    if (query.is_empty())
        return {};

    StringBuilder kept;
    size_t removed = 0;
    bool first = true;
    for (auto parameter : query.split_view('&', SplitBehavior::KeepEmpty)) {
        auto name = parameter;
        if (auto equals = parameter.find('='); equals.has_value())
            name = parameter.substring_view(0, *equals);
        if (!name.is_empty() && is_tracking_query_parameter(name)) {
            ++removed;
            continue;
        }
        if (!first)
            kept.append('&');
        kept.append(parameter);
        first = false;
    }

    if (removed == 0)
        return {};

    StrippedQuery result { .query = {}, .removed_parameter_count = removed };
    if (!kept.is_empty())
        result.query = MUST(kept.to_string());
    return result;
}

Optional<URL::URL> strip_tracking_parameters(URL::URL const& url, size_t* removed_parameter_count)
{
    if (!url.scheme().is_one_of("http"sv, "https"sv))
        return {};
    auto query = url.query();
    if (!query.has_value())
        return {};
    auto stripped = strip_tracking_parameters_from_query(*query);
    if (!stripped.has_value())
        return {};

    auto result = url;
    if (stripped->query.has_value())
        result.set_query(stripped->query->bytes_as_string_view());
    else
        result.set_query({});
    if (removed_parameter_count)
        *removed_parameter_count = stripped->removed_parameter_count;
    return result;
}

}
