/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/HashTable.h>
#include <AK/JsonValue.h>
#include <AK/Time.h>
#include <UI/Qt/DashboardStore.h>

namespace Ladybird {

static constexpr u32 DASHBOARD_SCHEMA_VERSION_1 = 1u;

ErrorOr<Database::MigrationOutcome> DashboardStore::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    auto migrations = to_array<Database::Migration>({
        {
            .version = DASHBOARD_SCHEMA_VERSION_1,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS DashboardMeta (
                    scope_key TEXT PRIMARY KEY,
                    layout_revision INTEGER NOT NULL DEFAULT 0,
                    initialized INTEGER NOT NULL DEFAULT 0,
                    compact_mode INTEGER NOT NULL DEFAULT 0
                );

                CREATE TABLE IF NOT EXISTS DashboardWidgets (
                    scope_key TEXT NOT NULL,
                    instance_id TEXT NOT NULL,
                    widget_type TEXT NOT NULL,
                    config_version INTEGER NOT NULL DEFAULT 1,
                    config_json TEXT NOT NULL,
                    PRIMARY KEY (scope_key, instance_id)
                );

                CREATE TABLE IF NOT EXISTS DashboardPlacements (
                    scope_key TEXT NOT NULL,
                    breakpoint TEXT NOT NULL,
                    instance_id TEXT NOT NULL,
                    col INTEGER NOT NULL,
                    row INTEGER NOT NULL,
                    w INTEGER NOT NULL,
                    h INTEGER NOT NULL,
                    PRIMARY KEY (scope_key, breakpoint, instance_id)
                );

                CREATE TABLE IF NOT EXISTS DashboardPersonalItems (
                    scope_key TEXT NOT NULL,
                    collection TEXT NOT NULL,
                    item_id TEXT NOT NULL,
                    item_revision INTEGER NOT NULL DEFAULT 1,
                    value_json TEXT NOT NULL,
                    updated_at INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY (scope_key, collection, item_id)
                );

                CREATE INDEX IF NOT EXISTS DashboardPersonalItemsByCollection
                ON DashboardPersonalItems(scope_key, collection, updated_at DESC);

                CREATE TABLE IF NOT EXISTS DashboardNetworkGrants (
                    scope_key TEXT NOT NULL,
                    source_id TEXT NOT NULL,
                    kind TEXT NOT NULL,
                    consent_version INTEGER NOT NULL DEFAULT 1,
                    config_json TEXT NOT NULL,
                    cache_json TEXT NOT NULL DEFAULT '{}',
                    expires_at INTEGER NOT NULL DEFAULT 0,
                    last_success_at INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY (scope_key, source_id)
                );
            )#"sv,
        },
    });

    return database.migrate("Dashboard"sv, migrations, mode);
}

ErrorOr<DashboardStore::Statements> DashboardStore::prepare_statements(Database::Database& db)
{
    Statements s;
    s.get_meta = TRY(db.prepare_statement("SELECT layout_revision, initialized, compact_mode FROM DashboardMeta WHERE scope_key = ?;"sv));
    s.upsert_meta = TRY(db.prepare_statement(R"#(
        INSERT INTO DashboardMeta(scope_key, layout_revision, initialized, compact_mode)
        VALUES (?, ?, ?, ?)
        ON CONFLICT(scope_key) DO UPDATE SET
            layout_revision = excluded.layout_revision,
            initialized = excluded.initialized,
            compact_mode = excluded.compact_mode;
    )#"sv));
    s.list_widgets = TRY(db.prepare_statement("SELECT instance_id, widget_type, config_version, config_json FROM DashboardWidgets WHERE scope_key = ? ORDER BY instance_id ASC;"sv));
    s.delete_widgets = TRY(db.prepare_statement("DELETE FROM DashboardWidgets WHERE scope_key = ?;"sv));
    s.insert_widget = TRY(db.prepare_statement("INSERT INTO DashboardWidgets(scope_key, instance_id, widget_type, config_version, config_json) VALUES (?, ?, ?, ?, ?);"sv));
    s.list_placements = TRY(db.prepare_statement("SELECT breakpoint, instance_id, col, row, w, h FROM DashboardPlacements WHERE scope_key = ? ORDER BY breakpoint ASC, row ASC, col ASC, instance_id ASC;"sv));
    s.delete_placements = TRY(db.prepare_statement("DELETE FROM DashboardPlacements WHERE scope_key = ?;"sv));
    s.insert_placement = TRY(db.prepare_statement("INSERT INTO DashboardPlacements(scope_key, breakpoint, instance_id, col, row, w, h) VALUES (?, ?, ?, ?, ?, ?, ?);"sv));
    s.list_items = TRY(db.prepare_statement("SELECT item_id, item_revision, value_json, updated_at FROM DashboardPersonalItems WHERE scope_key = ? AND collection = ? ORDER BY updated_at DESC, item_id ASC LIMIT ?;"sv));
    s.get_item = TRY(db.prepare_statement("SELECT item_id, item_revision, value_json, updated_at FROM DashboardPersonalItems WHERE scope_key = ? AND collection = ? AND item_id = ?;"sv));
    s.count_items = TRY(db.prepare_statement("SELECT COUNT(*) FROM DashboardPersonalItems WHERE scope_key = ? AND collection = ?;"sv));
    s.upsert_item = TRY(db.prepare_statement(R"#(
        INSERT INTO DashboardPersonalItems(scope_key, collection, item_id, item_revision, value_json, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(scope_key, collection, item_id) DO UPDATE SET
            item_revision = excluded.item_revision,
            value_json = excluded.value_json,
            updated_at = excluded.updated_at;
    )#"sv));
    s.delete_item = TRY(db.prepare_statement("DELETE FROM DashboardPersonalItems WHERE scope_key = ? AND collection = ? AND item_id = ?;"sv));
    s.delete_scope_meta = TRY(db.prepare_statement("DELETE FROM DashboardMeta WHERE scope_key = ?;"sv));
    s.delete_scope_items = TRY(db.prepare_statement("DELETE FROM DashboardPersonalItems WHERE scope_key = ?;"sv));
    s.list_grants = TRY(db.prepare_statement("SELECT source_id, kind, consent_version, config_json, cache_json, expires_at, last_success_at FROM DashboardNetworkGrants WHERE scope_key = ? ORDER BY source_id ASC;"sv));
    s.get_grant = TRY(db.prepare_statement("SELECT source_id, kind, consent_version, config_json, cache_json, expires_at, last_success_at FROM DashboardNetworkGrants WHERE scope_key = ? AND source_id = ?;"sv));
    s.upsert_grant = TRY(db.prepare_statement(R"#(
        INSERT INTO DashboardNetworkGrants(scope_key, source_id, kind, consent_version, config_json, cache_json, expires_at, last_success_at)
        VALUES (?, ?, ?, ?, ?, '{}', 0, 0)
        ON CONFLICT(scope_key, source_id) DO UPDATE SET
            kind = excluded.kind,
            consent_version = excluded.consent_version,
            config_json = excluded.config_json;
    )#"sv));
    s.update_grant_cache = TRY(db.prepare_statement("UPDATE DashboardNetworkGrants SET cache_json = ?, expires_at = ?, last_success_at = ? WHERE scope_key = ? AND source_id = ?;"sv));
    s.delete_grant = TRY(db.prepare_statement("DELETE FROM DashboardNetworkGrants WHERE scope_key = ? AND source_id = ?;"sv));
    s.delete_scope_grants = TRY(db.prepare_statement("DELETE FROM DashboardNetworkGrants WHERE scope_key = ?;"sv));
    s.sum_cache_bytes = TRY(db.prepare_statement("SELECT COALESCE(SUM(LENGTH(CAST(cache_json AS BLOB))), 0) FROM DashboardNetworkGrants WHERE scope_key = ?;"sv));
    return s;
}

NonnullOwnPtr<DashboardStore> DashboardStore::create_memory_store(StorageStatus status)
{
    auto db = MUST(Database::Database::create_memory_backed());
    MUST(migrate_schema(*db));
    auto statements = MUST(prepare_statements(*db));
    return adopt_own(*new DashboardStore(move(db), move(statements), status));
}

NonnullOwnPtr<DashboardStore> DashboardStore::open_persistent(ByteString const& data_directory, bool uses_sql_database)
{
    if (!uses_sql_database || data_directory.is_empty())
        return create_memory_store(StorageStatus::FallbackDisabledSQL);

    auto open_result = [&]() -> ErrorOr<Variant<NonnullOwnPtr<DashboardStore>, StorageStatus>> {
        auto db = TRY(Database::Database::create(data_directory, "Dashboard"sv));
        auto check = TRY(migrate_schema(*db, Database::MigrationMode::CheckOnly));
        if (check != Database::MigrationOutcome::Success)
            return StorageStatus::FallbackNewerSchema;
        if (TRY(migrate_schema(*db, Database::MigrationMode::Apply)) != Database::MigrationOutcome::Success)
            return StorageStatus::FallbackNewerSchema;
        auto statements = TRY(prepare_statements(*db));
        return adopt_own(*new DashboardStore(move(db), move(statements), StorageStatus::Persistent));
    }();

    if (open_result.is_error()) {
        dbgln("Unable to open Dashboard database: {}; falling back to session memory", open_result.error());
        return create_memory_store(StorageStatus::FallbackCorrupt);
    }

    auto value = open_result.release_value();
    if (value.has<NonnullOwnPtr<DashboardStore>>())
        return move(value.get<NonnullOwnPtr<DashboardStore>>());

    dbgln("Dashboard database was created by a newer version; preserving file and using session memory");
    return create_memory_store(value.get<StorageStatus>());
}

NonnullOwnPtr<DashboardStore> DashboardStore::create_ephemeral()
{
    return create_memory_store(StorageStatus::SessionOnly);
}

DashboardStore::DashboardStore(NonnullRefPtr<Database::Database> database, Statements statements, StorageStatus status)
    : m_database(move(database))
    , m_statements(move(statements))
    , m_storage_status(status)
{
}

DashboardStore::~DashboardStore() = default;

StringView DashboardStore::storage_status_string() const
{
    switch (m_storage_status) {
    case StorageStatus::Persistent:
        return "saved"sv;
    case StorageStatus::SessionOnly:
        return "session-only"sv;
    case StorageStatus::FallbackDisabledSQL:
        return "sql-disabled"sv;
    case StorageStatus::FallbackCorrupt:
        return "corrupt-fallback"sv;
    case StorageStatus::FallbackNewerSchema:
        return "newer-schema"sv;
    case StorageStatus::WriteFailed:
        return "write-failed"sv;
    }
    return "session-only"sv;
}

i64 DashboardStore::columns_for_breakpoint(StringView breakpoint)
{
    if (breakpoint == "wide"sv)
        return 12;
    if (breakpoint == "medium"sv)
        return 8;
    if (breakpoint == "narrow"sv)
        return 4;
    if (breakpoint == "compact"sv)
        return 2;
    return 0;
}

ErrorOr<void> DashboardStore::validate_placements(StringView breakpoint, Vector<Placement> const& placements, HashTable<String> const& valid_instance_ids)
{
    auto cols = columns_for_breakpoint(breakpoint);
    if (cols <= 0)
        return Error::from_string_literal("Unknown breakpoint");

    HashTable<String> seen;
    for (size_t i = 0; i < placements.size(); ++i) {
        auto const& p = placements[i];
        if (!valid_instance_ids.contains(p.instance_id))
            return Error::from_string_literal("Placement references unknown widget instance");
        if (seen.contains(p.instance_id))
            return Error::from_string_literal("Duplicate placement for widget instance");
        seen.set(p.instance_id);

        if (p.col < 0 || p.row < 0 || p.w < 1 || p.h < 1)
            return Error::from_string_literal("Placement dimensions out of bounds");
        if (p.col + p.w > cols)
            return Error::from_string_literal("Placement exceeds breakpoint column count");
        if (p.row + p.h > MAX_GRID_ROWS)
            return Error::from_string_literal("Placement exceeds maximum row count");

        for (size_t j = 0; j < i; ++j) {
            auto const& other = placements[j];
            bool overlaps = p.col < other.col + other.w
                && p.col + p.w > other.col
                && p.row < other.row + other.h
                && p.row + p.h > other.row;
            if (overlaps)
                return Error::from_string_literal("Placements overlap");
        }
    }
    return {};
}

DashboardStore::LayoutSnapshot DashboardStore::build_starter_layout(bool is_ephemeral_starter) const
{
    LayoutSnapshot snapshot;
    snapshot.layout_revision = 0;
    snapshot.initialized = false;
    snapshot.compact_mode = false;

    Vector<Placement> wide;
    if (is_ephemeral_starter) {
        JsonObject clock_cfg;
        clock_cfg.set("use24Hour"sv, false);
        clock_cfg.set("showSeconds"sv, false);
        clock_cfg.set("zones"sv, JsonArray {});
        snapshot.widgets.append({ "starter-clock"_string, "world-clock"_string, 1, move(clock_cfg) });
        wide.append({ "starter-clock"_string, 0, 0, 3, 2 });

        JsonObject calc_cfg;
        calc_cfg.set("remBase"sv, 16);
        calc_cfg.set("saveHistory"sv, false);
        snapshot.widgets.append({ "starter-calc"_string, "calculator"_string, 1, move(calc_cfg) });
        wide.append({ "starter-calc"_string, 3, 0, 3, 2 });
    } else {
        JsonObject clock_cfg;
        clock_cfg.set("use24Hour"sv, false);
        clock_cfg.set("showSeconds"sv, false);
        clock_cfg.set("zones"sv, JsonArray {});
        snapshot.widgets.append({ "starter-clock"_string, "world-clock"_string, 1, move(clock_cfg) });
        wide.append({ "starter-clock"_string, 0, 0, 3, 2 });

        JsonObject tasks_cfg;
        tasks_cfg.set("listFilter"sv, ""_string);
        snapshot.widgets.append({ "starter-tasks"_string, "quick-tasks"_string, 1, move(tasks_cfg) });
        wide.append({ "starter-tasks"_string, 3, 0, 4, 3 });

        JsonObject pad_cfg;
        pad_cfg.set("noteId"sv, "default-note"_string);
        pad_cfg.set("preview"sv, false);
        snapshot.widgets.append({ "starter-scratchpad"_string, "scratchpad"_string, 1, move(pad_cfg) });
        wide.append({ "starter-scratchpad"_string, 7, 0, 4, 3 });
    }

    snapshot.placements.set("wide"_string, move(wide));
    return snapshot;
}

DashboardStore::LayoutSnapshot DashboardStore::read_layout_snapshot(String const& scope_key)
{
    LayoutSnapshot snapshot;
    bool has_meta = false;
    (void)m_database->try_execute_statement(
        m_statements.get_meta,
        [&](auto stmt) -> ErrorOr<void> {
            has_meta = true;
            snapshot.layout_revision = m_database->result_column<i64>(stmt, 0);
            snapshot.initialized = m_database->result_column<i64>(stmt, 1) != 0;
            snapshot.compact_mode = m_database->result_column<i64>(stmt, 2) != 0;
            return {};
        },
        scope_key);

    if (!has_meta)
        return snapshot;

    (void)m_database->try_execute_statement(
        m_statements.list_widgets,
        [&](auto stmt) -> ErrorOr<void> {
            auto instance_id = m_database->result_column<String>(stmt, 0);
            auto widget_type = m_database->result_column<String>(stmt, 1);
            auto config_version = m_database->result_column<i64>(stmt, 2);
            auto config_str = m_database->result_column<String>(stmt, 3);
            JsonObject config;
            if (auto parsed = JsonValue::from_string(config_str); !parsed.is_error() && parsed.value().is_object())
                config = parsed.value().as_object();
            snapshot.widgets.append({ move(instance_id), move(widget_type), config_version, move(config) });
            return {};
        },
        scope_key);

    (void)m_database->try_execute_statement(
        m_statements.list_placements,
        [&](auto stmt) -> ErrorOr<void> {
            auto bp = m_database->result_column<String>(stmt, 0);
            Placement p {
                .instance_id = m_database->result_column<String>(stmt, 1),
                .col = m_database->result_column<i64>(stmt, 2),
                .row = m_database->result_column<i64>(stmt, 3),
                .w = m_database->result_column<i64>(stmt, 4),
                .h = m_database->result_column<i64>(stmt, 5),
            };
            snapshot.placements.ensure(bp).append(move(p));
            return {};
        },
        scope_key);

    return snapshot;
}

DashboardStore::LayoutSnapshot DashboardStore::load_or_initialize_layout(String const& scope_key, bool is_ephemeral_starter)
{
    auto snapshot = read_layout_snapshot(scope_key);
    if (snapshot.initialized)
        return snapshot;
    return build_starter_layout(is_ephemeral_starter);
}

void DashboardStore::record_committed_op(String const& scope_key, String const& op_id, CommitResult const& result)
{
    if (op_id.is_empty())
        return;
    auto& entries = m_recent_ops.ensure(scope_key);
    if (entries.size() >= MAX_OP_DEDUP_ENTRIES)
        entries.remove(0);
    entries.append({ op_id, result });
}

DashboardStore::CommitResult DashboardStore::commit_layout(
    String const& scope_key,
    String const& op_id,
    i64 expected_revision,
    bool compact_mode,
    Vector<WidgetInstance> const& widgets,
    HashMap<String, Vector<Placement>> const& placements)
{
    if (!op_id.is_empty()) {
        if (auto it = m_recent_ops.find(scope_key); it != m_recent_ops.end()) {
            for (auto const& cached : it->value) {
                if (cached.op_id == op_id) {
                    auto copy = cached.result;
                    copy.duplicate = true;
                    return copy;
                }
            }
        }
    }

    if (widgets.size() > MAX_CARDS) {
        return CommitResult {
            .ok = false,
            .conflict = false,
            .duplicate = false,
            .error = MUST(String::formatted("A dashboard can hold at most {} cards.", MAX_CARDS)),
            .snapshot = read_layout_snapshot(scope_key),
        };
    }

    HashTable<String> valid_ids;
    for (auto const& w : widgets) {
        if (w.instance_id.is_empty() || w.instance_id.bytes().size() > 128 || w.widget_type.is_empty() || w.widget_type.bytes().size() > 64) {
            return CommitResult {
                .ok = false,
                .conflict = false,
                .duplicate = false,
                .error = "Invalid widget identifier."_string,
                .snapshot = read_layout_snapshot(scope_key),
            };
        }
        if (valid_ids.contains(w.instance_id)) {
            return CommitResult {
                .ok = false,
                .conflict = false,
                .duplicate = false,
                .error = "Duplicate widget instance identifier."_string,
                .snapshot = read_layout_snapshot(scope_key),
            };
        }
        auto serialized_config = JsonValue { w.config }.serialized();
        if (serialized_config.bytes().size() > MAX_ITEM_JSON_BYTES) {
            return CommitResult {
                .ok = false,
                .conflict = false,
                .duplicate = false,
                .error = "Widget configuration exceeds maximum size."_string,
                .snapshot = read_layout_snapshot(scope_key),
            };
        }
        valid_ids.set(w.instance_id);
    }

    // Require canonical wide placements whenever any widget exists.
    if (!widgets.is_empty() && !placements.contains("wide"_string)) {
        return CommitResult {
            .ok = false,
            .conflict = false,
            .duplicate = false,
            .error = "Canonical wide placements are required."_string,
            .snapshot = read_layout_snapshot(scope_key),
        };
    }

    for (auto const& [bp, bp_placements] : placements) {
        if (auto err = validate_placements(bp, bp_placements, valid_ids); err.is_error()) {
            return CommitResult {
                .ok = false,
                .conflict = false,
                .duplicate = false,
                .error = MUST(String::formatted("{}", err.error())),
                .snapshot = read_layout_snapshot(scope_key),
            };
        }
    }

    auto current = read_layout_snapshot(scope_key);
    if (current.layout_revision != expected_revision) {
        return CommitResult {
            .ok = false,
            .conflict = true,
            .duplicate = false,
            .error = "Layout changed in another tab."_string,
            .snapshot = move(current),
        };
    }

    i64 next_revision = current.layout_revision + 1;
    auto tx_result = m_database->transaction([&]() -> ErrorOr<void> {
        TRY(m_database->try_execute_statement(m_statements.upsert_meta, {}, scope_key, next_revision, i64 { 1 }, compact_mode ? i64 { 1 } : i64 { 0 }));
        TRY(m_database->try_execute_statement(m_statements.delete_widgets, {}, scope_key));
        TRY(m_database->try_execute_statement(m_statements.delete_placements, {}, scope_key));

        for (auto const& w : widgets) {
            auto config_str = JsonValue { w.config }.serialized();
            TRY(m_database->try_execute_statement(m_statements.insert_widget, {}, scope_key, w.instance_id, w.widget_type, w.config_version, config_str));
        }

        for (auto const& [bp, bp_placements] : placements) {
            for (auto const& p : bp_placements) {
                TRY(m_database->try_execute_statement(m_statements.insert_placement, {}, scope_key, bp, p.instance_id, p.col, p.row, p.w, p.h));
            }
        }
        return {};
    });

    if (tx_result.is_error()) {
        dbgln("DashboardStore commit failed: {}", tx_result.error());
        m_storage_status = StorageStatus::WriteFailed;
        return CommitResult {
            .ok = false,
            .conflict = false,
            .duplicate = false,
            .error = "Couldn’t save dashboard layout."_string,
            .snapshot = read_layout_snapshot(scope_key),
        };
    }

    CommitResult result {
        .ok = true,
        .conflict = false,
        .duplicate = false,
        .error = {},
        .snapshot = read_layout_snapshot(scope_key),
    };
    record_committed_op(scope_key, op_id, result);
    return result;
}

Vector<DashboardStore::PersonalItem> DashboardStore::list_items(String const& scope_key, String const& collection, size_t limit)
{
    Vector<PersonalItem> items;
    auto capped_limit = static_cast<i64>(min(limit, MAX_COLLECTION_ITEMS));
    (void)m_database->try_execute_statement(
        m_statements.list_items,
        [&](auto stmt) -> ErrorOr<void> {
            auto item_id = m_database->result_column<String>(stmt, 0);
            auto rev = m_database->result_column<i64>(stmt, 1);
            auto val_str = m_database->result_column<String>(stmt, 2);
            auto updated_at = m_database->result_column<i64>(stmt, 3);
            JsonObject val;
            if (auto parsed = JsonValue::from_string(val_str); !parsed.is_error() && parsed.value().is_object())
                val = parsed.value().as_object();
            items.append({ move(item_id), rev, move(val), updated_at });
            return {};
        },
        scope_key,
        collection,
        capped_limit);
    return items;
}

Optional<DashboardStore::PersonalItem> DashboardStore::get_item(String const& scope_key, String const& collection, String const& item_id)
{
    Optional<PersonalItem> found;
    (void)m_database->try_execute_statement(
        m_statements.get_item,
        [&](auto stmt) -> ErrorOr<void> {
            auto id = m_database->result_column<String>(stmt, 0);
            auto rev = m_database->result_column<i64>(stmt, 1);
            auto val_str = m_database->result_column<String>(stmt, 2);
            auto updated_at = m_database->result_column<i64>(stmt, 3);
            JsonObject val;
            if (auto parsed = JsonValue::from_string(val_str); !parsed.is_error() && parsed.value().is_object())
                val = parsed.value().as_object();
            found = PersonalItem { move(id), rev, move(val), updated_at };
            return {};
        },
        scope_key,
        collection,
        item_id);
    return found;
}

DashboardStore::ItemSaveResult DashboardStore::save_item(
    String const& scope_key,
    String const& collection,
    String const& item_id,
    Optional<i64> expected_revision,
    JsonObject const& value)
{
    if (collection.is_empty() || collection.bytes().size() > 64 || item_id.is_empty() || item_id.bytes().size() > 128)
        return { .ok = false, .conflict = false, .error = "Invalid item identifier."_string, .revision = 0, .current_value = {} };

    auto serialized = JsonValue { value }.serialized();
    size_t max_bytes = (collection == "notes"sv) ? (MAX_NOTE_BYTES + 4096) : MAX_ITEM_JSON_BYTES;
    if (serialized.bytes().size() > max_bytes)
        return { .ok = false, .conflict = false, .error = "Item exceeds maximum allowed size."_string, .revision = 0, .current_value = {} };

    if (collection == "notes"sv) {
        if (auto body = value.get_string("content"sv); body.has_value() && body->bytes().size() > MAX_NOTE_BYTES)
            return { .ok = false, .conflict = false, .error = "Notes are limited to 64 KiB."_string, .revision = 0, .current_value = {} };
    }

    auto existing = get_item(scope_key, collection, item_id);
    if (!existing.has_value()) {
        i64 existing_count = 0;
        (void)m_database->try_execute_statement(
            m_statements.count_items,
            [&](auto stmt) -> ErrorOr<void> {
                existing_count = m_database->result_column<i64>(stmt, 0);
                return {};
            },
            scope_key,
            collection);
        if (static_cast<size_t>(existing_count) >= MAX_COLLECTION_ITEMS)
            return { .ok = false, .conflict = false, .error = "Collection limit reached."_string, .revision = 0, .current_value = {} };
    }

    if (expected_revision.has_value()) {
        i64 current_rev = existing.has_value() ? existing->revision : 0;
        if (current_rev != *expected_revision) {
            return {
                .ok = false,
                .conflict = true,
                .error = "Item was edited in another tab."_string,
                .revision = current_rev,
                .current_value = existing.has_value() ? Optional<JsonObject> { existing->value } : Optional<JsonObject> {},
            };
        }
    }

    i64 next_rev = existing.has_value() ? (existing->revision + 1) : 1;
    i64 now_ms = UnixDateTime::now().milliseconds_since_epoch();
    auto err = m_database->try_execute_statement(m_statements.upsert_item, {}, scope_key, collection, item_id, next_rev, serialized, now_ms);
    if (err.is_error()) {
        m_storage_status = StorageStatus::WriteFailed;
        return { .ok = false, .conflict = false, .error = "Couldn’t save item."_string, .revision = 0, .current_value = {} };
    }

    return { .ok = true, .conflict = false, .error = {}, .revision = next_rev, .current_value = {} };
}

bool DashboardStore::delete_item(String const& scope_key, String const& collection, String const& item_id)
{
    return !m_database->try_execute_statement(m_statements.delete_item, {}, scope_key, collection, item_id).is_error();
}

size_t DashboardStore::delete_items_by_ids(String const& scope_key, String const& collection, Vector<String> const& item_ids)
{
    size_t deleted = 0;
    auto tx = m_database->transaction([&]() -> ErrorOr<void> {
        for (auto const& id : item_ids) {
            TRY(m_database->try_execute_statement(m_statements.delete_item, {}, scope_key, collection, id));
            ++deleted;
        }
        return {};
    });
    return tx.is_error() ? 0 : deleted;
}

Vector<DashboardStore::NetworkGrant> DashboardStore::list_network_grants(String const& scope_key)
{
    Vector<NetworkGrant> grants;
    (void)m_database->try_execute_statement(
        m_statements.list_grants,
        [&](auto stmt) -> ErrorOr<void> {
            NetworkGrant g;
            g.source_id = m_database->result_column<String>(stmt, 0);
            g.kind = m_database->result_column<String>(stmt, 1);
            g.consent_version = m_database->result_column<i64>(stmt, 2);
            auto cfg_str = m_database->result_column<String>(stmt, 3);
            if (auto p = JsonValue::from_string(cfg_str); !p.is_error() && p.value().is_object())
                g.config = p.value().as_object();
            auto cache_str = m_database->result_column<String>(stmt, 4);
            if (auto p = JsonValue::from_string(cache_str); !p.is_error() && p.value().is_object())
                g.cache = p.value().as_object();
            g.expires_at = m_database->result_column<i64>(stmt, 5);
            g.last_success_at = m_database->result_column<i64>(stmt, 6);
            grants.append(move(g));
            return {};
        },
        scope_key);
    return grants;
}

Optional<DashboardStore::NetworkGrant> DashboardStore::get_network_grant(String const& scope_key, String const& source_id)
{
    Optional<NetworkGrant> found;
    (void)m_database->try_execute_statement(
        m_statements.get_grant,
        [&](auto stmt) -> ErrorOr<void> {
            NetworkGrant g;
            g.source_id = m_database->result_column<String>(stmt, 0);
            g.kind = m_database->result_column<String>(stmt, 1);
            g.consent_version = m_database->result_column<i64>(stmt, 2);
            auto cfg_str = m_database->result_column<String>(stmt, 3);
            if (auto p = JsonValue::from_string(cfg_str); !p.is_error() && p.value().is_object())
                g.config = p.value().as_object();
            auto cache_str = m_database->result_column<String>(stmt, 4);
            if (auto p = JsonValue::from_string(cache_str); !p.is_error() && p.value().is_object())
                g.cache = p.value().as_object();
            g.expires_at = m_database->result_column<i64>(stmt, 5);
            g.last_success_at = m_database->result_column<i64>(stmt, 6);
            found = move(g);
            return {};
        },
        scope_key,
        source_id);
    return found;
}

bool DashboardStore::upsert_network_grant(String const& scope_key, String const& source_id, String const& kind, i64 consent_version, JsonObject const& config)
{
    if (source_id.is_empty() || source_id.bytes().size() > 512)
        return false;
    auto cfg_str = JsonValue { config }.serialized();
    return !m_database->try_execute_statement(m_statements.upsert_grant, {}, scope_key, source_id, kind, consent_version, cfg_str).is_error();
}

bool DashboardStore::update_network_cache(String const& scope_key, String const& source_id, JsonObject const& cache, i64 expires_at, i64 last_success_at)
{
    auto cache_str = JsonValue { cache }.serialized();
    if (cache_str.bytes().size() > 1024 * 1024)
        return false;

    i64 current_bytes = 0;
    (void)m_database->try_execute_statement(
        m_statements.sum_cache_bytes,
        [&](auto stmt) -> ErrorOr<void> {
            current_bytes = m_database->result_column<i64>(stmt, 0);
            return {};
        },
        scope_key);
    if (static_cast<size_t>(current_bytes) + cache_str.bytes().size() > MAX_SCOPE_CACHE_BYTES)
        return false;

    return !m_database->try_execute_statement(m_statements.update_grant_cache, {}, cache_str, expires_at, last_success_at, scope_key, source_id).is_error();
}

bool DashboardStore::revoke_network_grant(String const& scope_key, String const& source_id)
{
    return !m_database->try_execute_statement(m_statements.delete_grant, {}, scope_key, source_id).is_error();
}

bool DashboardStore::delete_scope_data(String const& scope_key)
{
    m_recent_ops.remove(scope_key);
    auto tx = m_database->transaction([&]() -> ErrorOr<void> {
        TRY(m_database->try_execute_statement(m_statements.delete_scope_meta, {}, scope_key));
        TRY(m_database->try_execute_statement(m_statements.delete_widgets, {}, scope_key));
        TRY(m_database->try_execute_statement(m_statements.delete_placements, {}, scope_key));
        TRY(m_database->try_execute_statement(m_statements.delete_scope_items, {}, scope_key));
        TRY(m_database->try_execute_statement(m_statements.delete_scope_grants, {}, scope_key));
        return {};
    });
    return !tx.is_error();
}

JsonObject DashboardStore::serialize_layout_snapshot(LayoutSnapshot const& snapshot)
{
    JsonObject out;
    out.set("layoutRevision"sv, snapshot.layout_revision);
    out.set("initialized"sv, snapshot.initialized);
    out.set("compactMode"sv, snapshot.compact_mode);

    JsonArray widgets_arr;
    for (auto const& w : snapshot.widgets) {
        JsonObject obj;
        obj.set("instanceId"sv, w.instance_id);
        obj.set("widgetType"sv, w.widget_type);
        obj.set("configVersion"sv, w.config_version);
        obj.set("config"sv, w.config);
        widgets_arr.must_append(move(obj));
    }
    out.set("widgets"sv, move(widgets_arr));

    JsonObject placements_obj;
    for (auto const& [bp, list] : snapshot.placements) {
        JsonArray arr;
        for (auto const& p : list) {
            JsonObject po;
            po.set("instanceId"sv, p.instance_id);
            po.set("col"sv, p.col);
            po.set("row"sv, p.row);
            po.set("w"sv, p.w);
            po.set("h"sv, p.h);
            arr.must_append(move(po));
        }
        placements_obj.set(bp, move(arr));
    }
    out.set("placements"sv, move(placements_obj));
    return out;
}

}
