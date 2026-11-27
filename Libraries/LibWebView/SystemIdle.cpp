/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibWebView/SystemIdle.h>

#if defined(AK_OS_MACOS)
#    include <CoreGraphics/CoreGraphics.h>
#endif

namespace WebView {

static Optional<SystemIdleStatus> s_status_for_testing;

void set_system_idle_status_for_testing(Optional<SystemIdleStatus> status)
{
    s_status_for_testing = move(status);
}

SystemIdleStatus system_idle_status()
{
    if (s_status_for_testing.has_value())
        return *s_status_for_testing;

    SystemIdleStatus status;
#if defined(AK_OS_MACOS)
    auto seconds = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
    if (seconds >= 0)
        status.idle_time = AK::Duration::from_milliseconds(static_cast<i64>(seconds * 1000));
    if (auto session = CGSessionCopyCurrentDictionary()) {
        auto locked = static_cast<CFBooleanRef>(CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked")));
        status.locked = locked && CFGetTypeID(locked) == CFBooleanGetTypeID() && CFBooleanGetValue(locked);
        CFRelease(session);
    }
#endif
    return status;
}

}
