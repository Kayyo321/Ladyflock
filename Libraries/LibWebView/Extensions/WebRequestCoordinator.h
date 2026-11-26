/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>
#include <LibWebView/Export.h>

namespace WebView::Extensions {

// Coordinates webRequest.onBeforeRequest listener registration, watch snapshots pushed
// to WebContent and RequestServer processes, and sequential async blocking evaluation.
// Strictly independent of native Web::ContentBlocker.
class WEBVIEW_API WebRequestCoordinator {
public:
    using SnapshotCallback = Function<void(Web::Extensions::WebRequestWatchSnapshot const&)>;
    using DispatchEventCallback = Function<void(
        String const& extension_id,
        StringView namespace_name,
        StringView event_name,
        String const& args_json,
        Function<void(bool has_value, String const& result_json)> reply_callback)>;
    using PermissionCheckCallback = Function<bool(StringView extension_id, URL::URL const& url)>;

    void set_on_snapshot_changed(SnapshotCallback cb) { m_on_snapshot_changed = move(cb); }
    void set_dispatch_event_callback(DispatchEventCallback cb) { m_dispatch_event = move(cb); }
    void set_permission_check_callback(PermissionCheckCallback cb) { m_has_permission = move(cb); }

    void add_listener(
        String extension_id,
        u64 listener_id,
        u64 grant_generation,
        Vector<String> url_patterns,
        Vector<Web::Extensions::WebRequestResourceType> types,
        bool blocking);

    void remove_listener(StringView extension_id, u64 listener_id);
    void remove_all_for_extension(StringView extension_id);

    Web::Extensions::WebRequestWatchSnapshot snapshot() const { return m_snapshot; }
    bool has_matching_listener(URL::URL const&, Web::Extensions::WebRequestResourceType) const;

    void evaluate_before_request(
        Web::Extensions::WebRequestBeforeRequestDetails const& details,
        Function<void(Web::Extensions::WebRequestDecision)> callback) const;

private:
    void rebuild_snapshot();

    Vector<Web::Extensions::WebRequestWatchEntry> m_entries;
    Web::Extensions::WebRequestWatchSnapshot m_snapshot;
    u64 m_revision { 1 };

    SnapshotCallback m_on_snapshot_changed;
    DispatchEventCallback m_dispatch_event;
    PermissionCheckCallback m_has_permission;
};

}
