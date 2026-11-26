/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/BrowsingSession.h>
#include <LibWebView/PrivacyActivity.h>

namespace WebView {

// Counts saturate instead of wrapping.
static void add_saturating(u64& counter, u64 amount)
{
    counter = counter > NumericLimits<u64>::max() - amount ? NumericLimits<u64>::max() : counter + amount;
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
