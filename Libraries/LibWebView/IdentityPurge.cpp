/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibDatabase/Database.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/IdentityPurge.h>
#include <LibWebView/Utilities.h>

#if !defined(AK_OS_WINDOWS)
#    include <fcntl.h>
#    include <unistd.h>
#endif

namespace WebView {

static constexpr i64 JOURNAL_VERSION = 1;

StringView identity_purge_step_name(IdentityPurgeStep step)
{
    switch (step) {
    case IdentityPurgeStep::StopIdentity:
        return "stop-identity"sv;
    case IdentityPurgeStep::Cookies:
        return "cookies"sv;
    case IdentityPurgeStep::WebStorage:
        return "web-storage"sv;
    case IdentityPurgeStep::History:
        return "history"sv;
    case IdentityPurgeStep::Configuration:
        return "configuration"sv;
    case IdentityPurgeStep::Caches:
        return "caches"sv;
    case IdentityPurgeStep::CompactDatabases:
        return "compact-databases"sv;
    case IdentityPurgeStep::ClearJournal:
        return "clear-journal"sv;
    }
    VERIFY_NOT_REACHED();
}

IdentityPurgeJournal::IdentityPurgeJournal(ByteString path)
    : m_path(move(path))
{
}

void IdentityPurgeJournal::load()
{
    m_pending.clear();
    m_readable = true;

    if (!FileSystem::exists(m_path))
        return;

    auto json = read_json_file(m_path);
    if (json.is_error()) {
        warnln("Identity purge journal {} cannot be read ({}); named identities stay closed until it is repaired", m_path, json.error());
        m_readable = false;
        return;
    }

    auto version = json.value().get_integer<i64>("version"sv);
    auto pending = json.value().get_array("pending"sv);
    if (version != JOURNAL_VERSION || !pending.has_value()) {
        warnln("Identity purge journal {} has an unsupported format; named identities stay closed until it is repaired", m_path);
        m_readable = false;
        return;
    }

    for (auto const& value : pending->values()) {
        if (!value.is_string()) {
            m_readable = false;
            m_pending.clear();
            return;
        }
        m_pending.append(value.as_string());
    }
}

bool IdentityPurgeJournal::contains(StringView identity_id) const
{
    return m_pending.contains_slow(identity_id);
}

ErrorOr<void> IdentityPurgeJournal::add(String const& identity_id)
{
    if (!m_readable)
        return Error::from_string_literal("The identity purge journal cannot be read");
    if (contains(identity_id))
        return {};

    m_pending.append(identity_id);
    if (auto result = write(); result.is_error()) {
        m_pending.remove_all_matching([&](auto const& pending) { return pending == identity_id; });
        return result.release_error();
    }
    return {};
}

ErrorOr<void> IdentityPurgeJournal::remove(StringView identity_id)
{
    if (!m_readable)
        return Error::from_string_literal("The identity purge journal cannot be read");

    m_pending.remove_all_matching([&](auto const& pending) { return pending == identity_id; });
    if (m_pending.is_empty())
        return FileSystem::remove(m_path, FileSystem::RecursionMode::Disallowed);
    return write();
}

// The journal must be on disk before any data goes, and must never be left half-written: it is written to a temporary
// file that is synced and then renamed over the journal.
ErrorOr<void> IdentityPurgeJournal::write() const
{
    JsonArray pending;
    for (auto const& identity_id : m_pending)
        TRY(pending.append(identity_id));

    JsonObject journal;
    journal.set("version"sv, JOURNAL_VERSION);
    journal.set("pending"sv, move(pending));

    auto directory = LexicalPath { m_path }.parent();
    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));

    auto temporary_path = ByteString::formatted("{}.tmp", m_path);
    {
        auto file = TRY(Core::File::open(temporary_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        TRY(file->write_until_depleted(journal.serialized()));
#if !defined(AK_OS_WINDOWS)
        if (::fsync(file->fd()) < 0)
            return Error::from_errno(errno);
#endif
    }
    TRY(Core::System::rename(temporary_path, m_path));

    // FIXME: Windows has no directory fsync, and this has not been built there.
#if !defined(AK_OS_WINDOWS)
    auto directory_fd = TRY(Core::System::open(directory.string(), O_RDONLY | O_DIRECTORY));
    ::fsync(directory_fd);
    (void)Core::System::close(directory_fd);
#endif
    return {};
}

static ErrorOr<void> execute(Database::Database& database, StringView sql, Optional<String const&> identity_id = {})
{
    auto statement = TRY(database.prepare_statement(sql));
    return database.try_execute_bound_statement(statement, [&](auto& bind) -> ErrorOr<void> {
        if (identity_id.has_value())
            TRY(bind("identity"sv, *identity_id));
        return {};
    });
}

static ErrorOr<void> remove_directory_if_present(ByteString const& path)
{
    if (!FileSystem::exists(path))
        return {};
    TRY(FileSystem::remove(path, FileSystem::RecursionMode::Allowed));
    if (FileSystem::exists(path))
        return Error::from_string_literal("The directory is still there after removal");
    return {};
}

// VACUUM rewrites the whole file without free pages, and a truncating checkpoint then empties the write-ahead log, so
// neither holds an old copy of a deleted row.
static ErrorOr<void> compact(Database::Database& database)
{
    TRY(execute(database, "VACUUM;"sv));
    TRY(execute(database, "PRAGMA wal_checkpoint(TRUNCATE);"sv));
    return {};
}

void purge_identity_data(
    String const& identity_id,
    RefPtr<Database::Database> browsing_database,
    IdentityPurgeLocations const& locations,
    IdentityPurgeReport& report,
    Optional<IdentityPurgeStep> fail_step_for_testing)
{
    auto run = [&](IdentityPurgeStep step, auto&& action) {
        auto result = [&]() -> ErrorOr<void> {
            if (fail_step_for_testing == step)
                return Error::from_string_literal("Failure injected for testing");
            return action();
        }();
        if (result.is_error()) {
            report.failures.append({ step, MUST(String::formatted("{}", result.error())) });
            return;
        }
        report.completed_steps.append(step);
    };

    // Without the browsing database open, there is nothing to delete only if the file isn't there either: a database
    // this build may not write to, such as one a newer version created, still holds the identity's rows.
    auto no_browsing_database = [&]() -> ErrorOr<void> {
        if (FileSystem::exists(LexicalPath::join(locations.browsing_database_directory, "Ladybird.db"sv).string()))
            return Error::from_string_literal("The browsing database exists but is not open");
        return {};
    };

    run(IdentityPurgeStep::Cookies, [&]() -> ErrorOr<void> {
        if (!browsing_database)
            return no_browsing_database();
        return execute(*browsing_database, "DELETE FROM Cookies WHERE identity_id = :identity;"sv, identity_id);
    });

    run(IdentityPurgeStep::WebStorage, [&]() -> ErrorOr<void> {
        if (!browsing_database)
            return no_browsing_database();
        return execute(*browsing_database, "DELETE FROM WebStorage WHERE identity_id = :identity;"sv, identity_id);
    });

    // History belongs to the UI, in its own database in the profile's data directory. SQLite lets this process delete
    // from it alongside the UI's own connection. A profile that never kept history has nothing to delete.
    RefPtr<Database::Database> history_database;
    run(IdentityPurgeStep::History, [&]() -> ErrorOr<void> {
        auto path = LexicalPath::join(locations.history_database_directory, "History.db"sv).string();
        if (!FileSystem::exists(path))
            return {};
        history_database = TRY(Database::Database::create(locations.history_database_directory, "History"sv));
        TRY(execute(*history_database, "DELETE FROM History WHERE identity_id = :identity;"sv, identity_id));
        TRY(execute(*history_database, "DELETE FROM OmniboxEngagements WHERE identity_id = :identity;"sv, identity_id));
        return {};
    });

    run(IdentityPurgeStep::Configuration, [&]() -> ErrorOr<void> {
        return remove_directory_if_present(LexicalPath::join(locations.configuration_root, "Identities"sv, identity_id.to_byte_string()).string());
    });

    run(IdentityPurgeStep::Caches, [&]() -> ErrorOr<void> {
        return remove_directory_if_present(LexicalPath::join(locations.cache_root, "Identities"sv, identity_id.to_byte_string()).string());
    });

    run(IdentityPurgeStep::CompactDatabases, [&]() -> ErrorOr<void> {
        if (browsing_database)
            TRY(compact(*browsing_database));
        if (history_database)
            TRY(compact(*history_database));
        return {};
    });
}

}
