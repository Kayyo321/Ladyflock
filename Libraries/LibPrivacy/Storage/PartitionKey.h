/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibPrivacy/Export.h>
#include <LibURL/Origin.h>

namespace Privacy {

// Version prefix for canonical partitioned WebStorage keys:
//   p1|<serialized_origin>|<serialized_top_level_site>|<0|1>
// where the trailing bit records has_cross_site_ancestor (separating A->B->A from first-party A).
constexpr inline StringView STORAGE_PARTITION_KEY_PREFIX = "p1|"sv;

struct PRIVACY_API StoragePartitionKey {
    String origin;
    String top_level_site;
    bool has_cross_site_ancestor { false };

    // Obtains a partitioned persistent web-storage key for (origin, top_level_origin, has_cross_site_ancestor).
    // - Ordinary opaque origins ALWAYS return empty (cannot obtain a persistent web-storage key).
    // - Opaque file:// origins return empty unless allow_opaque_file_origin is explicitly requested.
    // - Missing or opaque top_level_origin returns empty.
    static Optional<StoragePartitionKey> from_context(
        URL::Origin const& origin,
        Optional<URL::Origin> const& top_level_origin,
        bool has_cross_site_ancestor,
        bool allow_opaque_file_origin = false);

    // Parses a canonical "p1|..." string back into a StoragePartitionKey.
    static Optional<StoragePartitionKey> parse(StringView);

    String to_storage_key_string() const;

    bool operator==(StoragePartitionKey const&) const = default;
};

enum class LegacyStorageKeyAction : u8 {
    AlreadyPartitioned,
    MigrateToFirstParty,
    Quarantine,
};

struct PRIVACY_API LegacyStorageKeyClassification {
    LegacyStorageKeyAction action { LegacyStorageKeyAction::Quarantine };
    Optional<String> migrated_storage_key;
    ByteString quarantine_reason;
};

// Classifies a pre-partitioning WebStorage.storage_key row during schema migration:
// - Existing "p1|..." rows stay as-is.
// - Well-formed http(s) tuple origins reconstruct a deterministic first-party partition for the default identity.
// - Legacy "file://", "null", opaque, or unparseable keys cannot reconstruct a trustworthy site key and are
//   quarantined out of live partitions pending explicit user recovery or clearing.
PRIVACY_API LegacyStorageKeyClassification classify_legacy_storage_key(StringView raw_storage_key);

}
