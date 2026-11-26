/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibURL/Forward.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Bounded, in-memory counts of what the site protections did, per identity's session, and the explicit captures that the
// privacy dashboard starts for one tab at a time. Nothing here is written to disk, and a session's counts go away with
// the session.
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

    enum class CaptureEventKind : u8 {
        Request,
        Blocked,
    };

    // One event of a capture. Only the host is kept, never the path, query, credentials, headers or body, and a host
    // that is an IP address is kept as the kind of address it is.
    struct CaptureEntry {
        String host;
        CaptureEventKind kind { CaptureEventKind::Request };
        MonotonicTime time;
    };

    struct Capture {
        u64 session_id { 0 };
        MonotonicTime started_at;
        MonotonicTime expires_at;
        Vector<CaptureEntry> entries;
        size_t dropped_entries { 0 };
    };

    static constexpr size_t MAX_CAPTURE_ENTRIES = 500;
    static constexpr AK::Duration MIN_CAPTURE_DURATION = AK::Duration::from_seconds(10);
    static constexpr AK::Duration MAX_CAPTURE_DURATION = AK::Duration::from_seconds(10 * 60);

    PrivacyActivity();
    ~PrivacyActivity();

    Counters counters_for(BrowsingSession const&) const;

    void record_sanitized_navigation(BrowsingSession const&, size_t removed_parameter_count);
    void record_scripting_blocked_navigation(BrowsingSession const&);

    void remove_session(u64 session_id);

    // Starts (or restarts) a capture of the tab's requests, which stops by itself after the duration (clamped to the
    // limits above), or sooner when the tab closes or leaves the identity it was started in.
    void start_capture(ViewImplementation const&, AK::Duration);
    void stop_capture(u64 view_id);
    void view_closed(u64 view_id) { stop_capture(view_id); }

    // The tab's live capture, if it has one that belongs to this session and has not expired.
    Optional<Capture const&> capture_for(u64 view_id, BrowsingSession const&);

    void record_capture_event(ViewImplementation const&, URL::URL const&, CaptureEventKind);

    // The host as a capture keeps it.
    static String host_for_capture(URL::URL const&);

    void set_clock_for_testing(Function<MonotonicTime()> clock) { m_clock = move(clock); }

private:
    Counters& counters(BrowsingSession const&);
    MonotonicTime now() const { return m_clock ? m_clock() : MonotonicTime::now(); }
    void discard_expired_captures();

    HashMap<u64, Counters> m_counters;
    HashMap<u64, Capture> m_captures;
    RefPtr<Core::Timer> m_expiry_timer;
    Function<MonotonicTime()> m_clock;
};

}
