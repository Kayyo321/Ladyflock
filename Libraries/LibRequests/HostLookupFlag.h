/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Types.h>

namespace Requests {

enum class HostLookupFlag : u8 {
    None = 0,
    DisableIPv4 = 1 << 0,
    DisableIPv6 = 1 << 1,
    BypassCache = 1 << 2,
    CacheOnly = 1 << 3,
    CanonicalName = 1 << 4,
    AllowNameCollisions = 1 << 5,
    IncludeLocalIPv6 = 1 << 6,
};
AK_ENUM_BITWISE_OPERATORS(HostLookupFlag)

}
