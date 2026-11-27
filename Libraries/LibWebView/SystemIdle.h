/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibWebView/Export.h>

namespace WebView {

// How long the whole system has gone without input, and whether its screen is locked, as the operating system tells
// it. Empty where the platform has no way to tell, in which case the system counts as in use.
struct SystemIdleStatus {
    Optional<AK::Duration> idle_time;
    bool locked { false };
};

WEBVIEW_API SystemIdleStatus system_idle_status();

// Tests set what the system reports, and clear it with an empty value.
WEBVIEW_API void set_system_idle_status_for_testing(Optional<SystemIdleStatus>);

}
