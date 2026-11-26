/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>

namespace ExtensionVerifier {

Web::Extensions::XpiVerificationResult verify_xpi_bytes(
    ReadonlyBytes xpi_bytes,
    Optional<String> const& expected_addon_id,
    Vector<String> const& trusted_root_pems,
    Vector<String> const& revoked_addon_ids,
    Vector<String> const& revoked_sha256_hexes,
    Vector<String>& file_names,
    Vector<ByteBuffer>& file_contents);

}
