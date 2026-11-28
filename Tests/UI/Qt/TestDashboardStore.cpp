/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/NumericLimits.h>
#include <LibCore/File.h>
#include <LibDatabase/Database.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/DashboardStore.h>

#include <QTemporaryDir>

using Ladybird::DashboardStore;

TEST_CASE(dashboard_store_starter_layouts_and_empty_persistence)
{
    QTemporaryDir temp_dir;
    VERIFY(temp_dir.isValid());
    auto dir_path = ByteString { temp_dir.path().toUtf8().constData() };

    {
        auto store = DashboardStore::open_persistent(dir_path, true);
        EXPECT_EQ(store->storage_status(), DashboardStore::StorageStatus::Persistent);

        auto starter = store->load_or_initialize_layout("default"_string, false);
        EXPECT(!starter.initialized);
        EXPECT_EQ(starter.widgets.size(), 3uz);
        EXPECT_EQ(starter.widgets[0].widget_type, "world-clock"_string);
        EXPECT_EQ(starter.widgets[1].widget_type, "quick-tasks"_string);
        EXPECT_EQ(starter.widgets[2].widget_type, "scratchpad"_string);
        auto const wide = starter.placements.get("wide"_string).value();
        EXPECT_EQ(wide[0].row, -5);
        EXPECT_EQ(wide[2].row + wide[2].h, 5);
        auto const compact = starter.placements.get("compact"_string).value();
        EXPECT_EQ(compact[0].row, -14);
        EXPECT_EQ(compact[2].row + compact[2].h, 14);

        // Save an intentionally empty customized layout.
        HashMap<String, Vector<DashboardStore::Placement>> empty_placements;
        auto commit = store->commit_layout("default"_string, "op-empty-1"_string, 0, false, {}, empty_placements);
        EXPECT(commit.ok);
        EXPECT_EQ(commit.snapshot.layout_revision, 1);
        EXPECT(commit.snapshot.initialized);
        EXPECT(commit.snapshot.widgets.is_empty());

        // Duplicate op ID returns prior result without bumping revision.
        auto dup = store->commit_layout("default"_string, "op-empty-1"_string, 0, false, {}, empty_placements);
        EXPECT(dup.ok);
        EXPECT(dup.duplicate);
        EXPECT_EQ(dup.snapshot.layout_revision, 1);
    }

    // Reopen and verify saved empty layout survives restart (does NOT re-seed starter).
    {
        auto store = DashboardStore::open_persistent(dir_path, true);
        auto reloaded = store->load_or_initialize_layout("default"_string, false);
        EXPECT(reloaded.initialized);
        EXPECT_EQ(reloaded.layout_revision, 1);
        EXPECT(reloaded.widgets.is_empty());
    }
}

TEST_CASE(dashboard_store_migrates_saved_placements_to_finer_grid)
{
    QTemporaryDir temp_dir;
    VERIFY(temp_dir.isValid());
    auto dir_path = ByteString { temp_dir.path().toUtf8().constData() };

    {
        // Re-create a version 1 profile with the old placement units and no image table.
        auto db = MUST(Database::Database::create(dir_path, "Dashboard"sv));
        MUST(DashboardStore::migrate_schema(*db));
        MUST(db->execute_raw(R"#(
            INSERT INTO DashboardMeta VALUES ('default', 7, 1, 0);
            INSERT INTO DashboardWidgets VALUES ('default', 'clock', 'world-clock', 1, '{}');
            INSERT INTO DashboardPlacements VALUES ('default', 'wide', 'clock', 5, 510, 3, 2);
            INSERT INTO DashboardPlacements VALUES ('default', 'compact', 'clock', 0, 1, 2, 2);
            DROP TABLE DashboardImages;
            UPDATE SchemaVersions SET version = 1 WHERE store = 'Dashboard';
        )#"sv));
    }

    for (int open = 0; open < 2; ++open) {
        auto store = DashboardStore::open_persistent(dir_path, true);
        EXPECT_EQ(store->storage_status(), DashboardStore::StorageStatus::Persistent);
        auto layout = store->load_or_initialize_layout("default"_string, false);
        EXPECT(layout.initialized);
        EXPECT_EQ(layout.layout_revision, 7);
        auto const wide = layout.placements.get("wide"_string).value().first();
        EXPECT_EQ(wide.col, 20);
        EXPECT_EQ(wide.row, -4);
        EXPECT_EQ(wide.w, 12);
        EXPECT_EQ(wide.h, 8);
        auto const compact = layout.placements.get("compact"_string).value().first();
        EXPECT_EQ(compact.col, 0);
        EXPECT_EQ(compact.row, -4);
        EXPECT_EQ(compact.w, 8);
        EXPECT_EQ(compact.h, 8);
    }
}

TEST_CASE(dashboard_store_migrates_saved_rows_relative_to_the_viewport_center)
{
    QTemporaryDir temp_dir;
    VERIFY(temp_dir.isValid());
    auto dir_path = ByteString { temp_dir.path().toUtf8().constData() };

    {
        auto db = MUST(Database::Database::create(dir_path, "Dashboard"sv));
        MUST(DashboardStore::migrate_schema(*db));
        MUST(db->execute_raw(R"#(
            INSERT INTO DashboardMeta VALUES ('default', 5, 1, 0), ('workspace', 2, 1, 0);
            INSERT INTO DashboardWidgets VALUES
                ('default', 'clock', 'world-clock', 1, '{}'),
                ('default', 'tasks', 'quick-tasks', 1, '{}'),
                ('workspace', 'clock', 'world-clock', 1, '{}');
            INSERT INTO DashboardPlacements VALUES
                ('default', 'wide', 'clock', 4, 3, 12, 8),
                ('default', 'wide', 'tasks', 4, 16, 12, 10),
                ('default', 'compact', 'clock', 0, 40, 8, 12),
                ('workspace', 'wide', 'clock', 0, 0, 12, 10);
            DROP TABLE DashboardImages;
            UPDATE SchemaVersions SET version = 2 WHERE store = 'Dashboard';
        )#"sv));
    }

    for (int open = 0; open < 2; ++open) {
        auto store = DashboardStore::open_persistent(dir_path, true);
        EXPECT_EQ(store->storage_status(), DashboardStore::StorageStatus::Persistent);
        auto layout = store->load_or_initialize_layout("default"_string, false);
        EXPECT_EQ(layout.layout_revision, 5);
        auto const wide = layout.placements.get("wide"_string).value();
        EXPECT_EQ(wide.size(), 2uz);
        EXPECT_EQ(wide[0].instance_id, "clock"_string);
        EXPECT_EQ(wide[0].row, -11);
        EXPECT_EQ(wide[1].instance_id, "tasks"_string);
        EXPECT_EQ(wide[1].row, 2);
        EXPECT_EQ(wide[1].row - (wide[0].row + wide[0].h), 5);
        EXPECT_EQ(wide[0].col, 4);
        EXPECT_EQ(wide[0].w, 12);
        EXPECT_EQ(layout.placements.get("compact"_string).value().first().row, -6);
        auto workspace = store->load_or_initialize_layout("workspace"_string, false);
        EXPECT_EQ(workspace.placements.get("wide"_string).value().first().row, -5);
    }
}

TEST_CASE(dashboard_store_ephemeral_starter_and_conflict_validation)
{
    auto store = DashboardStore::create_ephemeral();
    EXPECT_EQ(store->storage_status(), DashboardStore::StorageStatus::SessionOnly);

    auto starter = store->load_or_initialize_layout("ephemeral:1"_string, true);
    EXPECT_EQ(starter.widgets.size(), 2uz);
    EXPECT_EQ(starter.widgets[0].widget_type, "world-clock"_string);
    EXPECT_EQ(starter.widgets[1].widget_type, "calculator"_string);
    auto const wide = starter.placements.get("wide"_string).value();
    EXPECT_EQ(wide[0].row, -6);
    EXPECT_EQ(wide[1].row + wide[1].h, 6);
    auto const compact = starter.placements.get("compact"_string).value();
    EXPECT_EQ(compact[0].row, -10);
    EXPECT_EQ(compact[1].row + compact[1].h, 10);

    // Reject overlapping placements.
    HashMap<String, Vector<DashboardStore::Placement>> bad_overlap;
    bad_overlap.set("wide"_string, {
                                       { starter.widgets[0].instance_id, 0, 0, 3, 2 },
                                       { starter.widgets[1].instance_id, 2, 1, 3, 2 },
                                   });
    auto bad_res = store->commit_layout("ephemeral:1"_string, "op-bad"_string, 0, false, starter.widgets, bad_overlap);
    EXPECT(!bad_res.ok);
    EXPECT(!bad_res.conflict);

    // Valid commit succeeds, then stale revision triggers conflict.
    auto good_res = store->commit_layout("ephemeral:1"_string, "op-good"_string, 0, true, starter.widgets, starter.placements);
    EXPECT(good_res.ok);
    EXPECT_EQ(good_res.snapshot.layout_revision, 1);

    auto stale_res = store->commit_layout("ephemeral:1"_string, "op-stale"_string, 0, false, starter.widgets, starter.placements);
    EXPECT(!stale_res.ok);
    EXPECT(stale_res.conflict);
    EXPECT_EQ(stale_res.snapshot.layout_revision, 1);
}

TEST_CASE(dashboard_store_enforces_row_offsets_and_placement_height)
{
    auto instance_id = "clock"_string;
    HashTable<String> valid_instance_ids;
    valid_instance_ids.set(instance_id);
    auto is_valid = [&](i64 row, i64 h) {
        return !DashboardStore::validate_placements("wide"sv, { { instance_id, 0, row, 1, h } }, valid_instance_ids).is_error();
    };

    EXPECT(is_valid(DashboardStore::MIN_GRID_ROW, DashboardStore::MAX_GRID_ROWS));
    EXPECT(is_valid(DashboardStore::MAX_GRID_ROWS - 1, 1));
    EXPECT(!is_valid(DashboardStore::MIN_GRID_ROW - 1, 1));
    EXPECT(!is_valid(DashboardStore::MAX_GRID_ROWS, 1));
    EXPECT(!is_valid(DashboardStore::MIN_GRID_ROW, DashboardStore::MAX_GRID_ROWS + 1));
    EXPECT(!is_valid(DashboardStore::MIN_GRID_ROW, NumericLimits<i64>::max()));
    EXPECT(!is_valid(NumericLimits<i64>::min(), 1));
    EXPECT(!is_valid(NumericLimits<i64>::max(), 1));
}

TEST_CASE(dashboard_store_personal_item_conflicts_and_note_limit)
{
    auto store = DashboardStore::create_ephemeral();
    JsonObject note1;
    note1.set("title"sv, "Scratch"_string);
    note1.set("content"sv, "Hello v1"_string);

    auto r1 = store->save_item("default"_string, "notes"_string, "note-1"_string, i64 { 0 }, note1);
    EXPECT(r1.ok);
    EXPECT_EQ(r1.revision, 1);

    // Conflicting edit using stale revision 0 fails and returns currentValue.
    JsonObject note_conflict;
    note_conflict.set("title"sv, "Scratch"_string);
    note_conflict.set("content"sv, "Conflicting draft"_string);
    auto r_conflict = store->save_item("default"_string, "notes"_string, "note-1"_string, i64 { 0 }, note_conflict);
    EXPECT(!r_conflict.ok);
    EXPECT(r_conflict.conflict);
    EXPECT_EQ(r_conflict.revision, 1);
    EXPECT(r_conflict.current_value.has_value());
    EXPECT_EQ(r_conflict.current_value->get_string("content"sv).value_or(String {}), "Hello v1"_string);

    // Reject note exceeding 64 KiB.
    StringBuilder big;
    for (size_t i = 0; i < DashboardStore::MAX_NOTE_BYTES + 10; ++i)
        big.append('a');
    JsonObject oversized;
    oversized.set("title"sv, "Huge"_string);
    oversized.set("content"sv, MUST(big.to_string()));
    auto r_big = store->save_item("default"_string, "notes"_string, "note-big"_string, i64 { 0 }, oversized);
    EXPECT(!r_big.ok);
}

TEST_CASE(dashboard_store_image_copies_persist_and_are_scope_bound)
{
    QTemporaryDir temp_dir;
    VERIFY(temp_dir.isValid());
    auto dir_path = ByteString { temp_dir.path().toUtf8().constData() };
    auto const regular_scope = "default"_string;
    auto const other_scope = "workspace"_string;
    auto const instance_id = "picture"_string;
    static constexpr Array<u8, 5> picture { 0x89, 0x50, 0, 0xFF, 0x0A };
    static constexpr Array<u8, 3> replacement { 0xFF, 0xD8, 0xFF };

    {
        auto store = DashboardStore::open_persistent(dir_path, true);
        HashMap<String, Vector<DashboardStore::Placement>> placements;
        placements.set("wide"_string, { { instance_id, 2, -4, 8, 8 } });
        Vector<DashboardStore::WidgetInstance> widgets { { instance_id, "image"_string, 1, {} } };
        VERIFY(store->commit_layout(regular_scope, "add-image"_string, 0, false, widgets, placements).ok);
        EXPECT(!MUST(store->load_image(regular_scope, instance_id)).has_value());
        EXPECT(store->load_image(other_scope, instance_id).is_error());
        auto source_bytes = MUST(ByteBuffer::copy(picture.span()));
        MUST(store->save_image(regular_scope, instance_id, "image/png"sv, source_bytes.bytes()));
        source_bytes[0] = 0;

        VERIFY(store->commit_layout(other_scope, "add-image"_string, 0, false, widgets, placements).ok);
        EXPECT(!MUST(store->load_image(other_scope, instance_id)).has_value());
        MUST(store->save_image(other_scope, instance_id, "image/jpeg"sv, replacement.span()));
    }

    auto store = DashboardStore::open_persistent(dir_path, true);
    auto image = MUST(store->load_image(regular_scope, instance_id)).release_value();
    EXPECT_EQ(image.mime_type, "image/png"_string);
    EXPECT(image.bytes.bytes() == picture.span());
    auto other_image = MUST(store->load_image(other_scope, instance_id)).release_value();
    EXPECT_EQ(other_image.mime_type, "image/jpeg"_string);
    EXPECT(other_image.bytes.bytes() == replacement.span());

    MUST(store->save_image(regular_scope, instance_id, "image/jpeg"sv, replacement.span()));
    EXPECT(store->save_image(regular_scope, instance_id, "image/svg+xml"sv, picture.span()).is_error());
    image = MUST(store->load_image(regular_scope, instance_id)).release_value();
    EXPECT_EQ(image.mime_type, "image/jpeg"_string);
    EXPECT(image.bytes.bytes() == replacement.span());
    MUST(store->remove_image(regular_scope, instance_id));
    EXPECT(!MUST(store->load_image(regular_scope, instance_id)).has_value());
    EXPECT(MUST(store->load_image(other_scope, instance_id)).has_value());
}

TEST_CASE(dashboard_store_purges_images_with_removed_widgets)
{
    auto store = DashboardStore::create_ephemeral();
    auto const scope = "default"_string;
    auto const instance_id = "picture"_string;
    static constexpr Array<u8, 3> bytes { 0xFF, 0xD8, 0xFF };
    HashMap<String, Vector<DashboardStore::Placement>> placements;
    placements.set("wide"_string, { { instance_id, 0, -4, 8, 8 } });
    Vector<DashboardStore::WidgetInstance> widgets { { instance_id, "image"_string, 1, {} } };
    VERIFY(store->commit_layout(scope, "add-image"_string, 0, false, widgets, placements).ok);
    MUST(store->save_image(scope, instance_id, "image/jpeg"sv, bytes.span()));
    VERIFY(store->commit_layout(scope, "move-image"_string, 1, false, widgets, placements).ok);
    EXPECT(MUST(store->load_image(scope, instance_id)).has_value());

    VERIFY(store->commit_layout(scope, "remove-image"_string, 2, false, {}, {}).ok);
    EXPECT(store->save_image(scope, instance_id, "image/jpeg"sv, bytes.span()).is_error());
    EXPECT(store->remove_image(scope, instance_id).is_error());
    VERIFY(store->commit_layout(scope, "restore-image"_string, 3, false, widgets, placements).ok);
    EXPECT(!MUST(store->load_image(scope, instance_id)).has_value());

    MUST(store->save_image(scope, instance_id, "image/jpeg"sv, bytes.span()));
    widgets[0].widget_type = "world-clock"_string;
    VERIFY(store->commit_layout(scope, "change-type"_string, 4, false, widgets, placements).ok);
    EXPECT(store->load_image(scope, instance_id).is_error());
    widgets[0].widget_type = "image"_string;
    VERIFY(store->commit_layout(scope, "restore-type"_string, 5, false, widgets, placements).ok);
    EXPECT(!MUST(store->load_image(scope, instance_id)).has_value());

    MUST(store->save_image(scope, instance_id, "image/jpeg"sv, bytes.span()));
    VERIFY(store->delete_scope_data(scope));
    EXPECT(store->load_image(scope, instance_id).is_error());
    VERIFY(store->commit_layout(scope, "add-after-reset"_string, 0, false, widgets, placements).ok);
    EXPECT(!MUST(store->load_image(scope, instance_id)).has_value());
}

TEST_CASE(dashboard_store_caps_image_storage)
{
    auto store = DashboardStore::create_ephemeral();
    auto const scope = "default"_string;
    auto const image_count_at_limit = DashboardStore::MAX_SCOPE_IMAGE_BYTES / DashboardStore::MAX_IMAGE_BYTES;
    Vector<DashboardStore::WidgetInstance> widgets;
    Vector<DashboardStore::Placement> wide;
    for (size_t i = 0; i <= image_count_at_limit; ++i) {
        auto id = MUST(String::formatted("image-{}", i));
        widgets.append({ id, "image"_string, 1, {} });
        wide.append({ id, static_cast<i64>(i), 0, 1, 1 });
    }
    HashMap<String, Vector<DashboardStore::Placement>> placements;
    placements.set("wide"_string, move(wide));
    VERIFY(store->commit_layout(scope, "add-images"_string, 0, false, widgets, placements).ok);
    auto image_bytes = MUST(ByteBuffer::create_zeroed(DashboardStore::MAX_IMAGE_BYTES + 1));
    EXPECT(store->save_image(scope, widgets.first().instance_id, "image/png"sv, image_bytes.bytes()).is_error());
    EXPECT(!MUST(store->load_image(scope, widgets.first().instance_id)).has_value());

    auto maximum_size_image = image_bytes.bytes().slice(0, DashboardStore::MAX_IMAGE_BYTES);
    for (size_t i = 0; i < image_count_at_limit; ++i)
        MUST(store->save_image(scope, widgets[i].instance_id, "image/png"sv, maximum_size_image));
    EXPECT(store->save_image(scope, widgets.last().instance_id, "image/png"sv, maximum_size_image).is_error());
    MUST(store->save_image(scope, widgets.first().instance_id, "image/png"sv, maximum_size_image));
    MUST(store->remove_image(scope, widgets.first().instance_id));
    MUST(store->save_image(scope, widgets.last().instance_id, "image/png"sv, maximum_size_image));
}

TEST_CASE(dashboard_store_fallback_preserves_corrupt_and_newer_databases)
{
    // Disabled SQL falls back to memory without creating a file.
    QTemporaryDir temp_disabled;
    VERIFY(temp_disabled.isValid());
    auto disabled_dir = ByteString { temp_disabled.path().toUtf8().constData() };
    {
        auto store = DashboardStore::open_persistent(disabled_dir, false);
        EXPECT_EQ(store->storage_status(), DashboardStore::StorageStatus::FallbackDisabledSQL);
        EXPECT(!FileSystem::exists(ByteString::formatted("{}/Dashboard.db", disabled_dir)));
    }

    // Corrupt file is preserved untouched on disk while session falls back to memory.
    QTemporaryDir temp_corrupt;
    VERIFY(temp_corrupt.isValid());
    auto corrupt_dir = ByteString { temp_corrupt.path().toUtf8().constData() };
    auto corrupt_path = ByteString::formatted("{}/Dashboard.db", corrupt_dir);
    {
        auto file = MUST(Core::File::open(corrupt_path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted("not a valid sqlite database payload"sv.bytes()));
    }
    {
        auto store = DashboardStore::open_persistent(corrupt_dir, true);
        EXPECT_EQ(store->storage_status(), DashboardStore::StorageStatus::FallbackCorrupt);
        auto file = MUST(Core::File::open(corrupt_path, Core::File::OpenMode::Read));
        auto content = MUST(file->read_until_eof());
        EXPECT_EQ(StringView { content.bytes() }, "not a valid sqlite database payload"sv);
    }

    // Newer schema version is preserved untouched on disk while session falls back to memory.
    QTemporaryDir temp_newer;
    VERIFY(temp_newer.isValid());
    auto newer_dir = ByteString { temp_newer.path().toUtf8().constData() };
    {
        auto db = MUST(Database::Database::create(newer_dir, "Dashboard"sv));
        MUST(DashboardStore::migrate_schema(*db));
        MUST(db->execute_raw("UPDATE SchemaVersions SET version = 999 WHERE store = 'Dashboard';"sv));
    }
    {
        auto store = DashboardStore::open_persistent(newer_dir, true);
        EXPECT_EQ(store->storage_status(), DashboardStore::StorageStatus::FallbackNewerSchema);
    }
}

TEST_CASE(dashboard_store_network_grants_and_delete_scope_data)
{
    auto store = DashboardStore::create_ephemeral();
    JsonObject cfg;
    cfg.set("url"sv, "https://example.org/feed.xml"_string);
    EXPECT(store->upsert_network_grant("default"_string, "feed:https://example.org/feed.xml"_string, "feed"_string, 1, cfg));
    EXPECT_EQ(store->list_network_grants("default"_string).size(), 1uz);

    JsonObject cache;
    cache.set("title"sv, "Example Feed"_string);
    EXPECT(store->update_network_cache("default"_string, "feed:https://example.org/feed.xml"_string, cache, 999999, 123456));
    auto grant = store->get_network_grant("default"_string, "feed:https://example.org/feed.xml"_string);
    EXPECT(grant.has_value());
    EXPECT_EQ(grant->last_success_at, 123456);

    EXPECT(store->delete_scope_data("default"_string));
    EXPECT(store->list_network_grants("default"_string).is_empty());
    auto after_delete = store->load_or_initialize_layout("default"_string, false);
    EXPECT(!after_delete.initialized);
}

TEST_CASE(dashboard_store_image_shader_library_is_bounded_and_scoped)
{
    QTemporaryDir temp_dir;
    VERIFY(temp_dir.isValid());
    auto dir_path = ByteString { temp_dir.path().toUtf8().constData() };
    auto collection = MUST(String::from_utf8(DashboardStore::IMAGE_SHADER_COLLECTION));
    auto scope = "default"_string;
    auto other_scope = "workspace"_string;
    auto shader_id = "shader-test"_string;
    auto source = "vec4 shade(vec2 uv, vec4 color, float time) { return vec4(1.0 - color.rgb, color.a); }\n"_string;
    JsonObject shader;
    shader.set("name"sv, "Invert"_string);
    shader.set("source"sv, source);

    {
        auto store = DashboardStore::open_persistent(dir_path, true);
        EXPECT(store->save_item(scope, collection, shader_id, 0, shader).ok);
        EXPECT(!store->get_item(other_scope, collection, shader_id).has_value());

        auto invalid = shader;
        invalid.set("source"sv, "\n "_string);
        EXPECT(!store->save_item(scope, collection, "empty-source"_string, 0, invalid).ok);
        invalid.set("source"sv, MUST(String::repeated('a', DashboardStore::MAX_SHADER_BYTES + 1)));
        EXPECT(!store->save_item(scope, collection, "oversized-source"_string, 0, invalid).ok);

        for (size_t i = 1; i < DashboardStore::MAX_SHADER_FILES; ++i)
            EXPECT(store->save_item(scope, collection, MUST(String::formatted("shader-{}", i)), 0, shader).ok);
        EXPECT(!store->save_item(scope, collection, "shader-overflow"_string, 0, shader).ok);
        EXPECT(store->save_item(other_scope, collection, shader_id, 0, shader).ok);
    }

    auto store = DashboardStore::open_persistent(dir_path, true);
    auto saved = store->get_item(scope, collection, shader_id);
    VERIFY(saved.has_value());
    EXPECT_EQ(saved->value.get_string("source"sv).value(), source);
    EXPECT(store->delete_item(scope, collection, shader_id));
    EXPECT(!store->get_item(scope, collection, shader_id).has_value());
    EXPECT(store->save_item(scope, collection, "shader-replacement"_string, 0, shader).ok);
    EXPECT(store->delete_scope_data(scope));
    EXPECT(store->list_items(scope, collection).is_empty());
    EXPECT(store->get_item(other_scope, collection, shader_id).has_value());
}
