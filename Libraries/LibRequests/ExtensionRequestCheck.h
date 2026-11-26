/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Requests {

enum class ExtensionRequestCheck : u8 {
    NotApplicable,
    AlreadyChecked,
    MainFrame,
};

}
