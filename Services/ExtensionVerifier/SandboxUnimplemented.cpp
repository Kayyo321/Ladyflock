/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ExtensionVerifier/Sandbox.h>

namespace ExtensionVerifier {

ErrorOr<void> apply_sandbox(StringView)
{
    return Error::from_string_literal("Extension package verification requires a supported sandbox");
}

}
