/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <LibCore/Timer.h>
#include <LibURL/URL.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/PrivacyActivity.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

// Counts saturate instead of wrapping.
static void add_saturating(u64& counter, u64 amount)
{
    counter = counter > NumericLimits<u64>::max() - amount ? NumericLimits<u64>::max() : counter + amount;
}

PrivacyActivity::PrivacyActivity()
{
    // Expired captures are discarded within a second, whether or not anyone looks at them again.
    m_expiry_timer = Core::Timer::create_repeating(AK::Duration::from_seconds(1), [this] { discard_expired_captures(); });
}

PrivacyActivity::~PrivacyActivity() = default;

void PrivacyActivity::remove_session(u64 session_id)
{
    m_counters.remove(session_id);
    m_captures.remove_all_matching([&](auto const&, auto const& capture) { return capture.session_id == session_id; });
}

void PrivacyActivity::discard_expired_captures()
{
    auto current_time = now();
    m_captures.remove_all_matching([&](auto const&, auto const& capture) { return current_time >= capture.expires_at; });
    if (m_captures.is_empty())
        m_expiry_timer->stop();
}

void PrivacyActivity::start_capture(ViewImplementation const& view, AK::Duration duration)
{
    duration = clamp(duration, MIN_CAPTURE_DURATION, MAX_CAPTURE_DURATION);
    auto current_time = now();
    m_captures.set(view.view_id(), Capture {
                                       .session_id = view.session().id(),
                                       .started_at = current_time,
                                       .expires_at = current_time + duration,
                                       .entries = {},
                                       .dropped_entries = 0,
                                   });
    if (!m_expiry_timer->is_active())
        m_expiry_timer->start();
}

void PrivacyActivity::stop_capture(u64 view_id)
{
    m_captures.remove(view_id);
}

Optional<PrivacyActivity::Capture const&> PrivacyActivity::capture_for(u64 view_id, BrowsingSession const& session)
{
    discard_expired_captures();
    auto capture = m_captures.get(view_id);
    if (!capture.has_value() || capture->session_id != session.id())
        return {};
    return *capture;
}

String PrivacyActivity::host_for_capture(URL::URL const& url)
{
    if (!url.host().has_value())
        return "(no host)"_string;
    auto host = url.serialized_host();
    if (IPv4Address::from_string(host).has_value())
        return "(IPv4 address)"_string;
    if (host.starts_with('['))
        return "(IPv6 address)"_string;
    return host.to_ascii_lowercase_string();
}

void PrivacyActivity::record_capture_event(ViewImplementation const& view, URL::URL const& url, CaptureEventKind kind)
{
    auto it = m_captures.find(view.view_id());
    if (it == m_captures.end())
        return;
    auto& capture = it->value;

    auto current_time = now();
    // A capture ends when it expires or when its tab leaves the identity it was started in; its events go with it.
    if (current_time >= capture.expires_at || capture.session_id != view.session().id()) {
        m_captures.remove(it);
        return;
    }
    if (capture.entries.size() >= MAX_CAPTURE_ENTRIES) {
        ++capture.dropped_entries;
        return;
    }
    capture.entries.append({ .host = host_for_capture(url), .kind = kind, .time = current_time });
}

PrivacyActivity::Counters PrivacyActivity::counters_for(BrowsingSession const& session) const
{
    return m_counters.get(session.id()).value_or({});
}

PrivacyActivity::Counters& PrivacyActivity::counters(BrowsingSession const& session)
{
    return m_counters.ensure(session.id());
}

void PrivacyActivity::record_sanitized_navigation(BrowsingSession const& session, size_t removed_parameter_count)
{
    auto& session_counters = counters(session);
    add_saturating(session_counters.navigations_sanitized, 1);
    add_saturating(session_counters.tracking_parameters_removed, removed_parameter_count);
}

void PrivacyActivity::record_scripting_blocked_navigation(BrowsingSession const& session)
{
    add_saturating(counters(session).navigations_with_scripting_blocked, 1);
}

}
