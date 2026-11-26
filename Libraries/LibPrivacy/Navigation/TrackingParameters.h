/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibPrivacy/Export.h>
#include <LibURL/Forward.h>

namespace Privacy {

// Query parameters that only say where a visit came from (campaign tags and click identifiers) and never change what a
// page shows. The browser's autocomplete and its navigation sanitizer share this one list. Names compare without regard
// to ASCII case, after percent-decoding.
PRIVACY_API bool is_tracking_query_parameter(StringView name);

struct PRIVACY_API StrippedQuery {
    // The query without the tracking parameters, or nothing if none is left.
    Optional<String> query;
    size_t removed_parameter_count { 0 };
};

// Removes the tracking parameters from a serialized query, leaving every other byte as it was: the remaining parameters
// keep their order, their encoding and their separators, so a signed URL keeps its signature. Returns nothing when there
// is nothing to remove.
PRIVACY_API Optional<StrippedQuery> strip_tracking_parameters_from_query(StringView query);

// The URL without tracking query parameters, or nothing if it has none. Only http and https URLs are rewritten; the
// fragment is kept as it was.
PRIVACY_API Optional<URL::URL> strip_tracking_parameters(URL::URL const&, size_t* removed_parameter_count = nullptr);

}
