/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/HashTable.h>
#include <LibDatabase/Database.h>
#include <LibWebView/Extensions/ExtensionStorage.h>

namespace WebView::Extensions {

ErrorOr<Database::MigrationOutcome> ExtensionStorage::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    auto migrations = to_array<Database::Migration>({
        {
            .version = 1u,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS ExtensionLocalStorage (
                    extension_id TEXT NOT NULL,
                    storage_key TEXT NOT NULL,
                    value_json TEXT NOT NULL,
                    PRIMARY KEY(extension_id, storage_key)
                );
            )#"sv,
        },
        {
            .version = 2u,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS ExtensionSyncStorage (
                    extension_id TEXT NOT NULL,
                    storage_key TEXT NOT NULL,
                    value_json TEXT NOT NULL,
                    PRIMARY KEY(extension_id, storage_key)
                );
            )#"sv,
        },
    });

    return database.migrate("ExtensionLocalStorage"sv, migrations, mode);
}

ErrorOr<NonnullOwnPtr<ExtensionStorage>> ExtensionStorage::create(Database::Database& database, Area area)
{
    auto table = area == Area::Sync ? "ExtensionSyncStorage"sv : "ExtensionLocalStorage"sv;
    Statements statements {};
    statements.get_item = TRY(database.prepare_statement(ByteString::formatted("SELECT value_json FROM {} WHERE extension_id = ? AND storage_key = ?;", table)));
    statements.get_all = TRY(database.prepare_statement(ByteString::formatted("SELECT storage_key, value_json FROM {} WHERE extension_id = ?;", table)));
    statements.get_size = TRY(database.prepare_statement(ByteString::formatted("SELECT COALESCE(SUM(length(CAST(storage_key AS BLOB)) + length(CAST(value_json AS BLOB))), 0) FROM {} WHERE extension_id = ?;", table)));
    statements.set_item = TRY(database.prepare_statement(ByteString::formatted("INSERT OR REPLACE INTO {} (extension_id, storage_key, value_json) VALUES (?, ?, ?);", table)));
    statements.delete_item = TRY(database.prepare_statement(ByteString::formatted("DELETE FROM {} WHERE extension_id = ? AND storage_key = ?;", table)));
    statements.clear_extension = TRY(database.prepare_statement(ByteString::formatted("DELETE FROM {} WHERE extension_id = ?;", table)));

    return adopt_own(*new ExtensionStorage(NonnullRefPtr { database }, statements));
}

NonnullOwnPtr<ExtensionStorage> ExtensionStorage::create()
{
    return adopt_own(*new ExtensionStorage({}, {}));
}

ExtensionStorage::ExtensionStorage(Optional<NonnullRefPtr<Database::Database>> database, Statements statements)
    : m_database(database.has_value() ? database.release_value().ptr() : nullptr)
    , m_statements(statements)
{
}

ExtensionStorage::~ExtensionStorage() = default;

Optional<String> ExtensionStorage::read_raw(StringView extension_id, StringView key) const
{
    return MUST(try_read_raw(extension_id, key));
}

ErrorOr<Optional<String>> ExtensionStorage::try_read_raw(StringView extension_id, StringView key) const
{
    if (!m_database) {
        if (auto ext = m_memory_store.get(extension_id); ext.has_value()) {
            if (auto val = ext->get(key); val.has_value())
                return *val;
        }
        return Optional<String> {};
    }

    Optional<String> result;
    TRY(m_database->try_execute_statement(
        m_statements.get_item,
        [&](Database::StatementID stmt) -> ErrorOr<void> {
            result = m_database->result_column<String>(stmt, 0);
            return {};
        },
        MUST(String::from_utf8(extension_id)),
        MUST(String::from_utf8(key))));
    return result;
}

HashMap<String, String> ExtensionStorage::read_all_raw(StringView extension_id) const
{
    if (!m_database)
        return m_memory_store.get(extension_id).value_or({});

    HashMap<String, String> out;
    m_database->execute_statement(
        m_statements.get_all,
        [&](Database::StatementID stmt) -> ErrorOr<void> {
            auto k = m_database->result_column<String>(stmt, 0);
            auto v = m_database->result_column<String>(stmt, 1);
            out.set(move(k), move(v));
            return {};
        },
        MUST(String::from_utf8(extension_id)));
    return out;
}

ErrorOr<void> ExtensionStorage::write_raw(StringView extension_id, StringView key, StringView json_value)
{
    if (!m_database) {
        auto& bucket = m_memory_store.ensure(MUST(String::from_utf8(extension_id)));
        bucket.set(MUST(String::from_utf8(key)), MUST(String::from_utf8(json_value)));
        return {};
    }

    return m_database->try_execute_statement(
        m_statements.set_item,
        [](Database::StatementID) -> ErrorOr<void> { return {}; },
        MUST(String::from_utf8(extension_id)),
        MUST(String::from_utf8(key)),
        MUST(String::from_utf8(json_value)));
}

void ExtensionStorage::erase_raw(StringView extension_id, StringView key)
{
    if (!m_database) {
        if (auto it = m_memory_store.find(extension_id); it != m_memory_store.end())
            it->value.remove(key);
        return;
    }

    m_database->execute_statement(
        m_statements.delete_item,
        [](Database::StatementID) -> ErrorOr<void> { return {}; },
        MUST(String::from_utf8(extension_id)),
        MUST(String::from_utf8(key)));
}

void ExtensionStorage::erase_all_raw(StringView extension_id)
{
    if (!m_database) {
        m_memory_store.remove(extension_id);
        return;
    }

    m_database->execute_statement(
        m_statements.clear_extension,
        [](Database::StatementID) -> ErrorOr<void> { return {}; },
        MUST(String::from_utf8(extension_id)));
}

static JsonValue parse_stored_json(StringView raw)
{
    auto parsed = JsonValue::from_string(raw);
    if (parsed.is_error())
        return JsonValue {};
    return parsed.release_value();
}

JsonObject ExtensionStorage::get(StringView extension_id, JsonValue const& keys_spec) const
{
    JsonObject result;

    if (keys_spec.is_null()) {
        auto all = read_all_raw(extension_id);
        for (auto const& [k, v] : all)
            result.set(k, parse_stored_json(v));
        return result;
    }

    if (keys_spec.is_string()) {
        auto key = keys_spec.as_string();
        if (auto raw = read_raw(extension_id, key); raw.has_value())
            result.set(key, parse_stored_json(*raw));
        return result;
    }

    if (keys_spec.is_array()) {
        for (auto const& item : keys_spec.as_array().values()) {
            if (!item.is_string())
                continue;
            auto key = item.as_string();
            if (auto raw = read_raw(extension_id, key); raw.has_value())
                result.set(key, parse_stored_json(*raw));
        }
        return result;
    }

    if (keys_spec.is_object()) {
        keys_spec.as_object().for_each_member([&](String const& key, JsonValue const& default_value) {
            if (auto raw = read_raw(extension_id, key); raw.has_value())
                result.set(key, parse_stored_json(*raw));
            else
                result.set(key, default_value);
        });
        return result;
    }

    return result;
}

ErrorOr<u64> ExtensionStorage::get_bytes_in_use(StringView extension_id, JsonValue const& keys_spec) const
{
    auto item_size = [&](StringView key) -> ErrorOr<u64> {
        auto value = TRY(try_read_raw(extension_id, key));
        return value.has_value() ? key.length() + value->byte_count() : 0;
    };

    if (keys_spec.is_string())
        return item_size(keys_spec.as_string());

    u64 size = 0;
    if (keys_spec.is_array()) {
        HashTable<String> seen;
        for (auto const& entry : keys_spec.as_array().values()) {
            if (!entry.is_string())
                return Error::from_string_literal("Storage keys must be strings");
            if (seen.set(entry.as_string()) == AK::HashSetResult::InsertedNewEntry)
                size += TRY(item_size(entry.as_string()));
        }
        return size;
    }

    if (!keys_spec.is_null())
        return Error::from_string_literal("Expected a storage key or an array of keys");

    if (m_database) {
        TRY(m_database->try_execute_statement(
            m_statements.get_size,
            [&](Database::StatementID stmt) -> ErrorOr<void> {
                size = m_database->result_column<i64>(stmt, 0);
                return {};
            },
            MUST(String::from_utf8(extension_id))));
    } else if (auto bucket = m_memory_store.get(extension_id); bucket.has_value()) {
        for (auto const& [key, value] : *bucket)
            size += key.byte_count() + value.byte_count();
    }
    return size;
}

ErrorOr<JsonObject> ExtensionStorage::set(StringView extension_id, JsonObject const& items, Quota quota)
{
    JsonObject changes;
    if (items.is_empty())
        return changes;

    auto apply_changes = [&]() -> ErrorOr<void> {
        bool limited = quota != Quota::Unlimited;
        auto previous_size = limited ? TRY(get_bytes_in_use(extension_id)) : 0;
        auto item_count = quota == Quota::Sync ? read_all_raw(extension_id).size() : 0;
        auto next_size = previous_size;
        struct PendingWrite {
            String key;
            String value;
        };
        Vector<PendingWrite> writes;
        TRY(items.try_for_each_member([&](String const& key, JsonValue const& new_value) -> ErrorOr<void> {
            auto new_raw = new_value.serialized();
            auto old_raw = TRY(try_read_raw(extension_id, key));
            if (old_raw == new_raw)
                return {};

            // Firefox counts a storage.sync item as its key plus its JSON text.
            if (quota == Quota::Sync && key.byte_count() + new_raw.byte_count() > sync_quota_bytes_per_item)
                return Error::from_string_literal("QuotaExceededError: storage.sync API call exceeded its quota limitations.");
            if (limited) {
                if (old_raw.has_value()) {
                    next_size -= old_raw->byte_count();
                } else {
                    next_size += key.byte_count();
                    ++item_count;
                }
                next_size += new_raw.byte_count();
            }

            JsonObject change;
            if (old_raw.has_value())
                change.set("oldValue"sv, parse_stored_json(*old_raw));
            change.set("newValue"sv, new_value);
            changes.set(key, move(change));
            writes.append({ key, move(new_raw) });
            return {};
        }));
        // Existing over-quota data remains readable and can be reduced after a permission is lost.
        if (quota == Quota::Sync) {
            if ((next_size > sync_quota_bytes && next_size > previous_size) || item_count > sync_max_items)
                return Error::from_string_literal("QuotaExceededError: storage.sync API call exceeded its quota limitations.");
        } else if (next_size > default_quota_bytes && next_size > previous_size) {
            return Error::from_string_literal("Extension storage quota exceeded");
        }

        for (auto const& write : writes)
            TRY(write_raw(extension_id, write.key, write.value));
        return {};
    };

    if (m_database)
        TRY(m_database->transaction(move(apply_changes)));
    else
        TRY(apply_changes());
    return changes;
}

JsonObject ExtensionStorage::remove(StringView extension_id, Vector<String> const& keys)
{
    JsonObject changes;
    for (auto const& key : keys) {
        auto old_raw = read_raw(extension_id, key);
        if (!old_raw.has_value())
            continue;
        erase_raw(extension_id, key);

        JsonObject change;
        change.set("oldValue"sv, parse_stored_json(*old_raw));
        changes.set(key, move(change));
    }
    return changes;
}

JsonObject ExtensionStorage::clear(StringView extension_id)
{
    auto all = read_all_raw(extension_id);
    if (all.is_empty())
        return {};

    erase_all_raw(extension_id);

    JsonObject changes;
    for (auto const& [key, old_raw] : all) {
        JsonObject change;
        change.set("oldValue"sv, parse_stored_json(old_raw));
        changes.set(key, move(change));
    }
    return changes;
}

void ExtensionStorage::delete_extension_data(StringView extension_id)
{
    erase_all_raw(extension_id);
}

}
