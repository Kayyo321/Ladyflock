/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>

namespace Web::Extensions {

class WEB_API WebRequestClient {
public:
    static void set_watch_snapshot(WebRequestWatchSnapshot);
    static WebRequestWatchSnapshot const& watch_snapshot();

    static bool matches_watch_snapshot(URL::URL const&, WebRequestResourceType, WebRequestEvent = WebRequestEvent::BeforeRequest);
    static WebRequestResourceType resource_type_from_request(Fetch::Infrastructure::Request const&);
};

}
