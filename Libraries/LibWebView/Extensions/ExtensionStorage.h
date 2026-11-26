/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibDatabase/Database.h>
#include <LibDatabase/Forward.h>
#include <LibWebView/Export.h>

namespace WebView::Extensions {

// Per-extension storage.local backed by SQLite (via LibDatabase) with an in-memory fallback.
// Each changed key returns { oldValue?, newValue? } to drive browser.storage.onChanged.
class WEBVIEW_API ExtensionStorage {
    AK_MAKE_NONCOPYABLE(ExtensionStorage);

public:
    AK_ALLOC_WITH_KMALLOC;

    enum class Quota {
        Limited,
        Unlimited,
    };
    static constexpr u64 default_quota_bytes = 10 * MiB;

    static ErrorOr<Database::MigrationOutcome> migrate_schema(Database::Database&, Database::MigrationMode = Database::MigrationMode::Apply);
    static ErrorOr<NonnullOwnPtr<ExtensionStorage>> create(Database::Database&);
    static NonnullOwnPtr<ExtensionStorage> create();

    ~ExtensionStorage();

    // keys_spec may be null/undefined (return all items), a string (single key),
    // an array of strings, or a plain object mapping key -> defaultValue.
    JsonObject get(StringView extension_id, JsonValue const& keys_spec) const;

    // Counts UTF-8 key and JSON value bytes for one key, multiple keys, or the entire area.
    ErrorOr<u64> get_bytes_in_use(StringView extension_id, JsonValue const& keys_spec = {}) const;

    // Sets every property of `items` for `extension_id`. Returns a changes map { key: { oldValue?, newValue } }.
    ErrorOr<JsonObject> set(StringView extension_id, JsonObject const& items, Quota = Quota::Limited);

    // Removes specified key or array of keys. Returns a changes map { key: { oldValue } }.
    JsonObject remove(StringView extension_id, Vector<String> const& keys);

    // Clears all keys for `extension_id`. Returns a changes map { key: { oldValue } }.
    JsonObject clear(StringView extension_id);

    void delete_extension_data(StringView extension_id);

private:
    struct Statements {
        Database::StatementID get_item { 0 };
        Database::StatementID get_all { 0 };
        Database::StatementID get_size { 0 };
        Database::StatementID set_item { 0 };
        Database::StatementID delete_item { 0 };
        Database::StatementID clear_extension { 0 };
    };

    explicit ExtensionStorage(Optional<NonnullRefPtr<Database::Database>> database, Statements statements);

    Optional<String> read_raw(StringView extension_id, StringView key) const;
    ErrorOr<Optional<String>> try_read_raw(StringView extension_id, StringView key) const;
    HashMap<String, String> read_all_raw(StringView extension_id) const;
    ErrorOr<void> write_raw(StringView extension_id, StringView key, StringView json_value);
    void erase_raw(StringView extension_id, StringView key);
    void erase_all_raw(StringView extension_id);

    RefPtr<Database::Database> m_database;
    Statements m_statements;
    HashMap<String, HashMap<String, String>> m_memory_store;
};

}
