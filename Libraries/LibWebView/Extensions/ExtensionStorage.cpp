/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibDatabase/Database.h>
#include <LibWebView/Extensions/ExtensionStorage.h>

namespace WebView::Extensions {

static constexpr u32 EXTENSION_STORAGE_SCHEMA_VERSION = 1u;

ErrorOr<Database::MigrationOutcome> ExtensionStorage::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    auto migrations = to_array<Database::Migration>({
        {
            .version = EXTENSION_STORAGE_SCHEMA_VERSION,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS ExtensionLocalStorage (
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

ErrorOr<NonnullOwnPtr<ExtensionStorage>> ExtensionStorage::create(Database::Database& database)
{
    Statements statements {};
    statements.get_item = TRY(database.prepare_statement("SELECT value_json FROM ExtensionLocalStorage WHERE extension_id = ? AND storage_key = ?;"sv));
    statements.get_all = TRY(database.prepare_statement("SELECT storage_key, value_json FROM ExtensionLocalStorage WHERE extension_id = ?;"sv));
    statements.set_item = TRY(database.prepare_statement("INSERT OR REPLACE INTO ExtensionLocalStorage (extension_id, storage_key, value_json) VALUES (?, ?, ?);"sv));
    statements.delete_item = TRY(database.prepare_statement("DELETE FROM ExtensionLocalStorage WHERE extension_id = ? AND storage_key = ?;"sv));
    statements.clear_extension = TRY(database.prepare_statement("DELETE FROM ExtensionLocalStorage WHERE extension_id = ?;"sv));

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
    if (!m_database) {
        if (auto ext = m_memory_store.get(extension_id); ext.has_value()) {
            if (auto val = ext->get(key); val.has_value())
                return *val;
        }
        return {};
    }

    Optional<String> result;
    m_database->execute_statement(
        m_statements.get_item,
        [&](Database::StatementID stmt) -> ErrorOr<void> {
            result = m_database->result_column<String>(stmt, 0);
            return {};
        },
        MUST(String::from_utf8(extension_id)),
        MUST(String::from_utf8(key)));
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

void ExtensionStorage::write_raw(StringView extension_id, StringView key, StringView json_value)
{
    if (!m_database) {
        auto& bucket = m_memory_store.ensure(MUST(String::from_utf8(extension_id)));
        bucket.set(MUST(String::from_utf8(key)), MUST(String::from_utf8(json_value)));
        return;
    }

    m_database->execute_statement(
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

JsonObject ExtensionStorage::set(StringView extension_id, JsonObject const& items)
{
    JsonObject changes;
    items.for_each_member([&](String const& key, JsonValue const& new_val) {
        auto new_raw = new_val.serialized();
        auto old_raw = read_raw(extension_id, key);
        if (old_raw.has_value() && *old_raw == new_raw)
            return;

        write_raw(extension_id, key, new_raw);

        JsonObject change;
        if (old_raw.has_value())
            change.set("oldValue"sv, parse_stored_json(*old_raw));
        change.set("newValue"sv, new_val);
        changes.set(key, move(change));
    });
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
