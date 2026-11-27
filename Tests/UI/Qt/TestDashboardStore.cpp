/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/JsonObject.h>
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

TEST_CASE(dashboard_store_ephemeral_starter_and_conflict_validation)
{
    auto store = DashboardStore::create_ephemeral();
    EXPECT_EQ(store->storage_status(), DashboardStore::StorageStatus::SessionOnly);

    auto starter = store->load_or_initialize_layout("ephemeral:1"_string, true);
    EXPECT_EQ(starter.widgets.size(), 2uz);
    EXPECT_EQ(starter.widgets[0].widget_type, "world-clock"_string);
    EXPECT_EQ(starter.widgets[1].widget_type, "calculator"_string);

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
