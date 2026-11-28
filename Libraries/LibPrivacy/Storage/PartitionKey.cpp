/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibPrivacy/Storage/PartitionKey.h>
#include <LibURL/Parser.h>
#include <LibURL/Site.h>

namespace Privacy {

Optional<StoragePartitionKey> StoragePartitionKey::from_context(
    URL::Origin const& origin,
    Optional<URL::Origin> const& top_level_origin,
    bool has_cross_site_ancestor,
    bool allow_opaque_file_origin)
{
    // An allowed file: origin, as either origin, is keyed as the one file: origin that file: URLs have when they have
    // tuple origins.
    auto is_allowed_file_origin = [&](URL::Origin const& candidate) {
        return allow_opaque_file_origin && candidate.is_opaque_file_origin();
    };

    if (origin.is_opaque() && !is_allowed_file_origin(origin))
        return {};

    if (!top_level_origin.has_value() || (top_level_origin->is_opaque() && !is_allowed_file_origin(*top_level_origin)))
        return {};

    auto top_level_site_utf16 = URL::Site::serialize_for_partitioning(*top_level_origin);
    if (!top_level_site_utf16.has_value())
        return {};

    auto top_level_site = top_level_site_utf16->to_utf8();
    if (top_level_site.is_empty())
        return {};

    auto origin_site_utf16 = URL::Site::serialize_for_partitioning(origin);
    bool cross_site_ancestor = has_cross_site_ancestor
        || !origin_site_utf16.has_value()
        || *origin_site_utf16 != *top_level_site_utf16;

    // Each file: document has an opaque origin of its own, so a file: frame of a file: page is never same site with
    // it. Both are keyed as the one file: origin all the same, so the frame's storage is the page's.
    if (is_allowed_file_origin(origin) && is_allowed_file_origin(*top_level_origin))
        cross_site_ancestor = false;

    return StoragePartitionKey {
        .origin = is_allowed_file_origin(origin) ? "file://"_string : origin.serialize(),
        .top_level_site = move(top_level_site),
        .has_cross_site_ancestor = cross_site_ancestor,
    };
}

Optional<StoragePartitionKey> StoragePartitionKey::parse(StringView encoded)
{
    if (!encoded.starts_with(STORAGE_PARTITION_KEY_PREFIX))
        return {};

    auto parts = encoded.split_view('|', SplitBehavior::KeepEmpty);
    if (parts.size() != 4 || parts[0] != "p1"sv)
        return {};
    if (parts[1].is_empty() || parts[2].is_empty())
        return {};
    if (parts[3] != "0"sv && parts[3] != "1"sv)
        return {};

    auto origin = String::from_utf8(parts[1]);
    auto top_level_site = String::from_utf8(parts[2]);
    if (origin.is_error() || top_level_site.is_error())
        return {};

    return StoragePartitionKey {
        .origin = origin.release_value(),
        .top_level_site = top_level_site.release_value(),
        .has_cross_site_ancestor = parts[3] == "1"sv,
    };
}

String StoragePartitionKey::to_storage_key_string() const
{
    return MUST(String::formatted("{}{}|{}|{}",
        STORAGE_PARTITION_KEY_PREFIX,
        origin,
        top_level_site,
        has_cross_site_ancestor ? '1' : '0'));
}

LegacyStorageKeyClassification classify_legacy_storage_key(StringView raw_storage_key)
{
    if (auto existing = StoragePartitionKey::parse(raw_storage_key); existing.has_value()) {
        return {
            .action = LegacyStorageKeyAction::AlreadyPartitioned,
            .migrated_storage_key = existing->to_storage_key_string(),
            .quarantine_reason = {},
        };
    }

    if (raw_storage_key == "file://"sv || raw_storage_key.starts_with("file:"sv)) {
        return {
            .action = LegacyStorageKeyAction::Quarantine,
            .migrated_storage_key = {},
            .quarantine_reason = "legacy-file-origin-exception"sv,
        };
    }

    if (raw_storage_key.is_empty() || raw_storage_key == "null"sv) {
        return {
            .action = LegacyStorageKeyAction::Quarantine,
            .migrated_storage_key = {},
            .quarantine_reason = "opaque-or-empty-origin"sv,
        };
    }

    auto parsed_url = URL::Parser::basic_parse(raw_storage_key);
    if (!parsed_url.has_value() || !parsed_url->scheme().is_one_of("http"sv, "https"sv)) {
        return {
            .action = LegacyStorageKeyAction::Quarantine,
            .migrated_storage_key = {},
            .quarantine_reason = "unparseable-or-non-http-origin"sv,
        };
    }

    auto origin = parsed_url->origin();
    auto serialized_origin = origin.serialize();
    if (origin.is_opaque() || serialized_origin.bytes_as_string_view() != raw_storage_key) {
        return {
            .action = LegacyStorageKeyAction::Quarantine,
            .migrated_storage_key = {},
            .quarantine_reason = "non-canonical-origin"sv,
        };
    }

    auto partitioned = StoragePartitionKey::from_context(origin, origin, false);
    if (!partitioned.has_value()) {
        return {
            .action = LegacyStorageKeyAction::Quarantine,
            .migrated_storage_key = {},
            .quarantine_reason = "site-key-unreconstructable"sv,
        };
    }

    return {
        .action = LegacyStorageKeyAction::MigrateToFirstParty,
        .migrated_storage_key = partitioned->to_storage_key_string(),
        .quarantine_reason = {},
    };
}

}
