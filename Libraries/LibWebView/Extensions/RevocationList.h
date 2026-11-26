/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>

namespace WebView::Extensions {

struct WEBVIEW_API RevocationEntry {
    String addon_id;
    Optional<String> version;
    Optional<String> sha256_hex;
    String reason;
};

// Signed extension revocation list verified fail-closed via OpenSSL public-key verification.
class WEBVIEW_API RevocationList {
public:
    ErrorOr<void> load_bundled(ByteString const& resource_root);
    ErrorOr<void> load_signed_json(StringView payload_json, StringView signature_base64, StringView public_key_pem);
    ErrorOr<void> load_signed_envelope(StringView envelope_json, StringView public_key_pem);

    bool is_revoked(StringView addon_id, Optional<StringView> version = {}, Optional<StringView> sha256_hex = {}) const;

    Vector<String> revoked_addon_ids() const;
    Vector<String> revoked_sha256_hexes() const;
    Vector<RevocationEntry> const& entries() const { return m_entries; }

    void add_entry_for_testing(RevocationEntry entry) { m_entries.append(move(entry)); }

private:
    Vector<RevocationEntry> m_entries;
};

}
