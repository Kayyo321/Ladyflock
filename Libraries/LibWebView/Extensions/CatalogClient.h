/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>
#include <LibWebCommon/Extensions/SupportMatrix.h>
#include <LibWebView/Export.h>

namespace WebView::Extensions {

struct WEBVIEW_API CatalogEntry {
    String addon_id;
    String slug;
    String name;
    String summary;
    String version;
    Optional<String> download_url;
    Optional<String> sha256_hex;
    Vector<String> permissions;
    Web::Extensions::CompatibilityStatus compatibility_status { Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested };
    Vector<String> compatibility_reasons;
    String listing_url;
    String author;
    Optional<String> icon_url;
};

struct WEBVIEW_API CatalogSearchResult {
    Vector<CatalogEntry> entries;
    bool has_more { false };
};

// Optional AMO API v5 catalog client. Gated behind enable_public_amo_catalog (default: false).
class WEBVIEW_API CatalogClient {
public:
    bool enable_public_amo_catalog() const { return m_enable_public_amo_catalog; }
    void set_enable_public_amo_catalog(bool enabled) { m_enable_public_amo_catalog = enabled; }

    String const& api_base_url() const { return m_api_base_url; }
    void set_api_base_url(String url) { m_api_base_url = move(url); }

    void set_offline_fixture_json(Optional<String> json) { m_offline_fixture_json = move(json); }

    static ErrorOr<Vector<CatalogEntry>> parse_search_response(StringView json_text);

    void search(StringView query, Function<void(ErrorOr<Vector<CatalogEntry>>)> callback) const;
    void search(StringView query, u32 page, Function<void(ErrorOr<CatalogSearchResult>)> callback) const;
    void lookup(StringView addon_id, Function<void(ErrorOr<CatalogEntry>)> callback) const;
    void download(CatalogEntry const&, Function<void(ErrorOr<ByteBuffer>)> callback) const;
    void download_icon(StringView url, Function<void(ErrorOr<ByteBuffer>)> callback) const;

private:
    bool m_enable_public_amo_catalog { false };
    String m_api_base_url { "https://addons.mozilla.org/api/v5"_string };
    Optional<String> m_offline_fixture_json;
};

}
