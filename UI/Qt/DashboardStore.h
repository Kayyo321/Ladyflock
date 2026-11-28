/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibDatabase/Database.h>

namespace Ladybird {

class DashboardStore {
    AK_MAKE_NONCOPYABLE(DashboardStore);
    AK_MAKE_NONMOVABLE(DashboardStore);

public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr size_t MAX_CARDS = 60;
    static constexpr i64 MAX_GRID_ROWS = 2048;
    static constexpr i64 MIN_GRID_ROW = -MAX_GRID_ROWS;
    static constexpr size_t MAX_NOTE_BYTES = 64 * 1024;
    static constexpr size_t MAX_ITEM_JSON_BYTES = 128 * 1024;
    static constexpr size_t MAX_COLLECTION_ITEMS = 500;
    static constexpr size_t MAX_SCOPE_CACHE_BYTES = 5 * 1024 * 1024;
    static constexpr size_t MAX_OP_DEDUP_ENTRIES = 64;
    static constexpr size_t MAX_IMAGE_BYTES = 8 * 1024 * 1024;
    static constexpr size_t MAX_SCOPE_IMAGE_BYTES = 64 * 1024 * 1024;
    static constexpr auto IMAGE_SHADER_COLLECTION = "image-shaders"sv;
    static constexpr size_t MAX_SHADER_BYTES = 16 * 1024;
    static constexpr size_t MAX_SHADER_NAME_BYTES = 240;
    static constexpr size_t MAX_SHADER_FILES = 24;

    enum class StorageStatus : u8 {
        Persistent,
        SessionOnly,
        FallbackDisabledSQL,
        FallbackCorrupt,
        FallbackNewerSchema,
        WriteFailed,
    };

    struct WidgetInstance {
        String instance_id;
        String widget_type;
        i64 config_version { 1 };
        JsonObject config;
    };

    struct Placement {
        String instance_id;
        i64 col { 0 };
        i64 row { 0 };
        i64 w { 1 };
        i64 h { 1 };
    };

    struct LayoutSnapshot {
        i64 layout_revision { 0 };
        bool initialized { false };
        bool compact_mode { false };
        Vector<WidgetInstance> widgets;
        HashMap<String, Vector<Placement>> placements;
    };

    struct PersonalItem {
        String item_id;
        i64 revision { 1 };
        JsonObject value;
        i64 updated_at { 0 };
    };

    struct NetworkGrant {
        String source_id;
        String kind;
        i64 consent_version { 1 };
        JsonObject config;
        JsonObject cache;
        i64 expires_at { 0 };
        i64 last_success_at { 0 };
    };

    struct Image {
        String mime_type;
        ByteBuffer bytes;
    };

    struct CommitResult {
        bool ok { false };
        bool conflict { false };
        bool duplicate { false };
        String error;
        LayoutSnapshot snapshot;
    };

    struct ItemSaveResult {
        bool ok { false };
        bool conflict { false };
        String error;
        i64 revision { 0 };
        Optional<JsonObject> current_value;
    };

    static ErrorOr<Database::MigrationOutcome> migrate_schema(Database::Database&, Database::MigrationMode = Database::MigrationMode::Apply);

    static NonnullOwnPtr<DashboardStore> open_persistent(ByteString const& data_directory, bool uses_sql_database);
    static NonnullOwnPtr<DashboardStore> create_ephemeral();

    ~DashboardStore();

    StorageStatus storage_status() const { return m_storage_status; }
    StringView storage_status_string() const;

    LayoutSnapshot load_or_initialize_layout(String const& scope_key, bool is_ephemeral_starter);
    CommitResult commit_layout(
        String const& scope_key,
        String const& op_id,
        i64 expected_revision,
        bool compact_mode,
        Vector<WidgetInstance> const& widgets,
        HashMap<String, Vector<Placement>> const& placements);

    Vector<PersonalItem> list_items(String const& scope_key, String const& collection, size_t limit = MAX_COLLECTION_ITEMS);
    Optional<PersonalItem> get_item(String const& scope_key, String const& collection, String const& item_id);
    ItemSaveResult save_item(
        String const& scope_key,
        String const& collection,
        String const& item_id,
        Optional<i64> expected_revision,
        JsonObject const& value);
    bool delete_item(String const& scope_key, String const& collection, String const& item_id);
    size_t delete_items_by_ids(String const& scope_key, String const& collection, Vector<String> const& item_ids);

    Vector<NetworkGrant> list_network_grants(String const& scope_key);
    Optional<NetworkGrant> get_network_grant(String const& scope_key, String const& source_id);
    bool upsert_network_grant(String const& scope_key, String const& source_id, String const& kind, i64 consent_version, JsonObject const& config);
    bool update_network_cache(String const& scope_key, String const& source_id, JsonObject const& cache, i64 expires_at, i64 last_success_at);
    bool revoke_network_grant(String const& scope_key, String const& source_id);

    ErrorOr<void> verify_image_widget(String const& scope_key, String const& instance_id);
    ErrorOr<Optional<Image>> load_image(String const& scope_key, String const& instance_id);
    ErrorOr<void> save_image(String const& scope_key, String const& instance_id, StringView mime_type, ReadonlyBytes bytes);
    ErrorOr<void> remove_image(String const& scope_key, String const& instance_id);

    bool delete_scope_data(String const& scope_key);

    static JsonObject serialize_layout_snapshot(LayoutSnapshot const&);
    static i64 columns_for_breakpoint(StringView breakpoint);
    static ErrorOr<void> validate_placements(StringView breakpoint, Vector<Placement> const& placements, HashTable<String> const& valid_instance_ids);

private:
    struct Statements {
        Database::StatementID get_meta { 0 };
        Database::StatementID upsert_meta { 0 };
        Database::StatementID list_widgets { 0 };
        Database::StatementID delete_widgets { 0 };
        Database::StatementID insert_widget { 0 };
        Database::StatementID list_placements { 0 };
        Database::StatementID delete_placements { 0 };
        Database::StatementID insert_placement { 0 };
        Database::StatementID list_items { 0 };
        Database::StatementID get_item { 0 };
        Database::StatementID count_items { 0 };
        Database::StatementID upsert_item { 0 };
        Database::StatementID delete_item { 0 };
        Database::StatementID delete_scope_meta { 0 };
        Database::StatementID delete_scope_items { 0 };
        Database::StatementID list_grants { 0 };
        Database::StatementID get_grant { 0 };
        Database::StatementID upsert_grant { 0 };
        Database::StatementID update_grant_cache { 0 };
        Database::StatementID delete_grant { 0 };
        Database::StatementID delete_scope_grants { 0 };
        Database::StatementID sum_cache_bytes { 0 };
        Database::StatementID verify_image_widget { 0 };
        Database::StatementID get_image { 0 };
        Database::StatementID upsert_image { 0 };
        Database::StatementID delete_image { 0 };
        Database::StatementID delete_scope_images { 0 };
        Database::StatementID delete_orphan_images { 0 };
        Database::StatementID sum_other_image_bytes { 0 };
    };

    DashboardStore(NonnullRefPtr<Database::Database>, Statements, StorageStatus);
    static ErrorOr<Statements> prepare_statements(Database::Database&);
    static NonnullOwnPtr<DashboardStore> create_memory_store(StorageStatus);

    LayoutSnapshot read_layout_snapshot(String const& scope_key);
    LayoutSnapshot build_starter_layout(bool is_ephemeral_starter) const;
    void record_committed_op(String const& scope_key, String const& op_id, CommitResult const&);

    NonnullRefPtr<Database::Database> m_database;
    Statements m_statements;
    StorageStatus m_storage_status { StorageStatus::SessionOnly };

    struct OpCacheEntry {
        String op_id;
        CommitResult result;
    };
    HashMap<String, Vector<OpCacheEntry>> m_recent_ops;
};

}
