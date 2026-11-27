/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibDatabase/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

// The steps of an identity purge, in the order they run.
enum class IdentityPurgeStep : u8 {
    // Stop the identity's traffic: close its views, workers and RequestServers, and detach its stores from disk.
    StopIdentity,
    Cookies,
    WebStorage,
    History,
    // The identity's configuration directory, which holds its permissions.
    Configuration,
    // The identity's HTTP disk caches, including its per-site caches.
    Caches,
    // Rewrite the databases the rows were deleted from, so no copy of them stays in free pages or write-ahead logs.
    CompactDatabases,
    // Remove the identity from the purge journal, which lets it be created again.
    ClearJournal,
};

WEBVIEW_API StringView identity_purge_step_name(IdentityPurgeStep);

struct WEBVIEW_API IdentityPurgeReport {
    struct Failure {
        IdentityPurgeStep step;
        String reason;
    };

    String identity_id;
    Vector<IdentityPurgeStep> completed_steps;
    Vector<Failure> failures;

    // A purge that did not complete stays in the journal, and runs again at the next start before the identity can be
    // created again.
    bool is_complete() const { return failures.is_empty(); }
};

// The durable record of purges that have started and not yet completed. It is written before any data is deleted,
// and an identity stays in it until every step of its purge has succeeded.
class WEBVIEW_API IdentityPurgeJournal {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit IdentityPurgeJournal(ByteString path);

    // Reads the journal from disk. A journal that exists but cannot be read leaves the journal unreadable, which
    // refuses every named identity until it is repaired, as any of them might be one whose purge was interrupted.
    void load();

    bool is_readable() const { return m_readable; }
    bool contains(StringView identity_id) const;
    Vector<String> const& pending() const { return m_pending; }

    ErrorOr<void> add(String const& identity_id);
    ErrorOr<void> remove(StringView identity_id);

    ByteString const& path() const { return m_path; }

private:
    ErrorOr<void> write() const;

    ByteString m_path;
    Vector<String> m_pending;
    bool m_readable { true };
};

struct IdentityPurgeLocations {
    ByteString browsing_database_directory;
    ByteString configuration_root;
    ByteString cache_root;
    ByteString history_database_directory;
};

// Deletes what an identity left on disk, after its live session (if any) has been stopped. Every step runs even if an
// earlier one failed, so a purge removes as much as it can, and each failure is reported.
WEBVIEW_API void purge_identity_data(
    String const& identity_id,
    RefPtr<Database::Database> browsing_database,
    IdentityPurgeLocations const&,
    IdentityPurgeReport&,
    Optional<IdentityPurgeStep> fail_step_for_testing);

}
