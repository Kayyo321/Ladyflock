/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibWeb/Extensions/WebRequestClient.h>

namespace Web::Extensions {

static WebRequestWatchSnapshot& watch_snapshot_storage()
{
    static NeverDestroyed<WebRequestWatchSnapshot> snapshot;
    return *snapshot;
}

void WebRequestClient::set_watch_snapshot(WebRequestWatchSnapshot snapshot)
{
    watch_snapshot_storage() = move(snapshot);
}

WebRequestWatchSnapshot const& WebRequestClient::watch_snapshot()
{
    return watch_snapshot_storage();
}

bool WebRequestClient::matches_watch_snapshot(URL::URL const& url, WebRequestResourceType type)
{
    return watch_snapshot_storage().matches(url, type);
}

WebRequestResourceType WebRequestClient::resource_type_from_request(Fetch::Infrastructure::Request const& request)
{
    using Destination = Fetch::Infrastructure::Request::Destination;
    if (request.mode() == Fetch::Infrastructure::Request::Mode::Navigate) {
        if (request.destination() == Destination::Document)
            return WebRequestResourceType::MainFrame;
        return WebRequestResourceType::SubFrame;
    }

    auto destination = request.destination();
    if (!destination.has_value())
        return WebRequestResourceType::XmlHttpRequest;

    switch (*destination) {
    case Destination::Document:
        return WebRequestResourceType::MainFrame;
    case Destination::IFrame:
    case Destination::Frame:
        return WebRequestResourceType::SubFrame;
    case Destination::Style:
        return WebRequestResourceType::Stylesheet;
    case Destination::Script:
    case Destination::Worker:
    case Destination::SharedWorker:
    case Destination::ServiceWorker:
        return WebRequestResourceType::Script;
    case Destination::Image:
        return WebRequestResourceType::Image;
    case Destination::Font:
        return WebRequestResourceType::Font;
    case Destination::Object:
    case Destination::Embed:
        return WebRequestResourceType::Object;
    case Destination::Audio:
    case Destination::Video:
    case Destination::Track:
        return WebRequestResourceType::Media;
    default:
        return WebRequestResourceType::Other;
    }
}

}
