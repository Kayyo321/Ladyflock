/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Types.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Bounded, in-memory counts of what the site protections did, per identity's session. Nothing here is written to disk,
// and a session's counts go away with the session.
class WEBVIEW_API PrivacyActivity {
    AK_MAKE_NONCOPYABLE(PrivacyActivity);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct Counters {
        u64 navigations_sanitized { 0 };
        u64 tracking_parameters_removed { 0 };
        u64 navigations_with_scripting_blocked { 0 };

        bool operator==(Counters const&) const = default;
    };

    PrivacyActivity() = default;

    Counters counters_for(BrowsingSession const&) const;

    void record_sanitized_navigation(BrowsingSession const&, size_t removed_parameter_count);
    void record_scripting_blocked_navigation(BrowsingSession const&);

    void remove_session(u64 session_id) { m_counters.remove(session_id); }

private:
    Counters& counters(BrowsingSession const&);

    HashMap<u64, Counters> m_counters;
};

}
